// Cross-version compatibility shim collection.
//
// Each compat feature lives in its own `vX_Y_Z` sub-namespace so the
// version it dates from is visible at every call site, and so a clean
// removal is a one-shot operation:
//
//   1. Bump the codebase past the removal target.
//   2. Delete the matching `namespace vX_Y_Z { ... }` block in this file.
//   3. Rebuild — every reference to `xself::compat::vX_Y_Z::*` surfaces as
//      a hard build error. Delete the call and any surrounding
//      `COMPAT(X.Y.Z → drop in A.B.C)` marker comment. No grep needed.
//
// "Compat" here is a deliberately broad bucket: it covers both
//   * one-shot migrations  (legacy alias shim cleanup — really expires)
//   * permanent self-heal  (profile auto-upgrade — keeps paying off as
//                           the embedded resource version evolves)
// The `removal_target` line in each block tells you which is which.
export module xlings.core.xself.compat;

import std;
import xlings.platform;
import xlings.core.xself.profile_resources;

namespace xlings::xself::compat {

namespace fs = std::filesystem;

// ───────────────────────────────────────────────────────────────────────
// Internal: profile-version writer reused by v0_4_17::auto_upgrade_*.
// Kept private to this module because it's an implementation detail
// shared between init.cppm and the auto-upgrade hook.
// ───────────────────────────────────────────────────────────────────────

namespace detail_ {

inline std::string extract_profile_version(std::string_view text) {
    constexpr std::string_view marker = "# xlings-profile-version:";
    auto pos = text.find(marker);
    if (pos == std::string_view::npos) return {};
    auto value_start = pos + marker.size();
    while (value_start < text.size() &&
           (text[value_start] == ' ' || text[value_start] == '\t')) {
        ++value_start;
    }
    auto eol = text.find_first_of("\r\n", value_start);
    auto end = (eol == std::string_view::npos) ? text.size() : eol;
    auto value = std::string{text.substr(value_start, end - value_start)};
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

inline bool needs_profile_upgrade(const fs::path& path,
                                  std::string_view target_version) {
    if (!fs::exists(path)) return false;
    try {
        auto content = platform::read_file_to_string(path.string());
        return extract_profile_version(content) != target_version;
    } catch (...) {
        return false;
    }
}

} // namespace detail_

// =======================================================================
// COMPAT(0.4.8 → drop in 0.6.0)  removal_target: 0.6.0
//
// 0.4.8 collapsed the multicall surface to a single canonical entry point
// (`xlings`). The five legacy aliases — xim / xvm / xself / xsubos /
// xinstall — were sprayed into ~/.xlings/bin as symlinks-to-bootstrap by
// xlings ≤ 0.4.7 and are now migration artifacts.
//
// Everything in this block exists only to ease the upgrade path for users
// who still have those leftover shims on disk; once we no longer support
// jumping from ≤0.4.7 directly, the whole namespace can be deleted.
// =======================================================================
export namespace v0_4_8 {

// Names that older xlings versions sprayed into ~/.xlings/bin as multicall
// symlinks → bootstrap. Any leftover entry of these names is a migration
// artifact.
inline constexpr std::array<std::string_view, 5> LEGACY_ALIAS_NAMES = {
    "xim", "xvm", "xself", "xsubos", "xinstall"
};

// (alias name, suggested replacement command-line) — used only by main.cpp
// to print a helpful message when the user invokes a removed alias.
struct DeprecatedAlias {
    std::string_view name;
    std::string_view replacement;
};
inline constexpr std::array<DeprecatedAlias, 5> DEPRECATED_ALIASES = {{
    {"xim",      "xlings install/remove/search/list/info ..."},
    {"xvm",      "xlings use ..."},
    {"xself",    "xlings self ..."},
    {"xsubos",   "xlings subos ..."},
    {"xinstall", "xlings install ..."},
}};

// Safety predicate shared by cleanup_legacy_alias_shims and cmd_doctor:
// "is `path` a symlink whose target canonicalizes to `canonical_bootstrap`?".
// Both callers gate on this so they stay consistent on what counts as
// "safe to remove" — a user file that merely happens to share a name is
// never touched.
bool is_legacy_alias_symlink_to_bootstrap(
    const fs::path& path,
    const fs::path& canonical_bootstrap);

// Remove leftover legacy alias symlinks under `bin_dir`. Hooked into every
// path that re-pins the active bootstrap binary so the cleanup happens
// opportunistically — the user never needs to run a separate migration
// step. Callers:
//   * `xlings self init`            (init.cppm)
//   * `xlings install xlings --use` (xim/installer.cppm self-replace path)
//   * `xlings use xlings <ver>`     (xvm/commands.cppm self-replace path)
//   * `xlings self doctor --fix`    (doctor.cppm)
void cleanup_legacy_alias_shims(const fs::path& bin_dir,
                                const fs::path& bootstrap_path);

// `main.cpp` calls this with the argv[0] basename. Returns true after
// printing the migration error to stderr; main.cpp then exits with code 2.
bool report_deprecated_alias_if_match(std::string_view program_name);

} // namespace v0_4_8

// =======================================================================
// COMPAT(0.4.17 → permanent self-heal, no scheduled removal)
//
// 0.4.17 added shell-level subos switching (XLINGS_ACTIVE_SUBOS env), an
// auto-upgrading shell profile (write_or_upgrade_profile_), and a colored
// [xsubos:NAME] prompt marker — all delivered via the shipped
// xlings-profile.{sh,fish,ps1} files. New installs see them automatically;
// already-installed users only see them once `ensure_home_layout` runs,
// which `xlings update xlings` does NOT trigger (the xpkg recipe just
// extracts the new tarball + flips the xvm pointer).
//
// `v0_4_17::auto_upgrade_profiles_if_stale` is therefore wired into the
// xlings binary's startup so that the new binary, on its first run after
// a self-update, brings the on-disk profiles up to whatever
// `profile_resources::kVersion` it was compiled with. Idempotent — same
// version is a no-op, mismatched version overwrites with the new bytes
// (preserving any user comments below the marker line).
//
// Lives in compat.cppm because it's how we paper over the "old xlings's
// install path didn't propagate profile content" gap. Stays permanently:
// every time we bump kVersion in a future release, the same hook delivers
// the new profile to existing users without requiring a manual
// `xlings self init`. removal_target: never (or whenever profiles stop
// shipping content).
// =======================================================================
export namespace v0_4_17 {

// Cheap startup hook: if any of the three shell profiles have a version
// marker that differs from `profile_resources::kVersion`, rewrite them.
// Otherwise no-op (single small read per file). Safe to call on every
// xlings invocation — total cost on the unchanged path is well under 1 ms.
//
// Callers shouldn't pass a `home_dir` that doesn't exist; the function
// silently returns when the config/shell directory is missing (e.g.
// portable extracts that haven't been initialized yet).
void auto_upgrade_profiles_if_stale(const fs::path& home_dir);

} // namespace v0_4_17

// =======================================================================
// COMPAT(0.4.18 → drop in 0.6.0)  removal_target: 0.6.0
//
// 0.4.18 changed the subos `.xlings.json` workspace value type from a
// plain version string to an object with `{active, installed[]}` fields
// (the "C2 schema"). The new form lets each subos track its own opt-in
// version set independently of the global `versions` payload metadata,
// which in turn lets `xlings list / use / update` give a per-subos view
// (subsequent PR in the rollout plan) and lets the GC refcount honor
// inactive installs (the `installed[]` set holds versions that should
// keep their payloads pinned even when not currently active).
//
// The compat surface is in two places, both in the parser, NOT here:
//
//   1. `xvm::subos_workspace_from_json` (src/core/xvm/db.cppm) accepts
//      the legacy string value as form (1) and silently lifts it into
//      a SubosWorkspace with `installed[]` empty.
//   2. The save path in `Config::save_workspace` always emits the new
//      object form, so any subos file written by 0.4.18+ is immediately
//      in C2 form. Lazy migration: the first install / use / remove
//      after upgrade rewrites the file.
//
// There's no helper function to call here — the change is fully passive.
// This block exists only as documentation and as a removal anchor: when
// 0.6.0 lands and we no longer support direct upgrades from ≤0.4.17,
// rip out the form-(1) string branch in subos_workspace_from_json and
// delete this block.
// =======================================================================
export namespace v0_4_18 {

// Sentinel marker — referencing this from other modules will surface
// as a build error once this namespace is deleted, the same way the
// other compat blocks in this file work. No code currently references
// it; it exists to make the removal trace concrete.
inline constexpr std::string_view kSchemaForm = "subos workspace = {active, installed[]}";

} // namespace v0_4_18


// =======================================================================
// Implementations
// =======================================================================

namespace v0_4_8 {

bool is_legacy_alias_symlink_to_bootstrap(const fs::path& path,
                                          const fs::path& canonical_bootstrap);

void cleanup_legacy_alias_shims(const fs::path& bin_dir,
                                const fs::path& bootstrap_path);

bool report_deprecated_alias_if_match(std::string_view program_name);

} // namespace v0_4_8


namespace v0_4_17 {

void auto_upgrade_profiles_if_stale(const fs::path& home_dir);

} // namespace v0_4_17

} // namespace xlings::xself::compat
