export module xlings.core.xself.repair;

import std;

// The repair ladder behind `xlings self doctor --fix`.
//
// Split out of doctor.cppm on purpose: detection and repair have different
// risk profiles, and in doctor they were tangled -- the `--fix` branches sat
// inline among the reporting, which is why the escalation below could not
// have been written there without making a 585-line function longer.
//
// doctor keeps ownership of WHAT IS WRONG and hands over RepairTasks; this
// module owns WHAT TO DO ABOUT IT. The commands are injected, so the whole
// decision table is unit-testable without a home, a network or a payload.
//
// See .agents/docs/2026-07-28-self-repair-design.md.
export namespace xlings::xself {

enum class RepairKind {
    // A record written by an older client that the current one cannot read
    // as what it is. The measured case: 0.4.69 records a headers-only
    // package as a single program-typed entry with a payload path and no
    // executable, which is indistinguishable from a program whose binary
    // vanished. Re-running config() re-registers it in the current form.
    StaleRegistration,
    // The registered payload does not resolve to an executable on disk.
    BrokenPayload,
};

struct RepairTask {
    RepairKind  kind { RepairKind::BrokenPayload };
    std::string target;
    std::string version;
    std::string detail;

    // Whether the index can supply this package again. Filled in by the
    // caller, which owns the catalog. Gates the destructive rung: removing
    // something that cannot be reinstalled is destruction, not repair.
    bool reinstallable { false };
};

struct RepairPolicy {
    // R2/R3 shell out to `xlings install`, which may reach the network.
    // A caller wanting a purely local pass turns this off.
    bool allowNetwork   { true };
    // R3 (remove + install) is the destructive rung.
    bool allowReinstall { true };
};

struct RepairResult {
    bool        healed { false };
    // Which rung did the work, for the report: "re-register" | "reinstall" |
    // "none". Never a bare bool: "it failed" and "it was not attempted" have
    // to be distinguishable in the output, and a bool cannot say which.
    std::string rung   { "none" };
    std::string note;
};

// Names reach us from the versions DB, which recipes write, and the commands
// below go through std::system -- a shell. Anything outside this set is
// refused rather than quoted: a package name with a shell metacharacter in it
// is not a repair case, it is a broken or hostile index entry, and running it
// through a quoting function would be trusting the same input twice.
bool is_shell_safe_token(std::string_view s) {
    if (s.empty()) return false;
    if (s.front() == '-') return false;  // would be read as an option
    return std::ranges::all_of(s, [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_'
            || c == '+' || c == ':' || c == '@' || c == '/';
    });
}

// Runs a command, returns its exit code. Injected so the ladder can be tested
// as the pure decision table it is.
using CommandRunner = std::function<int(const std::string&)>;

// Silences a probe. The probe's output is not part of doctor's report and
// would interleave with it.
std::string quiet_suffix() {
#if defined(_WIN32)
    return " >NUL 2>&1";
#else
    return " >/dev/null 2>&1";
#endif
}

// Can the index supply this package again?
//
// The gate on the destructive rung, and the reason it is a separate probe
// rather than inferred from R2's exit code: `xlings install` fails the same
// way for "not in the index" and for "the registration layer refused to
// overwrite this record", and those two want opposite answers. `xlings info`
// reads the catalog only -- local, no network -- and answers 0/1 cleanly.
bool probe_reinstallable(const std::string& target,
                         const std::string& version,
                         const CommandRunner& run) {
    if (!is_shell_safe_token(target) || !is_shell_safe_token(version)) {
        return false;
    }
    return run(std::format("xlings info {}@{}{}",
                           target, version, quiet_suffix())) == 0;
}

// The one-line nudge toward `self doctor --fix`.
//
// The home config records which xlings set it up. When that differs from the
// running binary, packages registered by the older client may still be in its
// format -- which is what the repair ladder exists to migrate. A string
// comparison: no network, and no extra file read, because the home config is
// already loaded by every command.
//
// Returns nothing when the versions agree, which is what makes the hint stop
// appearing after a successful --fix stamps the field.
std::optional<std::string> migration_hint(std::string_view recorded,
                                          std::string_view running) {
    auto strip_v = [](std::string_view s) {
        return (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
            ? s.substr(1) : s;
    };
    const auto a = strip_v(recorded);
    const auto b = strip_v(running);
    // An absent record is not a mismatch. A home too old to carry the field
    // at all, or one written by a build that never set it, would otherwise
    // nag forever with nothing to compare against.
    if (a.empty() || b.empty() || a == b) return std::nullopt;
    return std::format(
        "this home was set up by {}; packages installed then may still be "
        "registered in its format\n  run  xlings self doctor --fix", a);
}

// One ladder, one (target, version). Rungs are tried in order and only if the
// previous one failed:
//
//   R2 re-register  `xlings install <pkg>@<ver> -y`
//                   Cheap and the usual answer. The installer's xvm-DB
//                   shortcut checks the payload on disk first, so with the
//                   payload intact this re-runs config() only -- no download
//                   -- and that is exactly what converts a 0.4.69 record into
//                   the current format.
//
//   R3 reinstall    `xlings remove …` then `xlings install …`
//                   For the case R2 cannot reach: a record the registration
//                   layer REFUSES to overwrite (xvm-legacy-payload-mismatch).
//                   Destructive, gated on `reinstallable`, and reported
//                   specially when it removes successfully and then fails to
//                   install -- that is the one outcome that leaves the user
//                   worse off than before, and it must never be silent.
RepairResult repair_one(const RepairTask& task,
                        const RepairPolicy& policy,
                        const CommandRunner& run) {
    if (!is_shell_safe_token(task.target)
        || !is_shell_safe_token(task.version)) {
        return {false, "none",
                "refusing to repair: the recorded name or version contains "
                "characters that are not safe to pass to a shell"};
    }
    if (!policy.allowNetwork) {
        return {false, "none", "skipped: repairs that may reach the network "
                               "are disabled for this pass"};
    }

    const auto install = std::format("xlings install {}@{} -y",
                                     task.target, task.version);
    const auto remove  = std::format("xlings remove {}@{} -y",
                                     task.target, task.version);

    // R2
    if (run(install) == 0) return {true, "re-register", {}};

    // R3
    if (!policy.allowReinstall) {
        return {false, "none",
                "re-register failed; reinstall was not permitted"};
    }
    if (!task.reinstallable) {
        return {false, "none",
                "re-register failed, and the package is not available from "
                "the index — removing it could not be undone"};
    }
    if (run(remove) != 0) {
        return {false, "none",
                "re-register failed and the entry could not be removed"};
    }
    if (run(install) != 0) {
        // The bad outcome. Say it plainly and hand back the exact command.
        return {false, "reinstall",
                std::format("REMOVED but could not reinstall — run "
                            "`xlings install {}@{}`",
                            task.target, task.version)};
    }
    return {true, "reinstall", {}};
}

} // namespace xlings::xself
