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
                     mcpplibs::xpkg::ExecutionContext& ctx);

bool default_config(const PlanNode& node,
                    const std::filesystem::path& dataDir,
                    const std::string& versionNamespace);

bool default_uninstall(const std::string& name, const std::string& version);

} // namespace xlings::xim::subos
