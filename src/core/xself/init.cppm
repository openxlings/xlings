export module xlings.core.xself.init;

import std;

import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
// Cross-version compat (currently: legacy alias cleanup, profile upgrade).
// See compact/xself.cppm — each compat lives in its own version sub-namespace.
import xlings.core.xself.compat;
// Generated at build time from config/shell/*.{sh,fish,ps1}; see mcpp.toml.
import xlings.core.xself.profile_resources;
import xlings.core.subos.manifest;

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

bool is_builtin_shim(std::string_view name);

bool is_bootstrap_home_root(const fs::path& root);

fs::path xlings_binary_in_home(const fs::path& home_dir);

// Unified shim creation: symlink (Unix) > hardlink > copy
LinkResult create_shim(const fs::path& source, const fs::path& target);

std::size_t ensure_subos_shims(const fs::path& target_bin_dir,
                               const fs::path& shim_src,
                               const fs::path& pkg_root);

static void ensure_parent_dirs_(const fs::path& file);

static void write_if_missing_(const fs::path& path, std::string_view content);

// Give a subos directory a valid `subos_info`, preserving everything else in
// the file. Idempotent: a block that already validates is left alone, so the
// envs packages declared into it survive.
static void ensure_subos_manifest_(const fs::path& subos_dir);

// Extract the value following `# xlings-profile-version: ` on any line of
// `text`. Returns an empty string when the marker is absent, which we
// interpret as "legacy v1" — anything older than the time we started
// shipping a version marker.
static std::string extract_profile_version_(std::string_view text);

// Profile-aware writer. Behavior:
//
//   1. Path doesn't exist  → write fresh.
//   2. Path exists, version matches `target_version` → leave alone (we
//      respect any user edits made after install).
//   3. Path exists with older or missing version marker → overwrite, log
//      the upgrade. This is how shell-level subos switching reaches users
//      who installed before v2 of the profile shipped.
//
// The version of the bytes we're shipping comes from
// xlings::xself::profile_resources::kVersion; the on-disk value comes from
// the marker line we inject as the first comment of every profile.
static void write_or_upgrade_profile_(const fs::path& path,
                                      std::string_view content,
                                      std::string_view target_version);

// Read-modify-writes `<home>/.xlings.json` without taking the state lock
// itself. Both callers reach it through `ensure_home_layout`, and the only
// caller of that is `xself::cmd_install`, which holds the lock for its whole
// home-rewriting stretch. Acquiring here as well would be harmless (the lock
// is re-entrant within a process) but would suggest this is safe to call
// from somewhere unlocked, which it is not.
static void ensure_home_config_defaults_(const fs::path& home_dir);

bool ensure_home_layout(const fs::path& home_dir);

// `xlings self init` — bootstrap (or repair) the home directory layout.
// Thin wrapper over ensure_home_layout; kept here so the dispatcher in
// xself.cppm can route by command name without pulling Config into a
// separate translation unit.
export int cmd_init();

} // namespace xlings::xself
