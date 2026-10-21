module xbus.address;

import std;
import openbus;
import xbus.detail.backend;

namespace xbus {

std::optional<Address> Address::parse(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) return std::nullopt;
    Address a{std::string(text.substr(0, colon)), std::string(text.substr(colon + 1))};
    static constexpr std::array<std::string_view, 5> kSchemes{"local", "unix", "vsock", "tcp", "quic"};
    if (!std::ranges::contains(kSchemes, a.scheme)) return std::nullopt;
    return a;
}

Address default_address() {
    if (auto given = detail::backend::environment("OPENBUS_ROUTER"))
        if (auto a = Address::parse(*given)) return *a;
    return Address{"local", "router"};
}

std::string host_root() {
    std::string name;
    for (const char c : detail::backend::host_name()) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        name += (std::isalnum(static_cast<unsigned char>(l)) || l == '-' || l == '_' || l == '.') ? l : '-';
    }
    if (name.empty()) name = "localhost";
    return "host/" + name.substr(0, openbus::kMaxSegment);
}

}  // namespace xbus
