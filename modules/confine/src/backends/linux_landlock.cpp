module xlings.confine.linux_landlock;

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
using common::posix;

Implementation linux_landlock() {
    Implementation impl;
    impl.backend = sp::Backend::Landlock;
    impl.name = "linux-landlock";
    impl.sockets = Implementation::Sockets::PassVariables;
    impl.mounts = Implementation::Mounts::Fence;
    impl.available = [](const Caps& caps, const in::Intent& i, fs::path&) -> std::optional<sp::Unmet> {
        // Only when asked for: it restricts writes and hides nothing, so it
        // never stands in for bwrap on its own.
        if (caps.landlock_abi < 1)
            return sp::Unmet{"backend", "Landlock is not available on this kernel "
                                        "(5.13 or later, with landlock in the LSM list)",
                             "xlings self doctor --isolation", policy::Need::Must};
        if (i.storage != sp::Storage::Shared)
            return sp::Unmet{"storage", std::string(sp::to_string(i.storage))
                                            + " storage needs a mount namespace (bwrap)",
                             "--sandbox bwrap", policy::Need::Must};
        return std::nullopt;
    };
    impl.view = [](const in::Intent& i, const HomeView&, sp::SandboxSpec& s) -> std::optional<sp::Unmet> {
        // Its own tree (home and tmp included) and devices (a terminal,
        // /dev/null, /dev/shm). Not the host's /tmp: a rule can only allow,
        // so allowing /tmp would allow every home that lives under it.
        // `--mount /tmp` opens it on purpose; --mount adds its sources.
        s.landlock_rw = {i.instance_dir, "/dev", "/proc"};
        return std::nullopt;
    };
    impl.processes = [](const in::Intent&, sp::SandboxSpec& s) {
        s.degraded.push_back({"fs", "Landlock restricts writes; the host's files stay visible",
                              "--sandbox bwrap", policy::Need::Should});
        // Not a boundary for untrusted code: a session bus or an agent socket
        // runs things on the host for whoever connects.
        s.degraded.push_back({"sockets", "the host's unix sockets (D-Bus, agents, other sessions) "
                                         "stay reachable -- a write fence, not a boundary for untrusted code",
                              "--sandbox bwrap", policy::Need::Should});
        for (auto dim : {"pid", "ipc", "terminal"})
            s.degraded.push_back({dim, "Landlock has no namespaces", "--sandbox bwrap", policy::Need::Should});
    };
    impl.finish = [](const in::Intent& i, const HomeView& home, const Caps&,
                     const std::map<std::string, std::string>& grant_env, sp::SandboxSpec& s) {
        // The host's filesystem with the instance's home and binaries first.
        const auto sandbox_home = posix(i.instance_dir / "home" / i.id.login);
        for (auto& [k, v] : grant_env) s.env[k] = v;
        s.env["HOME"] = sandbox_home;
        s.env["XLINGS_HOME"] = posix(home.home);
        s.env["XDG_CONFIG_HOME"] = sandbox_home + "/.config";
        s.env["XDG_DATA_HOME"] = sandbox_home + "/.local/share";
        s.env["XDG_CACHE_HOME"] = sandbox_home + "/.cache";
        s.env["XDG_STATE_HOME"] = sandbox_home + "/.local/state";
        s.env["TMPDIR"] = posix(i.instance_dir / "tmp");
        s.env["PATH"] = std::format("{0}/subos/{1}/bin:{0}/bin:/usr/local/bin:/usr/bin:/bin",
                                    posix(home.home), i.instance);
        s.env["XLINGS_BROKER_SOCKET"] = posix(home.broker_socket(i.instance));
        if (s.clear_env) {
            s.env["USER"] = i.id.user;
            s.env["LOGNAME"] = i.id.user;
            s.env["SHELL"] = i.shell;
        }
        s.cwd = sandbox_home;
        s.argv = i.argv.empty() ? std::vector<std::string>{i.shell} : i.argv;
        if (i.argv.empty() && i.interactive) s.argv.push_back("-i");
        common::proxy_env(s);
    };
    impl.gates = [](const Caps& c) -> std::vector<GateClaim> {
        if (c.platform != "linux" || c.landlock_abi <= 0) return {};
        return {{"FsGate", true, Enforced::Kernel,
                 std::format("Landlock ABI {}: writes restricted, host files visible (--sandbox landlock)",
                             c.landlock_abi),
                 "xlings self doctor --isolation"}};
    };
    return impl;
}

}  // namespace xlings::confine::backends
