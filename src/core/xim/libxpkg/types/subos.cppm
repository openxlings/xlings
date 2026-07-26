export module xlings.core.xim.libxpkg.types.subos;

import std;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.catalog;
import xlings.core.common;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xvm.db;
import xlings.core.xvm.removal;
import xlings.libs.json;
import mcpplibs.xpkg.executor;

// Default xim handlers for `type = "subos"` packages.
//
// A subos base package is a normal xpkg whose payload is a directory
// containing `.xlings.json` (workspace declaration) + optional static
// files (templates, README, etc.). xpm.deps drives standard dep resolution.
// The default hooks here:
//   1. install: ensure `.xlings.json` exists in install_dir (synthesize
//      from xpm.deps if the tarball didn't carry one);
//   2. config:  register the package via xvm.add_version so it's
//      queryable / uninstallable;
//   3. uninstall: remove the xvm version entry. The on-disk payload
//      removal is handled by xim's standard uninstall path.
//
// Authors can override any of these by defining install()/config()/
// uninstall() in the package's .lua. The existing executor's "has_hook"
// check is what decides hook vs default; this module is only invoked
// when the hook is absent.
//
// See `.agents/docs/subos-as-xpkg-design-2026-05-16.md` (M1).

export namespace xlings::xim::subos {

bool default_install(const PlanNode& node,
                     mcpplibs::xpkg::ExecutionContext& ctx) {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::create_directories(ctx.install_dir, ec);
    if (ec) {
        log::error("subos: failed to create install dir {}: {}",
                   ctx.install_dir.string(), ec.message());
        return false;
    }

    // Ensure bin/ exists for shim placement at fork time (subos new --from
    // expects to find this layout).
    fs::create_directories(ctx.install_dir / "bin", ec);

    // If tarball already laid down .xlings.json, leave it untouched (author
    // intent). Otherwise synthesize a minimal workspace from xpm.deps so
    // `subos new --from <pkg>` has a workspace to copy.
    auto xlingsJson = ctx.install_dir / ".xlings.json";
    if (!fs::exists(xlingsJson)) {
        nlohmann::json j;
        j["workspace"] = nlohmann::json::object();
        for (const auto& dep : node.deps) {
            // dep tokens look like "name@version", "ns:name@version", or "name"
            std::string raw = dep;
            if (auto colon = raw.find(':'); colon != std::string::npos) {
                raw = raw.substr(colon + 1);
            }
            if (auto at = raw.find('@'); at != std::string::npos) {
                j["workspace"][raw.substr(0, at)] = raw.substr(at + 1);
            } else if (!raw.empty()) {
                j["workspace"][raw] = "latest";
            }
        }
        std::ofstream ofs(xlingsJson);
        ofs << j.dump(2);
    }

    log::debug("subos base installed at {}", ctx.install_dir.string());
    return true;
}

bool default_config(const PlanNode& node,
                    const std::filesystem::path& dataDir) {
    auto storeName = package_store_name(node.namespaceName, node.name);
    auto installDir = (node.storeRoot.empty() ? (dataDir / "xpkgs") : node.storeRoot)
        / storeName
        / node.version;
    auto bindir = (installDir / "bin").string();

    // Register so `xlings list` / `xlings info` / `xlings uninstall` work
    // normally. Subos base does NOT need a `xlings`-dispatching shim like
    // type=script does — the package is a fork source, not a runnable.
    xvm::add_version(Config::versions_mut(),
                     node.name, node.version, bindir, "subos-base", "", "");

    auto ver_key = xvm::make_ns_version(node.namespaceName, node.version);
    Config::workspace_mut()[node.name] = ver_key;

    Config::save_versions();
    Config::save_workspace();
    return true;
}

bool default_uninstall(const std::string& name, const std::string& version) {
    // Remove xvm entry; xim's standard uninstall path handles the on-disk
    // payload at xpkgs/<name>/<ver>/. Forks already in subos/ are independent
    // and not touched by this — their .xlings.json points to xpkgs they
    // reference directly (deps), and those deps are independent xpkgs.
    if (version.empty()) return false;
    const std::vector<xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = name,
            .version = version,
        },
    };
    auto removal = xvm::apply_removal_batch(
        Config::versions_mut(),
        Config::workspace_mut(),
        Config::workspace_installed_mut(),
        operations,
        {});
    if (!removal) return false;

    Config::save_versions();
    Config::save_workspace();
    return true;
}

} // namespace xlings::xim::subos
