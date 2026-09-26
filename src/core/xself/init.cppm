export module xlings.core.xself.init;

import std;

import xlings.core.config;
// Cross-version compat (currently: legacy alias cleanup, profile upgrade).
// See compact/xself.cppm — each compat lives in its own version sub-namespace.
import xlings.core.xself.compat;
// Generated at build time from config/shell/*.{sh,fish,ps1}; see mcpp.toml.
import xlings.core.xself.profile_resources;
import xlings.core.subos.manifest;
// The routing table's decision layer. Imported by the INTERFACE because the
// three writer functions below name its types; the pure/testable half stays
// in xvm, this module is only its binding to a home.
import xlings.core.xvm.types;
import xlings.core.xvm.shim_table;

namespace xlings::xself {

namespace fs = std::filesystem;

// Base shim names (always created).
//
// 0.4.8 collapsed to a single canonical entry point. Earlier releases also
// created shims for {xim, xvm, xinstall, xsubos, xself} as multicall
// aliases, but they were removed (see main.cpp's deprecated-alias path).
// One-shot cleanup of leftover symlinks is delegated to the compat module.
inline constexpr std::array<std::string_view, 1> SHIM_NAMES_BASE = {
    "xlings"
};

// Optional shims (created only when pkg_root/bin/<name> exists)
inline constexpr std::array<std::string_view, 0> SHIM_NAMES_OPTIONAL = {};

export enum class LinkResult { Symlink, Hardlink, Copy, Failed };

export bool is_builtin_shim(std::string_view name);
export bool is_bootstrap_home_root(const fs::path& root);
export fs::path xlings_binary_in_home(const fs::path& home_dir);
export LinkResult create_shim(const fs::path& source, const fs::path& target);
// Returns how many shims could not be created. NOT void: every result used to
// be discarded, so a shim that failed -- which on Windows is what a locked
// `xlings.exe` produces -- left the caller reporting success over a repair that
// did not happen (issue #473: "healed 3", then the same broken state).
export std::size_t ensure_subos_shims(const fs::path& target_bin_dir,
                                      const fs::path& shim_src,
                                      const fs::path& pkg_root);
export bool ensure_home_layout(const fs::path& home_dir);

// ─── The routing table's single writer ──────────────────────────────
//
// `xvm::shim_table` holds the decision (pure, unit-testable, no Config).
// These three bind it to this home: they are the only code that computes a
// subos's desired command names, and the only code that adds or removes a
// file in a subos bin directory.
//
// See .agents/docs/2026-09-03-project-shim-routing-vs-state-design.md.

// What each known project contributes, recomputed from the project's own
// state file. A project whose state file is gone or unparseable comes back
// with `readable == false` and contributes nothing — its command names leave
// every table on the next rebuild, which is how a deleted project is
// reclaimed without the user doing anything.
export std::vector<xvm::ProjectContribution> project_contributions();

// What a rebuild of `subos_dir`'s table would change. Read-only.
export xvm::TableDiff plan_shim_table(
        const fs::path& subos_dir,
        const xvm::Workspace& active,
        const xvm::VersionDB& db,
        const std::vector<xvm::ProjectContribution>& projects);

// Apply a plan. Callers must already hold the home's state lock: the table is
// derived from the workspace, and the two must not be written apart.
export xvm::TableReport apply_shim_table(const fs::path& subos_dir,
                                         const xvm::TableDiff& diff);

// Bring this home's routing tables up to date after a workspace change.
//
// The one call install / use / remove make. It syncs the scope that was just
// written, and — when that scope is a PROJECT — also the global active subos,
// because a project's own bin is never on PATH: its command names have to
// reach the directory that is. That second sync is what replaced
// `mirror_shim_to_global_bin`, and the difference is that the names now
// arrive as derived table entries with an owner and a reclamation path,
// rather than as files nobody records and nothing can remove.
//
// Best-effort by design: a failure here means a name is momentarily missing
// from PATH, not that the install did not happen. Failures are logged, and
// `self doctor` reports the drift.

// What a rebuild changed, summed over the scopes it touched. Returned so the
// callers that run on UPGRADE can say what they repaired -- a repair nobody
// can see is indistinguishable from one that did not happen.
export struct ShimSyncSummary {
    std::size_t added   {};
    std::size_t removed {};
    std::size_t repointed {};
    std::size_t refused {};   // scopes whose workspace could not be observed

    [[nodiscard]] bool changed() const {
        return added != 0 || removed != 0 || repointed != 0;
    }
};

export ShimSyncSummary sync_shim_tables();

// ─── Shims that run an older build than the entry (#615) ────────────
//
// Replacing the entry binary changes its FILE OBJECT on Windows (a running
// image cannot be overwritten in place), and every shim there is a hard link
// to the old one. These two find and fix that across EVERY bin directory that
// holds this home's shims -- all real subos and every known project's subos
// -- not just the scope a command wrote.
//
// That breadth is allowed because relinking is not a routing decision: no
// name is added or removed, the file behind a name is swapped for the file it
// was always meant to be. The name set stays `sync_shim_tables`'s alone.

export struct StaleShims {
    fs::path                  binDir;
    // Legacy builds: they run their own (old) dispatcher. The #615 state.
    std::vector<std::string>  legacy;
    // Newer builds: they hand off to the entry at startup, so they already
    // run the right code; relinking only saves a process.
    std::vector<std::string>  handoff;
    std::vector<std::string>  unknown;   // could not be read

    [[nodiscard]] bool empty() const {
        return legacy.empty() && handoff.empty();
    }
};

// Read-only. One entry per bin directory that has something to report.
export std::vector<StaleShims> find_stale_shims(const fs::path& home);

export struct RepointSummary {
    std::size_t repointed {};
    std::size_t swept     {};   // `*.xlings.old*` leftovers deleted
    std::vector<std::pair<fs::path, std::string>> failed;
    bool refused { false };     // could not take the state lock
};

// Relink every stale shim to the entry. Takes the state lock (re-entrant).
export RepointSummary repoint_stale_shims(const fs::path& home);

// A command that runs THIS home's entry binary, spelled so it can be pasted.
//
// Not `xlings ...`: on a home with stale shims the `xlings` on PATH is one of
// them, and the command would run the old client -- which does not know the
// repair it is being asked for. PowerShell needs `&` to run a quoted path.
export std::string entry_command(const fs::path& home, std::string_view args);

// Replace the entry binary with `payloadBinary`, then re-point every shim at
// it. The one way `use` and `install` switch the running client: whoever
// changes the entry's file object is the one that knows every hard-link shim
// just detached from it, so the repair lives with the change rather than
// with each caller remembering it.
export bool replace_entry_binary(const fs::path& payloadBinary,
                                 const fs::path& entry,
                                 std::string_view coordinate,
                                 std::string_view toVersion);

bool is_builtin_shim(std::string_view name);

bool is_bootstrap_home_root(const fs::path& root);

fs::path xlings_binary_in_home(const fs::path& home_dir);

// Unified shim creation: symlink (Unix) > hardlink > copy
LinkResult create_shim(const fs::path& source, const fs::path& target);

std::size_t ensure_subos_shims(const fs::path& target_bin_dir,
                               const fs::path& shim_src,
                               const fs::path& pkg_root);

bool ensure_home_layout(const fs::path& home_dir);

// `xlings self init` — bootstrap (or repair) the home directory layout.
// Thin wrapper over ensure_home_layout; kept here so the dispatcher in
// xself.cppm can route by command name without pulling Config into a
// separate translation unit.
export int cmd_init();

} // namespace xlings::xself
