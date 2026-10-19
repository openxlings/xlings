// The host's proxy as a machine reaches it (vm.cppm; design 2026-10-19 §27.3):
// pure functions, tested as such.
module luban.service;

import std;

namespace luban::service {

namespace {

constexpr std::array<std::string_view, 3> kProxies{"HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY"};

std::string lower_(std::string_view s) {
    std::string out(s);
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// The variable of either case, upper first.
std::string get_(const std::map<std::string, std::string>& env, std::string_view upper) {
    if (auto it = env.find(std::string(upper)); it != env.end() && !it->second.empty()) return it->second;
    if (auto it = env.find(lower_(upper)); it != env.end() && !it->second.empty()) return it->second;
    return {};
}

}  // namespace

std::string map_proxy_url(std::string_view url, std::string_view gateway) {
    // scheme://[user[:pass]@]host[:port][/...]
    const auto scheme = url.find("://");
    const std::size_t from = scheme == std::string_view::npos ? 0 : scheme + 3;
    std::size_t host = from;
    const auto end = url.find('/', from);
    const auto authority = url.substr(from, end == std::string_view::npos ? std::string_view::npos : end - from);
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) host = from + at + 1;
    const auto rest = url.substr(host);
    std::string_view name;
    std::size_t name_len = 0;
    if (rest.starts_with('[')) {
        const auto close = rest.find(']');
        if (close == std::string_view::npos) return std::string(url);
        name = rest.substr(1, close - 1);
        name_len = close + 1;
    } else {
        name_len = std::min(rest.find_first_of(":/"), rest.size());
        name = rest.substr(0, name_len);
    }
    const auto n = lower_(name);
    if (n != "127.0.0.1" && n != "localhost" && n != "::1" && !n.starts_with("127.")) return std::string(url);
    return std::string(url.substr(0, host)) + std::string(gateway) + std::string(rest.substr(name_len));
}

std::map<std::string, std::string> host_proxy_env(const std::map<std::string, std::string>& host, std::string_view gateway,
                                                  std::string_view nat) {
    std::map<std::string, std::string> out;
    for (const auto k : kProxies)
        if (const auto v = get_(host, k); !v.empty()) {
            const auto mapped = map_proxy_url(v, gateway);
            out[std::string(k)] = mapped;
            out[lower_(k)] = mapped;
        }
    if (out.empty()) return out;
    // The NAT and the machine itself go direct.
    std::string no = get_(host, "NO_PROXY");
    for (const auto add : {std::string(nat), std::string("localhost"), std::string("127.0.0.1")})
        if (no.find(add) == std::string::npos) no += (no.empty() ? "" : ",") + add;
    out["NO_PROXY"] = no;
    out["no_proxy"] = no;
    return out;
}

std::string network_line(const std::map<std::string, std::string>& host, const std::map<std::string, std::string>& inside) {
    std::string proxy, original;
    for (const auto k : kProxies)
        if (proxy.empty()) {
            proxy = get_(inside, k);
            original = get_(host, k);
        }
    if (proxy.empty()) return "like the host";
    if (!original.empty() && original != proxy) return std::format("like the host · proxy {} (the host's {})", proxy, original);
    return std::format("like the host · proxy {}", proxy);
}

}  // namespace luban::service
