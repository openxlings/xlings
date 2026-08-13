module xlings.capabilities;

import std;
import xlings.platform;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;
import xlings.libs.json;
import xlings.core.xim.commands;
import xlings.core.xim.index_cmd;
import xlings.core.xvm.commands;
import xlings.core.config;
import xlings.core.home_config;
import xlings.core.subos;
import xlings.core.xself;
import xlings.platform;
import xlings.runtime.cancellation;
import xlings.core.utf8;

namespace xlings::capabilities {

Result exit_result(int code) {
    return nlohmann::json({{"exitCode", code}}).dump();
}

capability::Registry build_registry() {
    capability::Registry reg;
    // xlings core capabilities
    reg.register_capability(std::make_unique<SearchPackages>());
    reg.register_capability(std::make_unique<InstallPackages>());
    reg.register_capability(std::make_unique<PlanInstall>());
    reg.register_capability(std::make_unique<RemovePackage>());
    reg.register_capability(std::make_unique<UpdatePackages>());
    reg.register_capability(std::make_unique<ListPackages>());
    reg.register_capability(std::make_unique<PackageInfo>());
    reg.register_capability(std::make_unique<ListVersions>());
    reg.register_capability(std::make_unique<UseVersion>());
    reg.register_capability(std::make_unique<SystemStatus>());
    reg.register_capability(std::make_unique<ListIndexVersions>());
    // sub-OS + env (added 2026-04-26 per interface-api-v1-eval)
    reg.register_capability(std::make_unique<ListSubos>());
    reg.register_capability(std::make_unique<ListSubosShims>());
    reg.register_capability(std::make_unique<CreateSubos>());
    reg.register_capability(std::make_unique<SwitchSubos>());
    reg.register_capability(std::make_unique<RemoveSubos>());
    reg.register_capability(std::make_unique<Env>());
    // Index repo management (added 2026-04-26 per interface-api-v1-eval P1)
    reg.register_capability(std::make_unique<ListRepos>());
    reg.register_capability(std::make_unique<AddRepo>());
    reg.register_capability(std::make_unique<RemoveRepo>());
    return reg;
}

}
