module xlings.confine;

import std;
import xlings.confine.implementation;
import xlings.confine.linux_bwrap;
import xlings.confine.linux_proot;
import xlings.confine.linux_landlock;
import xlings.confine.home_redirect;
import xlings.confine.provider;
import xlings.confine.common;
import xlings.subos.home_view;
import xlings.subos.caps;
import xlings.subos.intent;
import xlings.subos.network;
import xlings.subos.policy;
import xlings.subos.spec;

namespace xlings::confine {

namespace policy = xlings::subos::policy;
namespace network = xlings::subos::network;
using sp::MountKind;
using common::posix;

std::span<const Implementation> implementations() {
    static const std::vector<Implementation> all{
        backends::linux_bwrap(), backends::linux_proot(), backends::linux_landlock(),
        backends::home_redirect(), backends::fake()};
    return all;
}

const Implementation* find(sp::Backend backend) {
    for (const auto& impl : implementations())
        if (impl.backend == backend) return &impl;
    return nullptr;
}

namespace {

std::unexpected<sp::Refusal> refuse(sp::Unmet u) {
    u.need = policy::Need::Must;
    return std::unexpected(sp::Refusal{.missing = {std::move(u)}});
}

// Which implementation runs it here. macOS and Windows have one (the home
// redirect); on Linux a named backend is honoured or refused, and otherwise
// bwrap when it works, then proot -- never Landlock on its own: it hides
// nothing, so it is only ever asked for by name.
std::expected<std::pair<const Implementation*, fs::path>, sp::Refusal>
select(const in::Intent& intent, const Caps& caps, std::optional<sp::Backend> preferred) {
    fs::path chosen;
    auto take = [&](sp::Backend b) -> std::expected<std::pair<const Implementation*, fs::path>, sp::Refusal> {
        const auto* impl = find(b);
        if (auto why = impl->available(caps, intent, chosen)) return refuse(*why);
        return std::pair{impl, chosen};
    };
    if (caps.platform == "macos" || caps.platform == "windows") return take(sp::Backend::HomeRedirect);
    if (preferred == sp::Backend::Fake) return take(sp::Backend::Fake);
    if (preferred && *preferred != sp::Backend::HomeRedirect) return take(*preferred);
    if (caps.bwrap && caps.bwrap->usable) return take(sp::Backend::Bwrap);
    if (caps.proot) return take(sp::Backend::Proot);
    return refuse({"backend", "no sandbox backend available", "xlings install bwrap (or: xlings install proot)",
                   policy::Need::Must});
}

}  // namespace

std::expected<sp::SandboxSpec, sp::Refusal> compile(const policy::Policy& pol, const HomeView& home,
                                                    const Caps& caps, const sp::Request& r) {
    const auto i = subos::intent::lower(pol, home, r);
    auto selected = select(i, caps, r.preferred);
    if (!selected) return std::unexpected(selected.error());
    const auto& impl = *selected->first;
    sp::SandboxSpec s;
    s.backend = impl.backend;
    s.backend_bin = selected->second;

    // ── filesystem ───────────────────────────────────────────────────
    const bool rootfs = !i.root.empty();
    if (rootfs && !impl.presents_root)
        return refuse({"root", std::string(sp::to_string(s.backend)) + " cannot present a root: a rootfs SubOS needs bwrap",
                       "xlings self doctor --isolation", policy::Need::Must});
    if (auto refused = impl.view(i, home, s)) return refuse(*refused);

    // ── processes and terminal (S0, design §16) ──────────────────────
    impl.processes(i, s);

    // ── network, identity, nested namespaces (design §19) ────────────
    // What the policy asks for and this backend cannot give is collected;
    // Must-items refuse below, Should-items degrade with a reason.
    std::vector<sp::Unmet> unmet;
    const bool kernel = impl.kernel;
    const auto backend_name = std::string(sp::to_string(s.backend));
    if (i.net.mode == policy::Net::None) {
        if (kernel) s.unshare_net = true;
        else unmet.push_back({"net", backend_name + " cannot isolate the network",
                              "xlings self doctor --isolation", i.need("net")});
    } else if (i.net.mode == policy::Net::Nat || i.net.mode == policy::Net::Proxy) {
        if (kernel && i.net.mode == policy::Net::Nat && caps.pasta) {
            s.unshare_net = true;
            s.net_nat = true;
            s.pasta_bin = *caps.pasta;
            s.host_loopback = i.net.host_loopback;
            s.publish = i.net.publish;
        } else if (!kernel) {
            unmet.push_back({"net", backend_name + " cannot isolate the network",
                             "xlings self doctor --isolation",
                             i.net.mode == policy::Net::Proxy ? policy::Need::Must : i.need("net")});
        } else if (i.net.mode == policy::Net::Nat) {
            unmet.push_back({"net", "net=nat needs pasta: " + (caps.pasta_missing.empty()
                                 ? std::string("pasta (passt) is not installed") : caps.pasta_missing),
                             "install passt (e.g. apt install passt), or --net none for no network",
                             i.need("net")});
        } else if (caps.platform != "linux") {
            unmet.push_back({"net", "net=proxy requires a Linux network namespace", "--net none",
                             policy::Need::Must});
        } else if (const auto proxy = network::parse_proxy(i.net.proxy); !proxy) {
            unmet.push_back({"net", proxy.error(), "set isolation.proxy to socks5h://HOST:PORT", policy::Need::Must});
        } else {
            s.unshare_net = true;
            s.net_proxy = true;
            s.proxy_url = i.net.proxy;
        }
    }
    if (!i.net.publish.empty() && !s.net_nat)
        unmet.push_back({"publish", "--publish needs net=nat (a private network to publish from)",
                         "--net nat", i.need("publish")});
    if (i.id.neutral) {
        if (kernel) s.hostname = i.instance;
        else unmet.push_back({"identity", "the host name cannot be changed without namespaces", "",
                              i.need("identity")});
    }
    if (i.disable_userns) {
        if (kernel) {
            s.unshare_user = true;
            s.disable_userns = true;
        } else {
            unmet.push_back({"userns", "nested user namespaces cannot be forbidden here", "", i.need("userns")});
        }
    }
    if (!kernel) {
        for (auto& d : s.degraded) d.need = i.need(d.dimension);
        for (auto& d : s.degraded) unmet.push_back(d);
        s.degraded.clear();
        if (impl.advisory_fs)
            unmet.push_back({"fs", *impl.advisory_fs, "not implemented on this platform yet", i.need("fs")});
    }

    // ── mounts (design §11) and named grants (§21.2) ─────────────────
    for (const auto& m : i.mounts) {
        if (m.refused) {
            unmet.push_back({"mount", *m.refused, "", policy::Need::Must});
            continue;
        }
        if (impl.mounts == Implementation::Mounts::Fence) {
            // No view to map into: a path is reachable where it is, and only
            // a read-write one is added to what may be written.
            if (posix(m.inside) != posix(m.source)) {
                unmet.push_back({"mount", m.source + ": Landlock cannot map a path elsewhere (" + m.inside + ")",
                                 "--mount " + m.source + " (the same path inside), or --sandbox bwrap",
                                 policy::Need::Must});
                continue;
            }
            if (m.writable) s.landlock_rw.push_back(m.source);
            continue;
        }
        if (!m.writable && !kernel)
            unmet.push_back({"mount", m.source + ": read-only needs bwrap; mapped read-write",
                             "xlings self doctor --isolation", i.need("fs")});
        s.mounts.push_back({m.writable ? MountKind::Bind : MountKind::RoBind, posix(m.source), posix(m.inside)});
    }
    // Each grant opens exactly one thing: a socket file bound in, its
    // variable pointed at it -- never the host's whole runtime directory.
    std::map<std::string, std::string> grant_env;
    {
        const auto& grants = i.grants;
        auto host_env = [&](std::string_view k) -> std::string {
            auto it = i.host_env.find(std::string(k));
            return it == i.host_env.end() ? std::string{} : it->second;
        };
        const std::string runtime = "/tmp/.xlings-runtime";
        bool used_runtime = false;
        auto bind_socket = [&](const std::string& src, const std::string& dst) {
            s.mounts.push_back({MountKind::Bind, src, dst});
        };
        if (grants.contains("display")) {
            bool any = false;
            if (auto d = host_env("DISPLAY"); !d.empty() && i.exists("/tmp/.X11-unix")) {
                bind_socket("/tmp/.X11-unix", "/tmp/.X11-unix");
                grant_env["DISPLAY"] = d;
                if (auto xa = host_env("XAUTHORITY"); !xa.empty() && i.exists(xa)) {
                    s.mounts.push_back({MountKind::RoBind, xa, "/tmp/.xlings-xauthority"});
                    grant_env["XAUTHORITY"] = "/tmp/.xlings-xauthority";
                }
                any = true;
            }
            if (auto w = host_env("WAYLAND_DISPLAY"), xdg = host_env("XDG_RUNTIME_DIR");
                !w.empty() && !xdg.empty() && i.exists(xdg + "/" + w)) {
                bind_socket(xdg + "/" + w, runtime + "/" + w);
                grant_env["WAYLAND_DISPLAY"] = w;
                used_runtime = true;
                any = true;
            }
            if (!any) unmet.push_back({"display", "no X11 or Wayland display on this host", "", policy::Need::Should});
        }
        if (grants.contains("audio")) {
            bool any = false;
            const auto xdg = host_env("XDG_RUNTIME_DIR");
            if (!xdg.empty() && i.exists(xdg + "/pulse/native")) {
                bind_socket(xdg + "/pulse/native", runtime + "/pulse/native");
                grant_env["PULSE_SERVER"] = "unix:" + runtime + "/pulse/native";
                used_runtime = any = true;
            }
            if (!xdg.empty() && i.exists(xdg + "/pipewire-0")) {
                bind_socket(xdg + "/pipewire-0", runtime + "/pipewire-0");
                used_runtime = any = true;
            }
            if (!any) unmet.push_back({"audio", "no PulseAudio or PipeWire socket on this host", "", policy::Need::Should});
        }
        if (grants.contains("camera")) {
            bool any = false;
            for (int k = 0; k < 10; ++k) {
                auto dev = std::format("/dev/video{}", k);
                if (i.exists(dev)) { s.mounts.push_back({MountKind::DevBind, dev, dev}); any = true; }
            }
            if (!any) unmet.push_back({"camera", "no /dev/video* on this host", "", policy::Need::Should});
        }
        if (grants.contains("ssh-agent")) {
            if (auto sock = host_env("SSH_AUTH_SOCK"); !sock.empty() && i.exists(sock)) {
                bind_socket(sock, "/tmp/.xlings-ssh-agent");
                grant_env["SSH_AUTH_SOCK"] = "/tmp/.xlings-ssh-agent";
            } else {
                unmet.push_back({"ssh-agent", "SSH_AUTH_SOCK is not set to a socket on this host", "",
                                 policy::Need::Should});
            }
        }
        if (grants.contains("dbus")) {
            auto addr = host_env("DBUS_SESSION_BUS_ADDRESS");
            auto path = addr.starts_with("unix:path=") ? addr.substr(10, addr.find(',') - 10) : std::string{};
            if (!path.empty() && i.exists(path)) {
                bind_socket(path, "/tmp/.xlings-dbus");
                grant_env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/tmp/.xlings-dbus";
            } else {
                unmet.push_back({"dbus", addr.empty() ? "no session bus on this host"
                                                      : "the session bus is not a socket file (abstract address)",
                                 "", policy::Need::Should});
            }
        }
        if (used_runtime) grant_env["XDG_RUNTIME_DIR"] = runtime;
        if (impl.sockets == Implementation::Sockets::PassVariables) {
            // Nothing to bind: the host's sockets are where they are. A grant
            // passes the variables that name them, and only those.
            s.mounts.clear();
            grant_env.clear();
            auto pass = [&](std::initializer_list<const char*> names) {
                for (auto n : names) if (auto v = host_env(n); !v.empty()) grant_env[n] = v;
            };
            if (grants.contains("display")) pass({"DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"});
            if (grants.contains("audio")) pass({"PULSE_SERVER", "XDG_RUNTIME_DIR"});
            if (grants.contains("ssh-agent")) pass({"SSH_AUTH_SOCK"});
            if (grants.contains("dbus")) pass({"DBUS_SESSION_BUS_ADDRESS"});
        }
    }

    sp::Refusal refusal;
    for (auto& u : unmet) {
        if (u.need == policy::Need::Must) refusal.missing.push_back(u);
        else s.degraded.push_back(u);
    }
    if (!refusal.missing.empty()) return std::unexpected(std::move(refusal));

    // ── environment and command ──────────────────────────────────────
    s.clear_env = i.env.clear;
    if (s.clear_env) {
        for (auto& [k, v] : i.host_env)
            if (policy::env_name_matches(k, i.env.pass)) s.env[k] = v;
    }
    s.env["XLINGS_ACTIVE_SUBOS"] = i.instance;
    s.env["XLINGS_SUBOS_MODE"] = "sandbox";
    const bool windows = caps.platform == "windows";
    s.env["XLINGS_SUBOS_LIB"] = windows ? (i.instance_dir / "lib").string() : posix(i.instance_dir / "lib");
    impl.finish(i, home, caps, grant_env, s);
    return s;
}

std::vector<std::string> launch_argv(const sp::SandboxSpec& s, std::optional<int> seccomp_fd,
                                     const std::string& self) {
    switch (s.backend) {
    case sp::Backend::Bwrap: return provider::bwrap_argv(s, seccomp_fd);
    case sp::Backend::Proot: return provider::proot_argv(s);
    case sp::Backend::Landlock:
    case sp::Backend::HomeRedirect: {
        if (s.argv.empty()) return {};
        auto argv = s.argv;
        argv[0] = self;
        return argv;
    }
    default: return {};
    }
}

}  // namespace xlings::confine
