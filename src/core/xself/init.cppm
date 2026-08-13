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

bool ensure_home_layout(const fs::path& home_dir);

// `xlings self init` — bootstrap (or repair) the home directory layout.
// Thin wrapper over ensure_home_layout; kept here so the dispatcher in
// xself.cppm can route by command name without pulling Config into a
// separate translation unit.
export int cmd_init();

} // namespace xlings::xself
