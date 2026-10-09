module xlings.subos.spec;

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.network;

namespace xlings::subos::spec {

std::string_view to_string(Backend b) {
    switch (b) {
    case Backend::Bwrap:        return "bwrap";
    case Backend::Proot:        return "proot";
    case Backend::HomeRedirect: return "home-redirect";
    case Backend::Landlock:     return "landlock";
    default:                    return "fake";
    }
}

std::string_view to_string(Storage s) {
    switch (s) {
    case Storage::Image: return "image";
    case Storage::Tmpfs: return "tmpfs";
    default:             return "shared";
    }
}

std::optional<Storage> storage_from_string(std::string_view s) {
    if (s == "shared") return Storage::Shared;
    if (s == "image") return Storage::Image;
    if (s == "tmpfs") return Storage::Tmpfs;
    return std::nullopt;
}

namespace {

std::string_view kind_name(MountKind k) {
    switch (k) {
    case MountKind::RoBind:  return "ro-bind";
    case MountKind::Bind:    return "bind";
    case MountKind::DevBind: return "dev-bind";
    case MountKind::Tmpfs:   return "tmpfs";
    case MountKind::Proc:    return "proc";
    case MountKind::Dev:     return "dev";
    case MountKind::Dir:     return "dir";
    }
    return "?";
}

}  // namespace

nlohmann::json SandboxSpec::describe() const {
    nlohmann::json j;
    j["backend"] = std::string(to_string(backend));
    j["mounts"] = nlohmann::json::array();
    for (auto& m : mounts) {
        nlohmann::json e{{"kind", std::string(kind_name(m.kind))}, {"dst", m.dst}};
        if (!m.src.empty()) e["src"] = m.src;
        j["mounts"].push_back(std::move(e));
    }
    j["unshare"] = {{"user", unshare_user}, {"pid", unshare_pid}, {"ipc", unshare_ipc},
                    {"uts", unshare_uts}, {"net", unshare_net}};
    j["disable_userns"] = disable_userns;
    if (net_nat) j["net"] = {{"mode", "nat"}, {"host_loopback", host_loopback}, {"publish", publish}};
    if (net_proxy) {
        const auto proxy = network::parse_proxy(proxy_url);
        j["net"] = {{"mode", "proxy"}, {"gateway", "socks5h://127.0.0.1:1080"}, {"dns", "remote"}};
        if (proxy) { j["net"]["proxy_host"] = proxy->host; j["net"]["proxy_port"] = proxy->port; }
    }
    j["die_with_parent"] = die_with_parent;
    j["new_session"] = new_session;
    j["block_tiocsti"] = block_tiocsti;
    if (!hostname.empty()) j["hostname"] = hostname;
    if (uid) j["uid"] = *uid;
    j["clear_env"] = clear_env;
    // Names only: values can hold secrets, and this lands in audits.
    j["env"] = nlohmann::json::array();
    for (auto& [k, v] : env) j["env"].push_back(k);
    if (backend == Backend::Landlock) {
        j["landlock_rw"] = nlohmann::json::array();
        for (auto& p : landlock_rw) j["landlock_rw"].push_back(p.generic_string());
    }
    j["cwd"] = cwd.string();
    j["argv"] = argv;
    j["degraded"] = nlohmann::json::array();
    for (auto& d : degraded)
        j["degraded"].push_back({{"dimension", d.dimension}, {"reason", d.reason}, {"fix", d.fix}});
    return j;
}

}  // namespace xlings::subos::spec
