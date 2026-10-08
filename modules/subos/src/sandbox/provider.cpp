module xlings.subos.provider;

import std;
import xlings.subos.spec;
import xlings.subos.session;

namespace xlings::subos::provider {

using spec::MountKind;

std::vector<std::string> bwrap_argv(const spec::SandboxSpec& s, std::optional<int> seccomp_fd) {
    std::vector<std::string> a{ s.backend_bin.generic_string() };
    if (s.unshare_user) a.push_back("--unshare-user");
    if (s.uid) a.insert(a.end(), {"--uid", std::to_string(*s.uid)});
    if (s.gid) a.insert(a.end(), {"--gid", std::to_string(*s.gid)});
    if (s.disable_userns) a.push_back("--disable-userns");
    if (s.unshare_pid) a.push_back("--unshare-pid");
    if (s.unshare_ipc) a.push_back("--unshare-ipc");
    if (s.unshare_uts) a.push_back("--unshare-uts");
    // nat: the network namespace already exists, pasta attached to it before
    // bwrap starts (session::host); bwrap runs inside it, it does not make one.
    if (s.unshare_net && !s.net_nat && !s.net_proxy) a.push_back("--unshare-net");
    if (!s.hostname.empty()) a.insert(a.end(), {"--hostname", s.hostname});
    if (s.die_with_parent) a.push_back("--die-with-parent");
    if (s.new_session) a.push_back("--new-session");
    for (const auto& m : s.mounts) {
        switch (m.kind) {
        case MountKind::Dev:     a.insert(a.end(), {"--dev", m.dst}); break;
        case MountKind::Proc:    a.insert(a.end(), {"--proc", m.dst}); break;
        case MountKind::Tmpfs:   a.insert(a.end(), {"--tmpfs", m.dst}); break;
        case MountKind::Dir:     a.insert(a.end(), {"--dir", m.dst}); break;
        case MountKind::RoBind:  a.insert(a.end(), {"--ro-bind", m.src, m.dst}); break;
        case MountKind::Bind:    a.insert(a.end(), {"--bind", m.src, m.dst}); break;
        case MountKind::DevBind: a.insert(a.end(), {"--dev-bind", m.src, m.dst}); break;
        }
    }
    if (seccomp_fd) a.insert(a.end(), {"--seccomp", std::to_string(*seccomp_fd)});
    a.insert(a.end(), {"--chdir", s.cwd.generic_string(), "--"});
    a.insert(a.end(), s.argv.begin(), s.argv.end());
    return a;
}

std::vector<std::string> proot_argv(const spec::SandboxSpec& s) {
    std::vector<std::string> a{ s.backend_bin.generic_string(), "-r", s.proot_root.generic_string() };
    for (const auto& m : s.mounts) {
        if (m.kind == MountKind::RoBind || m.kind == MountKind::Bind || m.kind == MountKind::DevBind)
            a.push_back(std::format("--bind={}:{}", m.src, m.dst));
    }
    a.push_back(std::format("--cwd={}", s.cwd.generic_string()));
    a.insert(a.end(), s.argv.begin(), s.argv.end());
    return a;
}

std::map<std::string, std::string> process_env(const spec::SandboxSpec& s,
                                               const std::map<std::string, std::string>& inherited) {
    std::map<std::string, std::string> env;
    if (!s.clear_env) env = inherited;
    for (const auto& [k, v] : s.env) env[k] = v;
    if (s.backend == spec::Backend::Landlock) {
        std::string rw;
        for (const auto& p : s.landlock_rw) rw += p.generic_string() + "\n";
        env[std::string(session::kLandlockRwEnv)] = rw;
    }
    return env;
}

}  // namespace xlings::subos::provider

namespace xlings::subos::provider {

std::vector<std::string> pasta_args(const spec::SandboxSpec& s) {
    std::vector<std::string> a{ s.pasta_bin.string(), "--config-net", "--quiet" };
    // Nothing in, unless published; nothing from the sandbox to the host's
    // own loopback, unless granted (design §19, §21.2).
    if (s.publish.empty()) a.insert(a.end(), {"-t", "none"});
    for (const auto& p : s.publish) a.insert(a.end(), {"-t", p});
    a.insert(a.end(), {"-u", "none"});
    if (!s.host_loopback) a.insert(a.end(), {"-T", "none", "-U", "none", "--no-map-gw"});
    return a;
}

}  // namespace xlings::subos::provider
