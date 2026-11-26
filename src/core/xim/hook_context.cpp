module xlings.core.xim.hook_context;

import std;
import mcpplibs.xpkg.executor;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xself;
import xlings.core.xim.payload;
import xlings.platform;
import xlings.platform.target;

namespace xlings::xim {

namespace {

// A hook runs `xlings` from its bin directory (`system.bindir()`): gcc's
// config hook rewrites the payload's specs through `xlings script`. A home's
// own subos has that entry from `self init`; a project's subos holds only the
// shims of what it installed, so a hook run there found no `xlings`
// (`sh: <project>/.mcpp/.xlings/subos/_/bin/xlings: not found`) and the
// install of the payload failed. The entry is linked beside those shims, to
// the binary each of them names. `xlings` is a reserved name, so the shim
// table neither adds nor removes it.
void ensure_entry_in_bin(const std::filesystem::path& bin) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto dst = bin / ("xlings" + std::string(platform::exe_suffix));
    if (fs::exists(fs::symlink_status(dst, ec))) return;
    auto entry = xself::xlings_binary_in_home(Config::paths().homeDir);
    if (entry.empty() && !Config::paths().systemLayer.empty())
        entry = xself::xlings_binary_in_home(Config::paths().systemLayer);
    if (entry.empty()) return;
    fs::create_directories(bin, ec);
    if (xself::create_shim(entry, dst) == xself::LinkResult::Failed)
        log::debug("hook bin directory {}: the xlings entry could not be linked", bin.string());
}

} // namespace

std::filesystem::path configure_hook_artifact_paths(mcpplibs::xpkg::ExecutionContext& context) {
    auto subosDir = Config::xvm_artifact_subos_dir();
    context.bin_dir = subosDir / "bin";
    context.subos_sysrootdir = subosDir.string();
    ensure_entry_in_bin(context.bin_dir);
    return subosDir;
}

void configure_hook_dependency_stores(mcpplibs::xpkg::ExecutionContext& context,
                                      const std::filesystem::path& selectedStore) {
    auto append = [&](std::filesystem::path root) {
        if (root.empty()) return;
        root = root.lexically_normal();
        if (std::ranges::find(context.dependency_store_roots, root)
            == context.dependency_store_roots.end()) {
            context.dependency_store_roots.push_back(std::move(root));
        }
    };

    append(selectedStore);
    for (auto store : Config::stores()) append(std::move(store));
}

std::string pkgindex_root_for(const std::filesystem::path& pkgFile, bool* outHasLibs) {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto derived = pkgFile.parent_path().parent_path().parent_path();
    if (fs::is_directory(derived / "libs", ec)) {
        if (outHasLibs) *outHasLibs = true;
        return derived.string();
    }

    // Only the primary index. Not "any index that happens to have libs": picking
    // between several would be a guess about which ecosystem a recipe belongs to,
    // and `xim.pkgindex.*` is one shared namespace by construction.
    auto primary = Config::global_data_dir() / "xim-pkgindex";
    if (fs::is_directory(primary / "libs", ec)) {
        log::debug("pkgindex modules: {} has no libs/, using {}",
                   derived.string(), primary.string());
        if (outHasLibs) *outHasLibs = true;
        return primary.string();
    }

    if (outHasLibs) *outHasLibs = false;
    return derived.string();
}

mcpplibs::xpkg::ExecutionContext make_execution_context(const HookScope& scope) {
    mcpplibs::xpkg::ExecutionContext ctx;
    ctx.pkg_name = scope.pkg_name;
    ctx.version = scope.version;
    ctx.platform = std::string(host_platform_tag());
    // The home's host machine (design 2026-11-27 §5.1): the architecture of
    // the packages this client installs, pkginfo.arch() in a hook.
    ctx.arch = std::string(platform::build_arch());
    ctx.install_dir = scope.install_dir;
    ctx.xpkg_dir = scope.xpkg_dir;
    std::error_code ec;
    ctx.run_dir = std::filesystem::current_path(ec);
    ctx.project_data_dir = Config::project_data_dir();
    configure_hook_artifact_paths(ctx);
    configure_hook_dependency_stores(ctx, scope.store.empty() ? Config::paths().dataDir / "xpkgs" : scope.store);
    if (!scope.recipe.empty()) ctx.pkgindex_dir = pkgindex_root_for(scope.recipe);
    ctx.args = scope.args;
    return ctx;
}

} // namespace xlings::xim
