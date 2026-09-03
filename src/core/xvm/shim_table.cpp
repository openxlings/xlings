module;

module xlings.core.xvm.shim_table;

import std;

import xlings.core.log;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xself.init;
import xlings.platform;

namespace xlings::xvm {

namespace {

#if defined(_WIN32)
constexpr std::string_view kShimExt = ".exe";
#else
constexpr std::string_view kShimExt = "";
#endif

// Is `path` one of our shims — a link to the entry binary?
//
// `fs::equivalent` answers on both platforms at once: it follows symlinks
// (POSIX) and compares file identity (Windows hard links). A `fs::is_symlink`
// test would be false for every Windows shim we have ever written.
bool is_our_shim_(const std::filesystem::path& path,
                  const std::filesystem::path& entryBinary) {
    std::error_code ec;
    if (std::filesystem::equivalent(path, entryBinary, ec) && !ec) return true;
    // A dangling symlink is still ours: it points at the entry binary's path
    // even when that path is momentarily gone (mid-`self update`), and
    // `equivalent` fails on a broken link rather than answering.
    ec.clear();
    if (std::filesystem::is_symlink(path, ec) && !ec) {
        std::error_code rec;
        auto target = std::filesystem::read_symlink(path, rec);
        if (rec) return false;
        auto resolved = target.is_absolute()
            ? target
            : path.parent_path() / target;
        std::error_code nec;
        auto a = std::filesystem::weakly_canonical(resolved, nec);
        std::error_code bec;
        auto b = std::filesystem::weakly_canonical(entryBinary, bec);
        return !nec && !bec && a == b;
    }
    return false;
}

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
    ActualScan scan;
    std::error_code ec;
    if (!std::filesystem::exists(binDir, ec)) return scan;

    for (const auto& entry : platform::dir_entries(binDir)) {
        std::error_code fec;
        if (!entry.is_regular_file(fec) && !entry.is_symlink(fec)) continue;
        auto fname = entry.path().filename().string();
        if (is_our_shim_(entry.path(), entryBinary)) {
            scan.ours.insert(std::move(fname));
        } else {
            scan.foreign.push_back(std::move(fname));
        }
    }
    std::ranges::sort(scan.foreign);
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
        if (!actual.ours.contains(want)) diff.toAdd.push_back(want);
    }
    for (const auto& have : actual.ours) {
        if (protectedNames.contains(have)) continue;
        if (!desired.contains(have)) diff.toRemove.push_back(have);
    }
    // A foreign file whose name we also want is NOT ours to replace: the user
    // put a real binary there. Reported, never overwritten — so drop it from
    // toAdd rather than clobbering it.
    for (const auto& name : actual.foreign) {
        std::erase(diff.toAdd, name);
    }
    diff.foreign = actual.foreign;
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
