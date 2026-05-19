// xim-specific cross-version compatibility shim collection.
//
// Follows the same convention as xself/compat.cppm: each compat feature
// lives in its own `vX_Y_Z` sub-namespace so the version it dates from
// is visible at every call site, and so a clean removal is a one-shot
// operation:
//
//   1. Bump the codebase past the removal target.
//   2. Delete the matching `namespace vX_Y_Z { ... }` block in this file.
//   3. Rebuild — every reference to `xim::compat::vX_Y_Z::*` surfaces as
//      a hard build error. Delete the call and any surrounding
//      `COMPAT(X.Y.Z → drop in A.B.C)` marker comment. No grep needed.
export module xlings.core.xim.compat;

import std;
import xlings.core.log;
import xlings.core.config;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.catalog;

namespace xlings::xim::compat {

// =======================================================================
// COMPAT(0.4.37 → drop in 0.6.0)  removal_target: 0.6.0
//
// Library/toolkit xpkgs whose config hook doesn't call xvm:add() for
// package.name silently break subos-scoped `xlings list`, `xlings
// uninstall`, and binding-root version switching — because
// `workspace_installed` has no entry for the package.
//
// `ensure_binding_root_registered` is called after every config-hook
// phase in installer.cppm. It checks whether `node.name` was registered
// in the current subos's `workspace_installed`. If not, it injects a
// `type="marker"` entry that:
//   * Acts as a binding root for `xlings use pkg@ver` cascade switching
//   * Does NOT create PATH shims (type != "program")
//   * Enables subos-scoped list / uninstall / tracking
//
// Once all xpkgs explicitly register a binding root (aided by the lint
// warning D1 in process_xvm_operations_), this compat shim can be
// deleted by removing the entire v0_4_37 namespace.
// =======================================================================
export namespace v0_4_37 {

// Compute the namespace prefix for version keys. Primary-repo packages
// get bare versions; other repos get "ns:version" to allow coexistence.
// Shared with process_xvm_operations_ (which duplicates this logic
// internally; a future cleanup can unify them).
inline std::string compute_version_ns(const PlanNode& node) {
    auto& globalRepos = Config::global_index_repos();
    bool isPrimary = !globalRepos.empty()
        && node.namespaceName == globalRepos[0].name;
    return (!isPrimary && !node.namespaceName.empty())
        ? node.namespaceName : std::string{};
}

void ensure_binding_root_registered(const PlanNode& node,
                                    const std::filesystem::path& dataDir) {
    if (node.kind == DepKind::Build) return;

    std::string version_ns = compute_version_ns(node);
    auto ver_key = xvm::make_ns_version(version_ns, node.version);
    const auto& wsi = Config::workspace_installed();

    // Check: is node.name already tracked in current subos?
    if (auto it = wsi.find(node.name); it != wsi.end()) {
        for (auto& v : it->second) {
            if (v == ver_key || xvm::strip_namespace(v) == node.version)
                return;  // already registered — nothing to do
        }
    }

    // Check VersionDB — another subos may have registered it but this
    // subos hasn't opted in yet.
    auto db = Config::versions();
    auto resolved = xvm::match_version(db, node.name, node.version);

    if (resolved.empty()) {
        // Not in VersionDB at all — inject marker entry
        std::string path = ((node.storeRoot.empty()
            ? (dataDir / "xpkgs") : node.storeRoot)
            / xim::package_store_name(node.namespaceName, node.name)
            / node.version).string();

        xvm::add_version(Config::versions_mut(),
                         node.name, node.version, path,
                         "marker", "", "", version_ns, "");
        Config::save_versions();
    }

    // Ensure current subos tracks this version
    auto& list = Config::workspace_installed_mut()[node.name];
    if (std::find(list.begin(), list.end(), ver_key) == list.end()) {
        list.push_back(ver_key);
    }
    Config::save_workspace();

    log::debug("[{}] COMPAT(0.4.37): auto-registered as marker (binding root), ver={}",
               node.name, ver_key);
}

} // namespace v0_4_37

} // namespace xlings::xim::compat
