module xlings.confine.linux_bwrap;

import std;
import xlings.confine.implementation;
import xlings.confine.common;
import xlings.subos.home_view;
import xlings.subos.caps;
import xlings.subos.intent;
import xlings.subos.policy;
import xlings.subos.spec;
import xlings.subos.root_etc;

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
        // persist home / none: the root's own state seen through a memory
        // overlay, so what the session writes is gone when it ends, and a /tmp
        // of its own. All of it BEFORE the store's binds: those land under the
        // xlings home (/home/<user>/.xlings, or anywhere under /tmp), and an
        // overlay mounted after them hid them -- no /usr, no /bin/sh.
        if (i.persist != sp::Persist::All) {
            std::vector<std::string> volatile_dirs{"etc", "var", "root", "opt", "srv"};
            if (i.persist == sp::Persist::None) volatile_dirs.push_back("home");
            for (const auto& d : volatile_dirs) {
                std::error_code ec;
                if (fs::is_directory(i.root / d, ec))
                    s.mounts.push_back({MountKind::TmpOverlay, posix(i.root / d), "/" + d});
            }
            s.mounts.push_back({MountKind::Tmpfs, "", "/tmp"});
        }
        s.mounts.insert(s.mounts.end(), i.root_mounts.begin(), i.root_mounts.end());
        if (i.net.mode != policy::Net::None && i.exists("/etc/resolv.conf"))
            s.mounts.push_back({MountKind::RoBind, "/etc/resolv.conf", "/etc/resolv.conf"});
        s.unshare_user = true;
        s.uid = i.root_login.uid;
        s.gid = i.root_login.gid;
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
    // TIOCSTI from inside would type into the user's shell (F8): a new
    // session (no controlling terminal) and the seccomp filter, interactive
    // or not. An interactive command has a terminal the session makes for it
    // (session_init), so it keeps job control; and a terminal entering the
    // environment is one more peer of the same session, whatever came first
    // (Luban 2026-10-18 design §12).
    s.new_session = true;
    s.block_tiocsti = true;
}

}  // namespace

namespace {

void root_finish(const in::Intent& i, const HomeView& home, const std::map<std::string, std::string>& grant_env,
                 sp::SandboxSpec& s) {
    // The root's own userland and its own root user. /run/xlings last: the
    // client hosting the session, whatever the root holds.
    if (i.id.neutral) s.env["TZ"] = i.id.tz.empty() ? "UTC" : i.id.tz;
    // The root's own userland has the locale its glibc builds in, not the
    // host's: a host's LANG / LC_* named one it does not have (bash:
    // "setlocale: LC_ALL: cannot change locale"). The same as a machine.
    s.env["LANG"] = "C.UTF-8";
    std::erase_if(s.env, [](const auto& kv) { return kv.first.starts_with("LC_") || kv.first == "LANGUAGE"; });
    for (auto& [k, v] : grant_env) s.env[k] = v;
    s.env.erase("XLINGS_ACTIVE_SUBOS");
    s.env.erase("XLINGS_ROOT_LOGIN");
    s.env["XLINGS_ROOT_INSTANCE"] = i.instance;
    s.env["HOME"] = i.root_login.home;
    s.env["USER"] = i.root_login.name;
    s.env["LOGNAME"] = i.root_login.name;
    // The root's own login shell (design §7): its passwd says, not the host.
    const auto shell = xlings::subos::root_etc::login_shell(i.root, i.root_login.name);
    s.env["SHELL"] = shell;
    s.env["XLINGS_HOME"] = posix(home.home);
    s.env["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin:/run/xlings";
    common::explicit_env(i, s, i.instance);
    s.cwd = i.cwd.empty() ? fs::path(i.root_login.home) : fs::path(i.cwd);
    if (i.argv.empty()) {
        s.argv = {shell};
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
