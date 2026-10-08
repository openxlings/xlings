module xlings.confine.linux_bwrap;

import std;
import xlings.confine.implementation;
import xlings.confine.common;
import xlings.subos.home_view;
import xlings.subos.caps;
import xlings.subos.intent;
import xlings.subos.policy;
import xlings.subos.spec;

namespace xlings::confine::backends {

namespace policy = xlings::subos::policy;
using sp::MountKind;
using common::posix;

namespace {

std::optional<sp::Unmet> namespaces_view(const in::Intent& i, const HomeView& home, sp::SandboxSpec& s) {
    if (!i.root.empty()) {
        // The instance's own tree is `/` (design part 2 §6.1): its /usr is
        // the projection, its /etc, /root, /var are its machine state. The
        // home is where it always is, read-only -- the projection's links and
        // every payload's RPATH name it -- with this instance writable at its
        // own path and, nested, as the root's `default` (§3.3): the xlings
        // inside is the package manager of this root.
        s.mounts.push_back({MountKind::Bind, posix(i.root), "/"});
        s.mounts.push_back({MountKind::Dev, "", "/dev"});
        s.mounts.push_back({MountKind::Proc, "", "/proc"});
        if (i.root_mounts.empty() || std::ranges::any_of(i.root_mounts, [](const sp::MountOp& mount) {
                return mount.kind != MountKind::RoBind;
            }))
            return sp::Unmet{"root", "a root needs an owner-checked read-only store closure",
                             "run owner `xlings install --reconfig` and retry", policy::Need::Must};
        s.mounts.insert(s.mounts.end(), i.root_mounts.begin(), i.root_mounts.end());
        if (i.net.mode != policy::Net::None && i.exists("/etc/resolv.conf"))
            s.mounts.push_back({MountKind::RoBind, "/etc/resolv.conf", "/etc/resolv.conf"});
        s.unshare_user = true;
        s.uid = 0;
        s.gid = 0;
        if (i.gpu) {
            auto g = common::gpu_mounts(i);
            s.mounts.insert(s.mounts.end(), g.begin(), g.end());
        }
        return std::nullopt;
    }
    s.mounts.push_back({MountKind::Dev, "", "/dev"});
    s.mounts.push_back({MountKind::Proc, "", "/proc"});
    common::host_userland(s.mounts, i, i.storage, !i.id.neutral);
    common::instance_files(s.mounts, home, i, i.storage, /*read_only_home=*/true);
    if (i.gpu) {
        auto g = common::gpu_mounts(i);
        s.mounts.insert(s.mounts.end(), g.begin(), g.end());
    }
    if (i.storage == sp::Storage::Tmpfs) {
        s.mounts.push_back({MountKind::Tmpfs, "", i.id.home_inside});
        s.mounts.push_back({MountKind::Tmpfs, "", "/tmp"});
    }
    if (i.storage == sp::Storage::Image) s.mounts.push_back({MountKind::Tmpfs, "", "/tmp"});
    return std::nullopt;
}

void namespaces_processes(const in::Intent& i, sp::SandboxSpec& s) {
    s.unshare_pid = true;      // host processes invisible (F4)
    s.unshare_ipc = true;
    s.unshare_uts = true;      // hostname changes stay inside
    s.die_with_parent = true;  // nothing outlives the session's owner
    // TIOCSTI from inside would type into the user's shell (F8). A
    // non-interactive run gets a new session (no controlling terminal); an
    // interactive one keeps its terminal for job control and gets the
    // seccomp filter instead.
    if (i.interactive) s.block_tiocsti = true;
    else s.new_session = true;
}

}  // namespace

namespace {

void root_finish(const in::Intent& i, const HomeView& home, const std::map<std::string, std::string>& grant_env,
                 sp::SandboxSpec& s) {
    // The root's own userland and its own root user. /run/xlings last: the
    // client hosting the session, whatever the root holds.
    if (i.id.neutral) {
        s.env["TZ"] = i.id.tz.empty() ? "UTC" : i.id.tz;
        s.env["LANG"] = "C.UTF-8";
    }
    for (auto& [k, v] : grant_env) s.env[k] = v;
    s.env.erase("XLINGS_ACTIVE_SUBOS");
    s.env["XLINGS_ROOT_INSTANCE"] = i.instance;
    s.env["HOME"] = "/root";
    s.env["USER"] = "root";
    s.env["LOGNAME"] = "root";
    s.env["SHELL"] = "/bin/sh";
    s.env["XLINGS_HOME"] = posix(home.home);
    s.env["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/run/xlings";
    common::explicit_env(i, s, i.instance);
    s.cwd = i.cwd.empty() ? fs::path("/root") : fs::path(i.cwd);
    if (i.argv.empty()) {
        s.argv = {"/bin/sh"};
        if (i.interactive) s.argv.push_back("-i");
    } else {
        s.argv = i.argv;
    }
    common::proxy_env(s);
}

Implementation namespaces(sp::Backend backend, std::string_view name) {
    Implementation impl;
    impl.backend = backend;
    impl.name = name;
    impl.kernel = true;
    impl.presents_root = true;
    impl.view = namespaces_view;
    impl.processes = namespaces_processes;
    impl.finish = [](const in::Intent& i, const HomeView& home, const Caps&,
                     const std::map<std::string, std::string>& grant_env, sp::SandboxSpec& s) {
        if (!i.root.empty()) root_finish(i, home, grant_env, s);
        else common::view_finish(i, home, grant_env, s);
    };
    return impl;
}

}  // namespace

Implementation linux_bwrap() {
    auto impl = namespaces(sp::Backend::Bwrap, "linux-bwrap");
    impl.available = [](const Caps& caps, const in::Intent&, fs::path& chosen) -> std::optional<sp::Unmet> {
        if (!caps.bwrap) return sp::Unmet{"backend", "bwrap not installed", "xlings install bwrap", policy::Need::Must};
        if (!caps.bwrap->usable)
            return sp::Unmet{"backend", "bwrap probe failed: " + caps.bwrap->probe_output,
                             "xlings self doctor --isolation", policy::Need::Must};
        chosen = caps.bwrap->bin;
        return std::nullopt;
    };
    impl.gates = [](const Caps& c) -> std::vector<GateClaim> {
        if (c.platform != "linux" || !c.bwrap || !c.bwrap->usable) return {};
        return {
            {"FsGate", true, Enforced::Kernel, "bwrap mount namespace", ""},
            {"ProcessScope", true, Enforced::Kernel, "pid namespace", ""},
            {"NetGate", true, Enforced::Kernel,
             c.pasta ? "net namespace; nat via pasta, proxy via SOCKS5h relay" : "net namespace (host, none, SOCKS5h proxy)", ""},
            {"DeviceGate", true, Enforced::Kernel, "minimal /dev, named grants", ""},
            {"IdentityShim", true, Enforced::Kernel, "uts namespace + passwd template", ""},
            {"ExecTracer", c.seccomp, Enforced::Kernel, "seccomp user notification (observe=full)", ""},
            {"RootfsRuntime", true, Enforced::Kernel, "bwrap root tree with uid 0 mapping", ""},
        };
    };
    return impl;
}

Implementation fake() {
    auto impl = namespaces(sp::Backend::Fake, "fake");
    impl.available = [](const Caps&, const in::Intent&, fs::path&) -> std::optional<sp::Unmet> {
        return std::nullopt;
    };
    return impl;
}

}  // namespace xlings::confine::backends
