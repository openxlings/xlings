module;

module xlings.core.xvm.shim_table;

import std;

import xlings.core.log;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.shim_identity;
import xlings.core.xself.init;
import xlings.platform;

namespace xlings::xvm {

namespace {

// Compile-time platform choice, expressed to the COMPILER rather than to the
// preprocessor: with `if constexpr` / a constexpr ternary both arms are still
// type-checked on every target, so a change that breaks the arm this build does
// not take fails here instead of in the other platform's CI run.
constexpr std::string_view kShimExt =
    platform::OS_NAME == "windows" ? ".exe" : "";

} // namespace

std::string shim_filename(std::string_view name) {
    std::string fn(name);
    if (!kShimExt.empty() && !fn.ends_with(kShimExt)) fn += kShimExt;
    return fn;
}

std::set<std::string> compute_desired(
    const VersionDB& db,
    const Workspace& active,
    const std::vector<ProjectContribution>& projects)
{
    std::set<std::string> desired;

    for (const auto& [target, version] : active) {
        if (version.empty()) continue;
        if (effective_kind_of(db, target, version) != "program") continue;

        if (get_vdata(db, target, version) == nullptr) continue;
        desired.insert(shim_filename(target));
    }

    for (const auto& project : projects) {
        if (!project.readable) continue;
        for (const auto& cmd : project.commands) {
            if (cmd.empty()) continue;
            desired.insert(shim_filename(cmd));
        }
    }

    return desired;
}

ActualScan scan_actual(const std::filesystem::path& binDir,
                       const std::filesystem::path& entryBinary) {
    ShimClassifier classifier(entryBinary);
    return scan_actual(binDir, classifier);
}

ActualScan scan_actual(const std::filesystem::path& binDir,
                       ShimClassifier& classifier) {
    ActualScan scan;
    std::error_code ec;
    if (!std::filesystem::exists(binDir, ec)) return scan;

    for (const auto& entry : platform::dir_entries(binDir)) {
        std::error_code fec;
        if (!entry.is_regular_file(fec) && !entry.is_symlink(fec)) continue;
        auto fname = entry.path().filename().string();

        // Displacement debris, not a routing entry.
        //
        // When a shim cannot be unlinked because it is running,
        // `displace_locked_file` renames it to `<name>.xlings.old[N]` on
        // Windows and schedules the OS to drop it at reboot. That leftover is
        // still a link to the entry binary, so without this it would scan as
        // ours, fail to match any desired name, be queued for removal, and be
        // renamed aside AGAIN -- `slang.exe.xlings.old.xlings.old`, growing a
        // suffix per rebuild. The platform layer owns these files; the table
        // does not see them.
        if (fname.find(".xlings.old") != std::string::npos) continue;

        const auto id = classifier.classify(entry.path());
        switch (id.state) {
            case ShimState::Current: scan.ours.insert(std::move(fname)); break;
            case ShimState::Stale:
                scan.stale.emplace(std::move(fname), id.handoffCapable);
                break;
            case ShimState::Foreign: scan.foreign.push_back(std::move(fname)); break;
            case ShimState::Unknown: scan.unknown.push_back(std::move(fname)); break;
        }
    }
    std::ranges::sort(scan.foreign);
    std::ranges::sort(scan.unknown);
    return scan;
}

TableDiff plan_table(const std::set<std::string>& desired,
                     const ActualScan& actual,
                     const std::vector<std::string>& reserved) {
    std::set<std::string> protectedNames;
    for (const auto& name : reserved) {
        protectedNames.insert(shim_filename(name));
    }

    TableDiff diff;
    // Protection is one-directional: never REMOVE a reserved name, but do add
    // it when the workspace actually has it active. `xlings` is a real package
    // -- installing it into a subos should give that subos its shim -- while
    // the file `ensure_subos_shims` places in a subos that never installed it
    // must survive a rebuild that has no workspace entry to justify it.
    for (const auto& want : desired) {
        if (!actual.ours.contains(want) && !actual.stale.contains(want)) {
            diff.toAdd.push_back(want);
        }
    }
    for (const auto& have : actual.ours) {
        if (protectedNames.contains(have)) continue;
        if (!desired.contains(have)) diff.toRemove.push_back(have);
    }
    // Stale is ours: the same keep/remove rule, with "keep" meaning relink.
    for (const auto& [have, handoff] : actual.stale) {
        if (desired.contains(have) || protectedNames.contains(have)) {
            diff.toRepoint.push_back(have);
        } else {
            diff.toRemove.push_back(have);
        }
    }
    // A foreign file whose name we also want is NOT ours to replace: the user
    // put a real binary there. Reported, never overwritten — so drop it from
    // toAdd rather than clobbering it.
    for (const auto& name : actual.foreign) {
        std::erase(diff.toAdd, name);
    }
    for (const auto& name : actual.unknown) {
        std::erase(diff.toAdd, name);
    }
    diff.foreign = actual.foreign;
    diff.unknown = actual.unknown;
    return diff;
}

TableReport apply_table(const TableDiff& diff,
                        const std::filesystem::path& binDir,
                        const std::filesystem::path& entryBinary) {
    TableReport report;
    report.foreign = diff.foreign;

    std::error_code ec;
    if (!diff.toAdd.empty()) {
        std::filesystem::create_directories(binDir, ec);
    }

    for (const auto& name : diff.toAdd) {
        auto dst = binDir / name;
        if (xself::create_shim(entryBinary, dst) == xself::LinkResult::Failed) {
            report.failed.emplace_back(name, "could not create");
            log::debug("[shim-table] create failed: {}", dst.string());
            continue;
        }
        report.added.push_back(name);
    }

    // Relink in place. `create_shim` frees the path first (renaming a running
    // image aside on Windows), so a stale shim that is executing right now is
    // replaced without disturbing the process running it.
    for (const auto& name : diff.toRepoint) {
        auto dst = binDir / name;
        if (xself::create_shim(entryBinary, dst) == xself::LinkResult::Failed) {
            report.failed.emplace_back(name, "could not relink");
            log::debug("[shim-table] relink failed: {}", dst.string());
            continue;
        }
        report.repointed.push_back(name);
    }

    for (const auto& name : diff.toRemove) {
        auto path = binDir / name;
        // Not a bare `fs::remove`. On Windows a shim that is currently
        // running cannot be unlinked, and the shim being removed is very
        // often `xlings.exe`'s own hard link. `displace_locked_file` renames
        // the occupant aside there and is a plain unlink on POSIX.
        if (!platform::displace_locked_file(path)) {
            report.failed.emplace_back(name, "in use");
            log::debug("[shim-table] remove failed (in use): {}", path.string());
            continue;
        }
        report.removed.push_back(name);
    }

    return report;
}

} // namespace xlings::xvm
