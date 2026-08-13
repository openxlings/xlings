module xlings.core.xself.repair;

import std;
import xlings.core.log;
import xlings.platform;

namespace xlings::xself {

bool is_shell_safe_token(std::string_view s) {
    if (s.empty()) return false;
    if (s.front() == '-') return false;  // would be read as an option
    return std::ranges::all_of(s, [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_'
            || c == '+' || c == ':' || c == '@' || c == '/';
    });
}

std::string quiet_suffix() {
#if defined(_WIN32)
    return " >NUL 2>&1";
#else
    return " >/dev/null 2>&1";
#endif
}

bool probe_reinstallable(const std::string& target, const std::string& version, const CommandRunner& run, const std::string& client) {
    if (!is_shell_safe_token(target) || !is_shell_safe_token(version)) {
        return false;
    }
    return run(std::format("{} info {}@{}{}",
                           client, target, version, quiet_suffix())) == 0;
}

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

RepairResult repair_one(const RepairTask& task, const RepairPolicy& policy, const CommandRunner& run, const RemovalVerifier& removalDone) {
    const auto coordinate = task.coordinate.empty()
        ? std::format("{}@{}", task.target, task.version)
        : task.coordinate;
    if (task.coordinate.empty()
        ? (!is_shell_safe_token(task.target)
           || !is_shell_safe_token(task.version))
        : !is_shell_safe_token(task.coordinate)) {
        return {false, "none",
                "refusing to repair: the recorded name or version contains "
                "characters that are not safe to pass to a shell"};
    }
    if (!policy.allowNetwork) {
        return {false, "none", "skipped: repairs that may reach the network "
                               "are disabled for this pass"};
    }

    const auto install = std::format("{} install {} -y",
                                     policy.client, coordinate);
    const auto remove  = std::format("{} remove {} -y",
                                     policy.client, coordinate);

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
    // The command exited 0. Two independent questions follow, and the order
    // they are asked in is the whole safety property.
    //
    // WHETHER the install runs must not depend on the verifier. It used to,
    // and returning early on "the records survived" performed the destructive
    // half of remove-and-reinstall while skipping the half that puts it back.
    // Measured on a real home, that uninstalled a working `musl-gcc`: its
    // recipe names targets the release does not own, those are skipped, the
    // finding's entry therefore outlived the removal, and the ladder read that
    // as a reason not to reinstall the package it had just taken out.
    //
    // WHAT IS REPORTED does depend on the verifier, because "REMOVED but could
    // not reinstall" is a claim about the user's disk. `xlings remove` exits 0
    // when it merely detaches this subos (another subos still references the
    // version), when the recipe names targets outside the selection, and when
    // the payload is already gone -- in none of those was anything removed,
    // and saying so is the message a user is most likely to act on.
    const bool recordsSurvived =
        removalDone && !removalDone(task.target, task.version);
    const bool installed = run(install) == 0;

    if (installed && !recordsSurvived) return {true, "reinstall", {}};

    if (recordsSurvived) {
        return {false, installed ? "reinstall" : "none",
                std::format(
                    "`remove` exited 0 without dropping {} — it is still "
                    "registered{}. Removal exits 0 when it only detaches this "
                    "subos (another subos still references the version), when "
                    "the recipe names targets the release does not own, and "
                    "when the payload is already gone; none of those clears "
                    "the record this repair needs cleared. Take it out where "
                    "it lives (`xlings subos use <name>` then `xlings remove "
                    "{}`) and rerun",
                    coordinate,
                    installed ? ", and the reinstall that followed did not "
                                "clear it either"
                              : " and the reinstall failed too",
                    coordinate)};
    }

    // Removed for real, and could not be put back. The one outcome that leaves
    // the user worse off than before the repair, so it is never folded into a
    // generic failure: name it and hand back the command that finishes the job.
    return {false, "reinstall",
            std::format("REMOVED but could not reinstall — run "
                        "`xlings install {}`", coordinate)};
}

void print_migration_hint_once(std::string_view recorded,
                               std::string_view running) {
    static bool shown = false;
    if (shown) return;
    if (!platform::supports_rewrite_output()) return;  // not a terminal
    auto hint = migration_hint(recorded, running);
    if (!hint) return;
    shown = true;
    log::info("{}", *hint);
}

}
