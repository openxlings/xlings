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
                     mcpplibs::xpkg::ExecutionContext& ctx);

bool default_config(const PlanNode& node,
                    const std::filesystem::path& dataDir,
                    const std::string& versionNamespace);

bool default_uninstall(const std::string& name, const std::string& version);

} // namespace xlings::xim::script
