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
module xlings.core.xself.compat;

import std;
import xlings.core.log;
import xlings.platform;
import xlings.core.xself.profile_resources;

namespace xlings::xself::compat {

namespace v0_4_8 {

bool is_legacy_alias_symlink_to_bootstrap(const fs::path& path,
                                          const fs::path& canonical_bootstrap)
{
    std::error_code ec;
    if (!fs::is_symlink(path, ec)) return false;
    ec.clear();
    auto target = fs::weakly_canonical(path, ec);
    if (ec) return false;
    return target == canonical_bootstrap;
}

void cleanup_legacy_alias_shims(const fs::path& bin_dir,
                                const fs::path& bootstrap_path) {
    std::error_code ec;
    auto canonical_bootstrap = fs::weakly_canonical(bootstrap_path, ec);
    if (ec) return;

    std::string ext = bootstrap_path.extension().string();
    for (auto name : LEGACY_ALIAS_NAMES) {
        auto path = bin_dir / (std::string(name) + ext);
        if (!is_legacy_alias_symlink_to_bootstrap(path, canonical_bootstrap))
            continue;
        ec.clear();
        fs::remove(path, ec);
        log::debug("[migrate] removed legacy alias shim: {}", path.string());
    }
}

bool report_deprecated_alias_if_match(std::string_view program_name) {
    for (auto& [alias, suggestion] : DEPRECATED_ALIASES) {
        if (alias == program_name) {
            std::println(std::cerr,
                "[error] `{}` was removed in 0.4.8. Use `{}` instead.",
                alias, suggestion);
            std::println(std::cerr,
                "        Run `xlings self doctor --fix` to clean up "
                "leftover shortcuts.");
            return true;
        }
    }
    return false;
}

}

namespace v0_4_17 {

void auto_upgrade_profiles_if_stale(const fs::path& home_dir) {
    if (home_dir.empty()) return;

    auto config_dir = home_dir / "config" / "shell";
    std::error_code ec;
    if (!fs::is_directory(config_dir, ec)) return;

    struct Slot {
        const char*       filename;
        std::string_view  bytes;
    };
    const Slot slots[] = {
        { "xlings-profile.sh",   profile_resources::bash_sh },
        { "xlings-profile.fish", profile_resources::fish    },
        { "xlings-profile.ps1",  profile_resources::pwsh    },
    };

    for (auto& s : slots) {
        auto path = config_dir / s.filename;
        if (!detail_::needs_profile_upgrade(path, profile_resources::kVersion))
            continue;
        try {
            platform::write_string_to_file(path.string(), std::string(s.bytes));
            log::debug("[compat] upgraded {} to profile version {}",
                       path.string(), profile_resources::kVersion);
        } catch (...) {
            // Profile upgrade is opportunistic — failure shouldn't block
            // the actual command the user invoked.
        }
    }
}

}

}
