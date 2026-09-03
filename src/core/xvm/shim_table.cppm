// The routing table: which command names a subos's bin directory carries.
//
// A shim file is a symlink/hardlink to the entry binary. It carries no
// version and no owner — the name is the whole of it. So the file can only
// ever mean ROUTING ("this name is dispatched through xlings"), never STATE
// ("this subos has that version active"). State lives in the subos's
// `.xlings.json` workspace and nowhere else.
//
// Before this module those two meanings shared one directory, written by two
// different scopes:
//
//   * `mirror_shim_to_global_bin` wrote routing entries into the GLOBAL bin
//     from a PROJECT-scope decision, because a project's own bin is never on
//     PATH and cmd.exe has no cd hook to put it there.
//   * install / use wrote state entries from a global-scope decision.
//
// and three readers each assumed a different one of them: shim dispatch and
// doctor's Check 2 read the file as state (so a routing entry errored outside
// its project), while doctor's `has_program_kind` filter skipped anything the
// GLOBAL versions DB did not know — which is exactly what a project-scope
// install writes. Measured on a real home: 23 such files, none reachable,
// none reported, none reclaimable.
//
// So: one writer, one meaning, and the table is rebuilt from the truth rather
// than audited against it. `plan_table` is pure so the whole decision is
// reachable from unit tests; only `scan_actual` / `apply_table` touch disk.
//
// See .agents/docs/2026-09-03-project-shim-routing-vs-state-design.md.
export module xlings.core.xvm.shim_table;

import std;

import xlings.core.xvm.types;

export namespace xlings::xvm {

namespace fs = std::filesystem;

// A command name plus the platform's executable suffix. The desired and the
// actual set must be spelled the same way or every entry reads as both
// missing and stale at once.
std::string shim_filename(std::string_view name);

// The entry binary's own names (`xlings`, plus whatever `self init` places)
// are never candidates for removal. The list lives in `xself::init`; callers
// pass it in rather than this module importing it, so that the xvm layer does
// not acquire a dependency on Config through `xself.init`, and so the whole
// decision stays reachable from a unit test with no home on disk.

// What one project contributes to every subos's table.
//
// Commands are recomputed from the project's own state file at rebuild time
// rather than cached in the home: a project's manifest records PACKAGE names,
// while which COMMAND names a package registers is only known after install.
// A cache would therefore have to be written at install time and would go
// stale the moment the project changed its dependencies — and the rebuild
// would then faithfully recreate names that no longer exist, which is the
// bug class this module exists to remove.
struct ProjectContribution {
    fs::path                  root;      // the project directory
    std::vector<std::string>  commands;  // bare names, no platform suffix
    bool                      readable { true };
};

// Files found in a bin directory, split by whether they are ours.
struct ActualScan {
    std::set<std::string>     ours;      // filenames that ARE entry-binary links
    std::vector<std::string>  foreign;   // everything else — reported, never touched
};

// What a rebuild would change. Filenames, already suffixed.
struct TableDiff {
    std::vector<std::string>  toAdd;
    std::vector<std::string>  toRemove;
    std::vector<std::string>  foreign;

    [[nodiscard]] bool empty() const {
        return toAdd.empty() && toRemove.empty();
    }
};

// What a rebuild did change.
struct TableReport {
    std::vector<std::string>  added;
    std::vector<std::string>  removed;
    std::vector<std::string>  foreign;
    // name -> why. A locked file on Windows lands here rather than being
    // discarded: `ensure_subos_shims` returns a failure count for the same
    // reason (issue #473) — a repair that did not happen must not read as one
    // that did.
    std::vector<std::pair<std::string, std::string>> failed;

    [[nodiscard]] bool changed() const {
        return !added.empty() || !removed.empty();
    }
};

// The desired table for one subos.
//
// A name belongs in the table when dispatch would find something to exec for
// it — the same question `shim_dispatch` asks at run time, so "there is a
// shim" and "running it works" cannot drift apart:
//
//   1. the subos has an active version for it, and
//   2. that version's effective kind is "program" (lib/files packages are
//      registered in xvm for version management by design, but nothing execs
//      them), and
//   3. the payload actually contains the executable.
//
// (3) is what separates a real program from a virtual root registered only so
// a release has something to bind to. Neither `kind` nor `path` nor
// `is_binding_root` can tell those apart — only the payload can (#452).
//
// `payloadHasExecutable` is injected so this stays testable without a store:
// callers pass a probe backed by `xvm::resolve_executable`.
std::set<std::string> compute_desired(
    const VersionDB& db,
    const Workspace& active,
    const std::vector<ProjectContribution>& projects,
    const std::vector<std::string>& reserved,
    const std::function<bool(const std::string& execName,
                            const std::string& payloadPath)>& payloadHasExecutable);

// Read a bin directory, classifying each file by whether it is one of our
// shims. `std::filesystem::equivalent` is the predicate on BOTH platforms:
// it follows symlinks (POSIX shims) and compares file identity (Windows
// hardlinks). Do not reach for `fs::is_symlink` — Windows shims are hard
// links, so a symlink test is false there for every shim we ever wrote, which
// is why `cleanup_legacy_alias_shims` has never once fired on Windows.
ActualScan scan_actual(const fs::path& binDir, const fs::path& entryBinary);

// Pure. The whole rebuild decision, reachable from a unit test.
TableDiff plan_table(const std::set<std::string>& desired,
                     const ActualScan& actual);

// Apply the diff. Adds and removals only — never a wipe-and-recreate.
//
// A full rebuild (what proto's `regen` and pyenv-win's `Rehash` do) leaves a
// window where PATH resolves nothing, and on Windows one occupied file fails
// the entire pass. A diff touches only what changed and is idempotent, so a
// partial failure converges on the next run.
TableReport apply_table(const TableDiff& diff,
                        const fs::path& binDir,
                        const fs::path& entryBinary);

} // namespace xlings::xvm
