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
import xlings.core.xvm.shim_identity;

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
    // Already carrying the platform suffix, because they come back from
    // `compute_desired` applied to the project's own state -- one predicate,
    // two callers, so a project cannot contribute a name a subos would have
    // rejected. Re-suffixing is harmless (`shim_filename` is idempotent), but
    // the values ARE suffixed and code that assumed otherwise would be wrong
    // on Windows only.
    std::vector<std::string>  commands;
    bool                      readable { true };
};

// Files found in a bin directory, split by what they are (`shim_identity`).
struct ActualScan {
    std::set<std::string>     ours;      // current: the entry binary, or identical to it
    // Ours, but an older xlings build than the entry: what every Windows shim
    // became after an upgrade (#615). name -> handoff-capable.
    std::map<std::string, bool> stale;
    std::vector<std::string>  foreign;   // not an xlings build — reported, never touched
    std::vector<std::string>  unknown;   // could not be read — reported, never touched
};

// What a rebuild would change. Filenames, already suffixed.
struct TableDiff {
    std::vector<std::string>  toAdd;
    std::vector<std::string>  toRemove;
    // Stale shims whose NAME stays: relinked to the entry, not re-decided.
    std::vector<std::string>  toRepoint;
    std::vector<std::string>  foreign;
    std::vector<std::string>  unknown;

    [[nodiscard]] bool empty() const {
        return toAdd.empty() && toRemove.empty() && toRepoint.empty();
    }
};

// What a rebuild did change.
struct TableReport {
    std::vector<std::string>  added;
    std::vector<std::string>  removed;
    std::vector<std::string>  repointed;
    std::vector<std::string>  foreign;
    // name -> why. A locked file on Windows lands here rather than being
    // discarded: `ensure_subos_shims` returns a failure count for the same
    // reason (issue #473) — a repair that did not happen must not read as one
    // that did.
    std::vector<std::pair<std::string, std::string>> failed;

    [[nodiscard]] bool changed() const {
        return !added.empty() || !removed.empty() || !repointed.empty();
    }
};

// The desired table for one subos.
//
// A name belongs in the table when this subos has an active version of it and
// that version's effective kind is "program". Lib and files packages are
// registered in xvm for version management by design; nothing execs them.
//
// Deliberately NOT gated on the payload being intact.
//
// An earlier draft probed `resolve_executable` here, reasoning that the table
// should only carry names dispatch can serve. That is the wrong place to ask.
// A package whose payload is broken, or whose layout the probe does not cover,
// would silently leave the table -- and the user would get the shell's
// "command not found", or worse a passthrough to the host's copy of the same
// name, instead of dispatch's `executable 'X' not found` with the path and the
// reinstall command. A broken payload is a runtime fact that dispatch reports
// well and `self doctor` already finds; dropping the name is strictly less
// information.
//
// The virtual-root case (#452) that motivated the probe is handled where it
// belongs: registration withholds activation from an anchor-only release
// (registration.cpp, `anyRealUncontested`), so an anchor never reaches here
// with an active version. And measured on a real home, every one of the 23
// leaked names is excluded by the `active` clause alone -- the probe was
// never what caught them.
std::set<std::string> compute_desired(
    const VersionDB& db,
    const Workspace& active,
    const std::vector<ProjectContribution>& projects);

// Read a bin directory, classifying each file with `ShimClassifier`.
//
// File identity alone is NOT the predicate. It answers "is this the entry's
// file object", which on Windows every upgrade makes false for every shim on
// purpose -- so it read a whole home's shims as somebody else's files and
// left them running the previous client (#615). What makes a file ours is
// what it IS (an xlings build); identity only decides whether it is current.
ActualScan scan_actual(const fs::path& binDir, const fs::path& entryBinary);
// Same, sharing one classifier -- and its per-file-object cache -- across
// several directories.
ActualScan scan_actual(const fs::path& binDir, ShimClassifier& classifier);

// Pure. The whole rebuild decision, reachable from a unit test.
//
// `reserved` names the entry binary's own shims (`xlings`, plus whatever
// `self init` places). Protection is ONE-DIRECTIONAL: they are never removed,
// but they are added when the workspace has them active.
//
// Both directions matter. `ensure_subos_shims` places `xlings` in a home subos
// that never installed the package, and nothing in the workspace justifies
// that file -- without the removal guard the diff reads the entry binary's own
// link as stale and deletes it, which is how a home loses the one file every
// other shim points at. But `xlings` is also a real package: installing it
// into a subos must give that subos its shim, so the add side stays open. A
// project subos gets neither, because its workspace has no `xlings` and its
// bin is not on PATH for anything to dispatch through.
//
// A stale shim is ours, so it follows the same rule as a current one: kept
// when desired (relinked, `toRepoint`), removed when not. An unknown file is
// treated like a foreign one -- nothing that could not be read is replaced.
TableDiff plan_table(const std::set<std::string>& desired,
                     const ActualScan& actual,
                     const std::vector<std::string>& reserved);

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
