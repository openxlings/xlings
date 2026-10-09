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
    return std::format(" >{} 2>&1", platform::null_device);
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
    return std::format("set up by {}; last verified by {}", a, b);
}

RepairResult repair_one(const RepairTask& task, const RepairPolicy& policy, const CommandRunner& run, const RemovalVerifier& removalDone, const DependentsProvider& dependentsOf) {
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
    // --force: this remove is a MEANS to a reinstall, not the user's own
    // request to remove something. Without it, a recipe whose uninstall()
    // hook throws exits 1 -- but 2026.9.12's `remove` withdraws the version
    // DB entry, workspace binding and shim BEFORE running that hook, so the
    // record is already gone by the time the exit code says "failed". Read
    // literally, that exit code used to mean "could not be removed" and this
    // rung returned early: a false report, and the reinstall that should
    // have followed never ran. `--force` also makes the exit code itself
    // mostly moot for R3's purposes -- a hook failure now exits 0 under
    // force -- but the exit code is no longer trusted for control flow
    // either way; see below.
    const auto remove  = std::format("{} remove {} --force -y",
                                     policy.client, coordinate);

    // R2. Skipped for SweptPayload: that payload's directory is already
    // non-empty and already registered, so `xlings install` would exit 0
    // having found "nothing to do" and this rung would report the finding
    // healed while the files a sweep left beside the package's own stayed
    // exactly where they were. See RepairKind::SweptPayload.
    if (task.kind != RepairKind::SweptPayload && run(install) == 0) {
        return {true, "re-register", {}};
    }

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
    // Exit code intentionally not checked. `remove`'s exit code answers "did
    // everything about this removal go perfectly" (hook included), not "is
    // the record still there" -- and those are different questions even
    // under `--force`. What decides whether R3 proceeds and what it reports
    // is entirely below: run the install regardless, then ask the verifier.
    run(remove);
    // Two independent questions follow, and the order they are asked in is
    // the whole safety property.
    //
    // WHETHER the install runs must not depend on the verifier (nor, now,
    // on remove's exit code). It used to depend on the verifier, and
    // returning early on "the records survived" performed the destructive
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
                    "`remove --force` did not drop {} — it is still "
                    "registered{}. Removal is a no-op on the record when it "
                    "only detaches this subos (another subos still "
                    "references the version), when the recipe names targets "
                    "the release does not own, and when the payload is "
                    "already gone; none of those clears the record this "
                    "repair needs cleared. Take it out where it lives "
                    "(`xlings subos use <name>` then `xlings remove "
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
    std::vector<Dependent> dependents;
    if (dependentsOf) dependents = dependentsOf(task.target);
    return {false, "reinstall",
            std::format("REMOVED but could not reinstall — run "
                        "`xlings install {}`", coordinate),
            std::move(dependents)};
}

}
