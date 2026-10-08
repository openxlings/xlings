module xlings.confine.common;

import std;
import xlings.subos.home_view;
import xlings.subos.intent;
import xlings.subos.policy;
import xlings.subos.spec;
import xlings.subos.gpu;

namespace xlings::confine::common {

namespace policy = xlings::subos::policy;
namespace gpu = xlings::subos::gpu;
using sp::MountKind;

std::string posix(const fs::path& p) { return p.generic_string(); }

void host_userland(std::vector<sp::MountOp>& m, const in::Intent& i, sp::Storage, bool host_time) {
    auto try_ro = [&](const char* src, const char* dst) {
        if (i.exists(src)) m.push_back({MountKind::RoBind, src, dst});
    };
    try_ro("/usr", "/usr");
    try_ro("/bin", "/bin");
    try_ro("/usr/lib", "/lib");
    try_ro("/usr/lib64", "/lib64");
    try_ro("/etc/resolv.conf", "/etc/resolv.conf");
    try_ro("/etc/ld.so.cache", "/etc/ld.so.cache");
    try_ro("/etc/ssl", "/etc/ssl");
    try_ro("/etc/pki", "/etc/pki");
    try_ro("/etc/alternatives", "/etc/alternatives");
    if (host_time) try_ro("/etc/localtime", "/etc/localtime");
}

namespace {

// The xlings home as a sandbox sees it (#640 F1, F6; design §16).
//
// At its own absolute path -- xvm alias targets, RPATH and INTERP are
// absolute host paths baked at install time -- and READ-ONLY: the shims, the
// profile, the payloads and the home config are code the host runs, and a
// sandbox that can write them has escaped. Writable inside it is only this
// instance's own tree. Other instances, the audit (logs/), the sockets
// (run/) and the host facts (state/) are covered by empty tmpfs; config/
// stays readable, so the xlings inside can read its own policy and cannot
// change it.
void home_view_ro(std::vector<sp::MountOp>& m, const HomeView& home, const in::Intent& i) {
    const auto h = posix(home.home);
    m.push_back({MountKind::RoBind, h, h});
    m.push_back({MountKind::Tmpfs, "", posix(home.subos_root())});
    m.push_back({MountKind::Bind, posix(i.instance_dir), posix(home.instance(i.instance))});
    for (auto dir : {"logs", "run", "state"})
        m.push_back({MountKind::Tmpfs, "", posix(home.home / dir)});
}

}  // namespace

void instance_files(std::vector<sp::MountOp>& m, const HomeView& home, const in::Intent& i,
                    sp::Storage storage, bool read_only_home) {
    const auto dir = i.instance_dir;
    if (storage == sp::Storage::Shared) {
        m.push_back({MountKind::Bind, posix(dir / "home"), "/home"});
        m.push_back({MountKind::Bind, posix(dir / "tmp"), "/tmp"});
    } else if (storage == sp::Storage::Image) {
        m.push_back({MountKind::Bind, posix(i.image_mountpoint), "/home"});
    }
    if (read_only_home) home_view_ro(m, home, i);
    else m.push_back({MountKind::Bind, posix(home.home), posix(home.home)});
    for (auto f : {"passwd", "group", "hosts", "nsswitch.conf"})
        m.push_back({MountKind::Bind, posix(i.id.etc_dir / f), std::string("/etc/") + f});
}

std::vector<sp::MountOp> gpu_mounts(const in::Intent& i) {
    std::vector<sp::MountOp> m;
    auto args = gpu::passthrough_args([&](const std::string& p) { return i.exists(p); });
    for (std::size_t k = 0; k + 2 < args.size(); k += 3) {
        auto kind = args[k] == "--dev-bind" ? MountKind::DevBind : MountKind::RoBind;
        m.push_back({kind, args[k + 1], args[k + 2]});
    }
    return m;
}

void proxy_env(sp::SandboxSpec& s) {
    if (!s.net_proxy) return;
    s.env["ALL_PROXY"] = "socks5h://127.0.0.1:1080";
    s.env["all_proxy"] = s.env["ALL_PROXY"];
    for (const auto* key : {"http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "no_proxy", "NO_PROXY"})
        s.env[key] = "";
}

void explicit_env(const in::Intent& i, sp::SandboxSpec& s, std::string_view config_name) {
    for (auto& [k, v] : i.explicit_env) {
        if (i.env.explicit_any || policy::env_name_matches(k, i.env.pass)) s.env[k] = v;
        else s.degraded.push_back({"env", "--env " + k + " is not in the policy's env_pass",
                                   std::format("xlings subos config {} --env-pass {}", config_name, k),
                                   policy::Need::Should});
    }
}

// The environment of a view of the host (bwrap, fake, proot).
void view_finish(const in::Intent& i, const HomeView& home, const std::map<std::string, std::string>& grant_env,
                 sp::SandboxSpec& s) {
    if (i.id.neutral) {
        s.env["TZ"] = i.id.tz.empty() ? "UTC" : i.id.tz;
        s.env["LANG"] = "C.UTF-8";
        std::erase_if(s.env, [](const auto& kv) { return kv.first.starts_with("LC_"); });
    }
    for (auto& [k, v] : grant_env) s.env[k] = v;
    s.env["HOME"] = i.id.home_inside;
    s.env["XLINGS_HOME"] = posix(home.home);
    // /run/xlings last: the client that hosts the session, there whatever the
    // home holds -- every instance has an xlings (design §8). The home's own
    // entry comes first and stays the authority on what dispatches.
    s.env["PATH"] = std::format("{0}/subos/{1}/bin:{0}/bin:/usr/local/bin:/usr/bin:/bin:/run/xlings",
                                posix(home.home), i.instance);
    if (s.backend == sp::Backend::Proot) s.env["PROOT_NO_SECCOMP"] = "1";
    if (s.clear_env) {
        s.env["USER"] = i.id.user;
        s.env["LOGNAME"] = i.id.user;
        s.env["SHELL"] = i.shell;
    }
    explicit_env(i, s, "<name>");
    s.cwd = i.cwd.empty() ? fs::path(i.id.home_inside) : fs::path(i.cwd);
    if (i.argv.empty()) {
        s.argv = {i.shell};
        // `sh -i` prints prompts and job-control warnings into a pipe; -i only
        // on a terminal, and never under proot (it did not pass it before).
        if (i.interactive && s.backend != sp::Backend::Proot) s.argv.push_back("-i");
    } else {
        s.argv = i.argv;
    }
    proxy_env(s);
}

}  // namespace xlings::confine::common
