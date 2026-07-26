export module xlings.core.xim.libxpkg.types.script;

import std;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.catalog;
import xlings.core.common;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xself;
import xlings.core.xvm.db;
import xlings.core.xvm.removal;
import mcpplibs.xpkg.executor;

export namespace xlings::xim::script {

bool default_install(const PlanNode& node,
                     mcpplibs::xpkg::ExecutionContext& ctx) {
    auto dest = ctx.install_dir / (node.name + ".lua");
    std::error_code ec;
    std::filesystem::create_directories(ctx.install_dir, ec);
    if (ec) {
        log::error("failed to create install dir {}: {}", ctx.install_dir.string(), ec.message());
        return false;
    }
    std::filesystem::copy_file(node.pkgFile, dest,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        log::error("failed to copy script {} -> {}: {}", node.pkgFile.string(), dest.string(), ec.message());
        return false;
    }
    log::debug("script installed: {}", dest.string());
    return true;
}

bool default_config(const PlanNode& node,
                    const std::filesystem::path& dataDir) {
    auto storeName = package_store_name(node.namespaceName, node.name);
    auto targetScript = (node.storeRoot.empty() ? (dataDir / "xpkgs") : node.storeRoot)
        / storeName
        / node.version
        / (node.name + ".lua");

    auto alias = "xlings script " + targetScript.string();
    auto bindir = targetScript.parent_path().string();

    xvm::add_version(Config::versions_mut(),
                     node.name, node.version, bindir, "program", "", alias);

    auto ver_key = xvm::make_ns_version("", node.version);
    Config::workspace_mut()[node.name] = ver_key;

    auto paths = Config::paths();
#ifdef _WIN32
    auto xlings_bin = paths.homeDir / "bin" / "xlings.exe";
    constexpr std::string_view shim_ext = ".exe";
#else
    auto xlings_bin = paths.homeDir / "bin" / "xlings";
    constexpr std::string_view shim_ext = "";
#endif
    // Bootstrap-layout fallback: pre-`self init` the binary may live
    // directly at <homeDir>/xlings before being moved under bin/.
    // Matches the resolution used in xvm/commands.cppm and xself/doctor.cppm.
    if (!std::filesystem::exists(xlings_bin))
        xlings_bin = paths.homeDir / ("xlings" + std::string(shim_ext));

    if (std::filesystem::exists(xlings_bin)) {
        std::string shim_name = node.name;
        if (!shim_ext.empty() && !shim_name.ends_with(shim_ext))
            shim_name += std::string(shim_ext);
        std::filesystem::create_directories(paths.binDir);
        xself::create_shim(xlings_bin, paths.binDir / shim_name);
        common::mirror_shim_to_global_bin(xlings_bin, shim_name);
    }

    Config::save_versions();
    Config::save_workspace();
    return true;
}

bool default_uninstall(const std::string& name, const std::string& version) {
    if (version.empty()) return false;

    auto exactVersion = xvm::resolve_exact_version_key(
        Config::versions_mut(), name, version);
    if (!exactVersion) return false;
    const auto wasActive =
        Config::workspace().contains(name)
        && Config::workspace().at(name) == *exactVersion;
    const std::vector<xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = name,
            .version = *exactVersion,
        },
    };
    auto removal = xvm::apply_removal_batch(
        Config::versions_mut(),
        Config::workspace_mut(),
        Config::workspace_installed_mut(),
        operations,
        {});
    if (!removal) return false;

    auto paths = Config::paths();
#ifdef _WIN32
    constexpr std::string_view shim_ext = ".exe";
#else
    constexpr std::string_view shim_ext = "";
#endif
    std::string shim_name = name;
    if (!shim_ext.empty() && !shim_name.ends_with(shim_ext))
        shim_name += std::string(shim_ext);
    auto shim_path = paths.binDir / shim_name;
    if (wasActive
        && !xvm::has_usable_workspace_version(
            Config::versions_mut(),
            Config::workspace_installed(),
            name)) {
        std::error_code ec;
        if (std::filesystem::exists(shim_path, ec)) {
            ec.clear();
            std::filesystem::remove(shim_path, ec);
        }
    }

    Config::save_versions();
    Config::save_workspace();
    return true;
}

} // namespace xlings::xim::script
