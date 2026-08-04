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

bool is_builtin_shim(std::string_view name) {
    for (auto n : SHIM_NAMES_BASE)
        if (n == name) return true;
    for (auto n : SHIM_NAMES_OPTIONAL)
        if (n == name) return true;
    return false;
}

bool is_bootstrap_home_root(const fs::path& root) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root / ".xlings.json", ec)) return false;
    if (!fs::exists(root / "bin", ec) || !fs::is_directory(root / "bin", ec)) return false;
#ifdef _WIN32
    return fs::exists(root / "bin" / "xlings.exe", ec);
#else
    return fs::exists(root / "bin" / "xlings", ec);
#endif
}

fs::path xlings_binary_in_home(const fs::path& home_dir) {
#ifdef _WIN32
    auto bin = home_dir / "bin" / "xlings.exe";
#else
    auto bin = home_dir / "bin" / "xlings";
#endif
    if (fs::exists(bin)) return bin;
    return {};
}

// Unified shim creation: symlink (Unix) > hardlink > copy
LinkResult create_shim(const fs::path& source, const fs::path& target) {
    std::error_code ec;

    if (!fs::exists(source, ec)) return LinkResult::Failed;

    // Free the target path. `fs::remove` alone is not enough on Windows: it
    // cannot delete a running executable, and the shim being rewritten here is
    // frequently `xlings.exe` itself during `self update`. The failure was
    // discarded, so the code went on to hard-link and copy over a path that
    // was still occupied and reported "failed to create shim ... the process
    // cannot access the file because it is being used by another process"
    // (issue #473) -- with the update half-applied.
    //
    // displace_locked_file renames the occupant aside on Windows, which works
    // on a running image, and is a plain unlink on POSIX.
    if (!platform::displace_locked_file(target)) {
        log::error("[xlings:self]: cannot free shim path {} "
                   "(a process is holding it open)", target.string());
        return LinkResult::Failed;
    }
    ec.clear();

#if !defined(_WIN32)
    // Unix: prefer relative symlink
    auto rel = fs::relative(source, target.parent_path(), ec);
    if (!ec && !rel.empty()) {
        fs::create_symlink(rel, target, ec);
        if (!ec) return LinkResult::Symlink;
    }
    ec.clear();
    // Fallback: absolute symlink
    fs::create_symlink(source, target, ec);
    if (!ec) return LinkResult::Symlink;
    ec.clear();
#endif

    // Hardlink (Unix fallback / Windows primary)
    fs::create_hard_link(source, target, ec);
    if (!ec) return LinkResult::Hardlink;
    ec.clear();

    // Final fallback: copy
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    if (!ec) return LinkResult::Copy;

    log::error("[xlings:self]: failed to create shim {} - {}",
        target.string(), ec.message());
    return LinkResult::Failed;
}

std::size_t ensure_subos_shims(const fs::path& target_bin_dir,
                               const fs::path& shim_src,
                               const fs::path& pkg_root) {
    if (!fs::exists(shim_src)) return 0;

    std::string ext = shim_src.extension().string();
    std::size_t failures = 0;

    for (auto name : SHIM_NAMES_BASE) {
        auto dst = target_bin_dir / (std::string(name) + ext);
        if (create_shim(shim_src, dst) == LinkResult::Failed) ++failures;
    }

    if (!pkg_root.empty()) {
        auto bin_dir = pkg_root / "bin";
        for (auto name : SHIM_NAMES_OPTIONAL) {
            auto opt_bin = bin_dir / (std::string(name) + ext);
            if (fs::exists(opt_bin)) {
                auto dst = target_bin_dir / (std::string(name) + ext);
                if (create_shim(shim_src, dst) == LinkResult::Failed) ++failures;
            }
        }
    }

    // COMPAT(0.4.8 → drop in 0.6.0): one-shot migration cleanup.
    compat::v0_4_8::cleanup_legacy_alias_shims(target_bin_dir, shim_src);

    platform::make_files_executable(target_bin_dir);
    return failures;
}

static void ensure_parent_dirs_(const fs::path& file) {
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
}

static void write_if_missing_(const fs::path& path, std::string_view content) {
    if (fs::exists(path)) return;
    ensure_parent_dirs_(path);
    platform::write_string_to_file(path.string(), std::string(content));
}

// Give a subos directory a valid `subos_info`, preserving everything else in
// the file. Idempotent: a block that already validates is left alone, so the
// envs packages declared into it survive.
static void ensure_subos_manifest_(const fs::path& subos_dir) {
    namespace mf = xlings::subos::manifest;
    auto path = subos_dir / ".xlings.json";

    nlohmann::json json = nlohmann::json::object();
    if (fs::exists(path)) {
        try {
            auto parsed = nlohmann::json::parse(
                platform::read_file_to_string(path.string()), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                // Unreadable, not absent. Overwriting would throw away a
                // workspace we cannot see; leave it for doctor to report.
                log::warn("[xlings:self]: {} is not readable JSON; leaving it "
                          "alone (run `xlings self doctor`)",
                          Config::display_path(path));
                return;
            }
            json = std::move(parsed);
        } catch (...) { return; }
    }
    if (!json.contains("workspace")) json["workspace"] = nlohmann::json::object();
    if (mf::validate_block(json).empty()) return;

    json[std::string(mf::BLOCK)] =
        mf::make_block(mf::DEFAULT_RUNTIME, std::format("xlings {}", Info::VERSION));
    ensure_parent_dirs_(path);
    platform::write_string_to_file(path.string(), json.dump(2));
}

// Extract the value following `# xlings-profile-version: ` on any line of
// `text`. Returns an empty string when the marker is absent, which we
// interpret as "legacy v1" — anything older than the time we started
// shipping a version marker.
static std::string extract_profile_version_(std::string_view text) {
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
                                      std::string_view target_version) {
    if (!fs::exists(path)) {
        ensure_parent_dirs_(path);
        platform::write_string_to_file(path.string(), std::string(content));
        return;
    }

    auto existing = platform::read_file_to_string(path.string());
    auto existing_version = extract_profile_version_(existing);
    if (existing_version == target_version) return;

    log::debug("upgrading {} (was: {}, now: {})",
               path.filename().string(),
               existing_version.empty() ? "<no marker, treated as v1>"
                                        : existing_version,
               target_version);
    platform::write_string_to_file(path.string(), std::string(content));
}

// Read-modify-writes `<home>/.xlings.json` without taking the state lock
// itself. Both callers reach it through `ensure_home_layout`, and the only
// caller of that is `xself::cmd_install`, which holds the lock for its whole
// home-rewriting stretch. Acquiring here as well would be harmless (the lock
// is re-entrant within a process) but would suggest this is safe to call
// from somewhere unlocked, which it is not.
static void ensure_home_config_defaults_(const fs::path& home_dir) {
    auto config_path = home_dir / ".xlings.json";
    nlohmann::json json = nlohmann::json::object();

    if (fs::exists(config_path)) {
        try {
            auto content = platform::read_file_to_string(config_path.string());
            auto parsed = nlohmann::json::parse(content, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) json = std::move(parsed);
        } catch (...) {}
    }

    if (!json.contains("activeSubos") || !json["activeSubos"].is_string() ||
        json["activeSubos"].get<std::string>().empty()) {
        json["activeSubos"] = "default";
    }
    if (!json.contains("subos") || !json["subos"].is_object()) {
        json["subos"] = nlohmann::json::object();
    }
    if (!json["subos"].contains("default") || !json["subos"]["default"].is_object()) {
        json["subos"]["default"] = {{"dir", ""}};
    }

    ensure_parent_dirs_(config_path);
    platform::write_string_to_file(config_path.string(), json.dump(2));
}

bool ensure_home_layout(const fs::path& home_dir) {
    std::error_code ec;
    if (home_dir.empty()) return false;

    auto default_subos = home_dir / "subos" / "default";
    auto dirs = {
        home_dir,
        home_dir / "bin",
        home_dir / "config" / "shell",
        home_dir / "data" / "xpkgs",
        home_dir / "data" / "runtimedir",
        home_dir / "data" / "xim-index-repos",
        home_dir / "data" / "local-indexrepo",
        default_subos / "bin",
        default_subos / "lib",
        default_subos / "usr",
        default_subos / "generations",
    };

    for (auto& dir : dirs) {
        fs::create_directories(dir, ec);
        if (ec) {
            log::error("[xlings:self]: failed to create {} - {}",
                       Config::display_path(dir), ec.message());
            return false;
        }
    }

    auto current_link = home_dir / "subos" / "current";
    platform::create_directory_link(current_link, default_subos);

    // Not write_if_missing_: every home that predates `subos_info` already has
    // this file, so "missing" is exactly the case that never fires on the
    // subos that matters most. `default` is where an ordinary user installs
    // everything, and a default without the block would make the whole
    // configuration layer inert on upgrade while looking installed.
    //
    // This is also the migration: `self init` runs on install and update, so an
    // old home repairs its default subos on the next either. Other subos in
    // that home are `subos doctor --fix`'s job -- init does not enumerate them.
    ensure_subos_manifest_(default_subos);
    write_if_missing_(home_dir / "data" / "xim-index-repos" / "xim-indexrepos.json", "{}");
    // Profile content lives in xlings.core.xself.profile_resources. We use
    // the version-aware writer so users who installed an older xlings get
    // their profile auto-upgraded on the next `self init` / `self update`,
    // while users on the current version preserve any local edits.
    write_or_upgrade_profile_(home_dir / "config" / "shell" / "xlings-profile.sh",
                              profile_resources::bash_sh,
                              profile_resources::kVersion);
    write_or_upgrade_profile_(home_dir / "config" / "shell" / "xlings-profile.fish",
                              profile_resources::fish,
                              profile_resources::kVersion);
    write_or_upgrade_profile_(home_dir / "config" / "shell" / "xlings-profile.ps1",
                              profile_resources::pwsh,
                              profile_resources::kVersion);

    ensure_home_config_defaults_(home_dir);

    auto xlings_bin = xlings_binary_in_home(home_dir);
    if (!xlings_bin.empty()) {
        // A home whose shims could not be written is not a laid-out home. It
        // used to report success anyway, which is how a failed Windows update
        // produced "healed" alongside the state that made it fail.
        if (auto failures = ensure_subos_shims(default_subos / "bin",
                                               xlings_bin, home_dir);
            failures != 0) {
            log::error("[xlings:self]: {} shim(s) could not be written into {}",
                       failures, (default_subos / "bin").string());
            return false;
        }
    }

    return true;
}

// `xlings self init` — bootstrap (or repair) the home directory layout.
// Thin wrapper over ensure_home_layout; kept here so the dispatcher in
// xself.cppm can route by command name without pulling Config into a
// separate translation unit.
export int cmd_init() {
    auto& p = Config::paths();
    if (!ensure_home_layout(p.homeDir)) return 1;
    log::info("init ok");
    return 0;
}

} // namespace xlings::xself
