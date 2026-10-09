module xlings.confine.linux_proot;

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

Implementation linux_proot() {
    Implementation impl;
    impl.backend = sp::Backend::Proot;
    impl.name = "linux-proot";
    impl.available = [](const Caps& caps, const in::Intent& i, fs::path& chosen) -> std::optional<sp::Unmet> {
        if (!caps.proot) return sp::Unmet{"backend", "proot not installed", "xlings install proot", policy::Need::Must};
        if (i.storage != sp::Storage::Shared) {
            std::string hint = "xlings install bwrap";
            if (caps.bwrap && !caps.bwrap->usable) hint = "xlings self doctor --isolation";
            return sp::Unmet{"storage",
                             std::string(sp::to_string(i.storage))
                                 + " storage requires bwrap (proot does not support mount namespace)",
                             hint, policy::Need::Must};
        }
        chosen = caps.proot->bin;
        return std::nullopt;
    };
    impl.view = [](const in::Intent& i, const HomeView& home, sp::SandboxSpec& s) -> std::optional<sp::Unmet> {
        // proot passes /proc, /sys and /dev through whole; --gpu is a no-op.
        s.mounts.push_back({MountKind::Bind, "/proc", "/proc"});
        s.mounts.push_back({MountKind::Bind, "/sys", "/sys"});
        s.mounts.push_back({MountKind::Bind, "/dev", "/dev"});
        common::host_userland(s.mounts, i, sp::Storage::Shared, !i.id.neutral);
        // proot binds read-write only: the home stays writable, and says so.
        common::instance_files(s.mounts, home, i, sp::Storage::Shared, /*read_only_home=*/false);
        s.proot_root = i.instance_dir / "sandbox-root";
        return std::nullopt;
    };
    impl.processes = [](const in::Intent&, sp::SandboxSpec& s) {
        for (auto dim : {"fs", "pid", "ipc", "terminal"})
            s.degraded.push_back({dim, "proot has no namespaces", "xlings self doctor --isolation",
                                  policy::Need::Should});
    };
    impl.finish = [](const in::Intent& i, const HomeView& home, const Caps&,
                     const std::map<std::string, std::string>& grant_env, sp::SandboxSpec& s) {
        common::view_finish(i, home, grant_env, s);
    };
    impl.gates = [](const Caps& c) -> std::vector<GateClaim> {
        if (c.platform != "linux" || !c.proot) return {};
        return {{"FsGate", true, Enforced::Advisory, "proot (ptrace) is a view, not a security boundary",
                 "xlings self doctor --isolation"}};
    };
    return impl;
}

}  // namespace xlings::confine::backends
