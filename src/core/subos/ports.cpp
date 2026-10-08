module xlings.core.subos.ports;

import std;
import xlings.core.config;
import xlings.core.entry_binary;
import xlings.core.xim.commands;
import xlings.core.xvm.shim;
import xlings.core.xvm.shim_identity;
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
    ports.shim_owner =
        [classifier = xvm::ShimClassifier{entry_binary::path_of(Config::paths().homeDir)}](
            const std::filesystem::path& path) mutable -> std::optional<std::filesystem::path> {
        // This asks whether the backend candidate is an xlings dispatcher;
        // resolve_owner_home alone also falls back to the caller's entry.
        const auto identity = classifier.classify(path);
        if (identity.state == xvm::ShimState::Current || identity.state == xvm::ShimState::Stale)
            return xvm::resolve_owner_home(path);
        return std::nullopt;
    };
    return ports;
}

}  // namespace xlings::subos
