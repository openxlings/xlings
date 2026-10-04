module xlings.core.subos.ports;

import std;
import xlings.core.config;
import xlings.core.xim.commands;
import xlings.core.xvm.shim;
import xlings.runtime;
import xlings.subos.home_view;
import xlings.subos.ports;

namespace xlings::subos {

HomeView home_view() {
    return HomeView{ .home = Config::paths().homeDir };
}

Ports make_ports(EventStream& stream) {
    Ports ports;
    ports.install_backend = [&stream](std::string_view package) -> std::expected<void, std::string> {
        std::vector<std::string> targets{ "xim:" + std::string(package) };
        if (xim::cmd_install(targets, /*yes=*/true, /*noDeps=*/false, stream) != 0)
            return std::unexpected("could not install " + std::string(package));
        return {};
    };
    ports.shim_owner = [](const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
        if (auto owner = xvm::resolve_owner_home(path)) return *owner;
        return std::nullopt;
    };
    return ports;
}

}  // namespace xlings::subos
