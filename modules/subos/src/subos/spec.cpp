module xlings.subos.spec;

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.caps;
import xlings.subos.gpu;

namespace xlings::subos::spec {

std::string_view to_string(Backend b) {
    switch (b) {
    case Backend::Bwrap:        return "bwrap";
    case Backend::Proot:        return "proot";
    case Backend::HomeRedirect: return "home-redirect";
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

bool exists(const Request& r, std::string_view p) {
    if (r.host_exists) return r.host_exists(p);
    std::error_code ec;
    return fs::exists(fs::path(p), ec);
}

// The host userland every POSIX sandbox needs read-only, and nothing else
// of /etc: the loader cache, DNS, certificates, the zone file.
void host_userland(std::vector<MountOp>& m, const Request& r, bool host_time = true) {
    auto try_ro = [&](const char* src, const char* dst) {
        if (exists(r, src)) m.push_back({MountKind::RoBind, src, dst});
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

// The instance's own files at the places a POSIX userland looks for them.
// A POSIX sandbox's paths are POSIX paths, whichever host compiled the spec
// (the goldens run on Windows too).
std::string posix(const fs::path& p) { return p.generic_string(); }

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
void home_view_ro(std::vector<MountOp>& m, const HomeView& home, const Request& r) {
    const auto h = posix(home.home);
    m.push_back({MountKind::RoBind, h, h});
    m.push_back({MountKind::Tmpfs, "", posix(home.subos_root())});
    m.push_back({MountKind::Bind, posix(r.instance_dir), posix(home.instance(r.instance))});
    for (auto dir : {"logs", "run", "state"})
        m.push_back({MountKind::Tmpfs, "", posix(home.home / dir)});
}

void instance_files(std::vector<MountOp>& m, const HomeView& home, const Request& r,
                    const fs::path& etc, bool read_only_home) {
    const auto dir = r.instance_dir;
    if (r.storage == Storage::Shared) {
        m.push_back({MountKind::Bind, posix(dir / "home"), "/home"});
        m.push_back({MountKind::Bind, posix(dir / "tmp"), "/tmp"});
    } else if (r.storage == Storage::Image) {
        m.push_back({MountKind::Bind, posix(r.image_mountpoint), "/home"});
    }
    if (read_only_home) home_view_ro(m, home, r);
    else m.push_back({MountKind::Bind, posix(home.home), posix(home.home)});
    for (auto f : {"passwd", "group", "hosts", "nsswitch.conf"})
        m.push_back({MountKind::Bind, posix(etc / f), std::string("/etc/") + f});
}

std::vector<MountOp> gpu_mounts(const Request& r) {
    std::vector<MountOp> m;
    auto args = gpu::passthrough_args([&](const std::string& p) { return exists(r, p); });
    for (std::size_t i = 0; i + 2 < args.size(); i += 3) {
        auto kind = args[i] == "--dev-bind" ? MountKind::DevBind : MountKind::RoBind;
        m.push_back({kind, args[i + 1], args[i + 2]});
    }
    return m;
}

// A host path a mount may not use (design §11): the system's own trees, the
// sandbox's machinery, and the xlings home or anything above it.
std::optional<std::string> mount_refusal(const std::string& src, const std::string& dst,
                                         const HomeView& home) {
    auto norm = [](const std::string& p) { return fs::path(p).lexically_normal().generic_string(); };
    const auto s = norm(src), d = norm(dst);
    const auto h = posix(home.home.lexically_normal());
    if (s.empty() || s.front() != '/') return "the host path must be absolute";
    if (d.empty() || d.front() != '/') return "the path inside must be absolute";
    if (h == s || h.starts_with(s.ends_with('/') ? s : s + "/"))
        return "the xlings home, or a directory above it, cannot be mapped";
    for (std::string_view sys : {"/", "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/proc",
                                 "/dev", "/sys", "/run", "/run/xlings", "/boot"}) {
        if (d == sys) return std::format("{} is the sandbox's own; map below it or elsewhere", sys);
    }
    if (d.starts_with("/run/xlings/") || d.starts_with("/proc/") || d.starts_with("/sys/"))
        return "that path inside is the sandbox's own";
    return std::nullopt;
}

std::string probe_fix(const caps::Backend& b) {
    (void)b;
    return "xlings self doctor --isolation";
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
    j["die_with_parent"] = die_with_parent;
    j["new_session"] = new_session;
    j["block_tiocsti"] = block_tiocsti;
    if (!hostname.empty()) j["hostname"] = hostname;
    j["clear_env"] = clear_env;
    // Names only: values can hold secrets, and this lands in audits.
    j["env"] = nlohmann::json::array();
    for (auto& [k, v] : env) j["env"].push_back(k);
    j["cwd"] = cwd.string();
    j["argv"] = argv;
    j["degraded"] = nlohmann::json::array();
    for (auto& d : degraded)
        j["degraded"].push_back({{"dimension", d.dimension}, {"reason", d.reason}, {"fix", d.fix}});
    return j;
}

std::expected<SandboxSpec, Refusal> compile(const policy::Policy& policy,
                                            const HomeView& home,
                                            const caps::Caps& caps,
                                            const Request& r) {
    SandboxSpec s;
    // A neutral identity (private, locked) is the same for every host: the
    // user is `user`, the host name is the instance's, the clock is UTC and
    // the locale C.UTF-8 (#640 F7). Its passwd/group live in etc-neutral/.
    const bool neutral = policy.identity == policy::Identity::Neutral;
    const auto user = neutral ? std::string("user") : r.user;
    const auto etc_dir = r.instance_dir / (neutral ? "etc-neutral" : "etc");
    const auto user_home = "/home/" + user;
    const auto name = r.instance;
    const auto dir = r.instance_dir;
    const bool gpu = r.grants.contains("gpu") || policy.grants.contains("gpu");

    // ── backend ──────────────────────────────────────────────────────
    if (caps.platform == "macos" || caps.platform == "windows") {
        s.backend = Backend::HomeRedirect;
    } else if (r.preferred == Backend::Fake) {
        s.backend = Backend::Fake;
    } else {
        auto refuse = [](std::string dim, std::string reason, std::string fix) {
            return std::unexpected(Refusal{ .missing = { Unmet{
                std::move(dim), std::move(reason), std::move(fix), policy::Need::Must } } });
        };
        if (r.preferred == Backend::Bwrap) {
            if (!caps.bwrap) return refuse("backend", "bwrap not installed", "xlings install bwrap");
            if (!caps.bwrap->usable)
                return refuse("backend", "bwrap probe failed: " + caps.bwrap->probe_output,
                              probe_fix(*caps.bwrap));
            s.backend = Backend::Bwrap;
        } else if (r.preferred == Backend::Proot) {
            if (!caps.proot) return refuse("backend", "proot not installed", "xlings install proot");
            s.backend = Backend::Proot;
        } else if (caps.bwrap && caps.bwrap->usable) {
            s.backend = Backend::Bwrap;
        } else if (caps.proot) {
            s.backend = Backend::Proot;
        } else {
            return refuse("backend", "no sandbox backend available",
                          "xlings install bwrap (or: xlings install proot)");
        }
        if (r.storage != Storage::Shared && s.backend == Backend::Proot) {
            std::string hint = "xlings install bwrap";
            if (caps.bwrap && !caps.bwrap->usable) hint = probe_fix(*caps.bwrap);
            return refuse("storage",
                          std::string(to_string(r.storage))
                              + " storage requires bwrap (proot does not support mount namespace)",
                          hint);
        }
        s.backend_bin = s.backend == Backend::Bwrap ? caps.bwrap->bin
                      : s.backend == Backend::Proot ? caps.proot->bin : fs::path{};
    }

    // ── filesystem ───────────────────────────────────────────────────
    if (s.backend == Backend::Bwrap || s.backend == Backend::Fake) {
        s.mounts.push_back({MountKind::Dev, "", "/dev"});
        s.mounts.push_back({MountKind::Proc, "", "/proc"});
        host_userland(s.mounts, r, !neutral);
        instance_files(s.mounts, home, r, etc_dir, /*read_only_home=*/true);
        if (gpu) {
            auto g = gpu_mounts(r);
            s.mounts.insert(s.mounts.end(), g.begin(), g.end());
        }
        if (r.storage == Storage::Tmpfs) {
            s.mounts.push_back({MountKind::Tmpfs, "", user_home});
            s.mounts.push_back({MountKind::Tmpfs, "", "/tmp"});
        }
        if (r.storage == Storage::Image) s.mounts.push_back({MountKind::Tmpfs, "", "/tmp"});
    } else if (s.backend == Backend::Proot) {
        // proot passes /proc, /sys and /dev through whole; --gpu is a no-op.
        s.mounts.push_back({MountKind::Bind, "/proc", "/proc"});
        s.mounts.push_back({MountKind::Bind, "/sys", "/sys"});
        s.mounts.push_back({MountKind::Bind, "/dev", "/dev"});
        Request shared = r;
        shared.storage = Storage::Shared;
        host_userland(s.mounts, shared, !neutral);
        // proot binds read-write only: the home stays writable, and says so.
        instance_files(s.mounts, home, shared, etc_dir, /*read_only_home=*/false);
        s.proot_root = dir / "sandbox-root";
    }

    // ── processes and terminal (S0, design §16) ──────────────────────
    if (s.backend == Backend::Bwrap || s.backend == Backend::Fake) {
        s.unshare_pid = true;      // host processes invisible (F4)
        s.unshare_ipc = true;
        s.unshare_uts = true;      // hostname changes stay inside
        s.die_with_parent = true;  // nothing outlives the session's owner
        // TIOCSTI from inside would type into the user's shell (F8). A
        // non-interactive run gets a new session (no controlling terminal);
        // an interactive one keeps its terminal for job control and gets the
        // seccomp filter instead.
        if (r.interactive) s.block_tiocsti = true;
        else s.new_session = true;
    } else if (s.backend == Backend::Proot) {
        for (auto dim : {"fs", "pid", "ipc", "terminal"})
            s.degraded.push_back({dim, "proot has no namespaces", "xlings self doctor --isolation",
                                  policy::Need::Should});
    }

    // ── network, identity, nested namespaces (design §19) ────────────
    // What the policy asks for and this backend cannot give is collected;
    // Must-items refuse below, Should-items degrade with a reason.
    std::vector<Unmet> unmet;
    auto need_of = [&](std::string_view dim) {
        if (policy.no_degrade) return policy::Need::Must;
        auto it = policy.needs.find(dim);
        return it == policy.needs.end() ? policy::Need::Should : it->second;
    };
    const bool kernel = s.backend == Backend::Bwrap || s.backend == Backend::Fake;
    if (policy.net == policy::Net::None) {
        if (kernel) s.unshare_net = true;
        else unmet.push_back({"net", std::string(to_string(s.backend)) + " cannot isolate the network",
                              "xlings self doctor --isolation", need_of("net")});
    } else if (policy.net == policy::Net::Nat || policy.net == policy::Net::Proxy) {
        if (kernel && policy.net == policy::Net::Nat && caps.pasta) {
            s.unshare_net = true;
            s.net_nat = true;
            s.pasta_bin = *caps.pasta;
            s.host_loopback = policy.grants.contains("host-loopback") || r.grants.contains("host-loopback");
            s.publish = r.publish;
        } else if (!kernel) {
            unmet.push_back({"net", std::string(to_string(s.backend)) + " cannot isolate the network",
                             "xlings self doctor --isolation", need_of("net")});
        } else if (policy.net == policy::Net::Nat) {
            unmet.push_back({"net", "net=nat needs pasta: " + (caps.pasta_missing.empty()
                                 ? std::string("pasta (passt) is not installed") : caps.pasta_missing),
                             "install passt (e.g. apt install passt), or --net none for no network",
                             need_of("net")});
        } else {
            unmet.push_back({"net", "net=proxy is not implemented yet",
                             "--net nat or --net none", need_of("net")});
        }
    }
    if (!r.publish.empty() && !s.net_nat)
        unmet.push_back({"publish", "--publish needs net=nat (a private network to publish from)",
                         "--net nat", need_of("publish")});
    if (neutral) {
        if (kernel) s.hostname = name;
        else unmet.push_back({"identity", "the host name cannot be changed without namespaces", "",
                              need_of("identity")});
    }
    if (policy.disable_userns) {
        if (s.backend == Backend::Bwrap || s.backend == Backend::Fake) {
            s.unshare_user = true;
            s.disable_userns = true;
        } else {
            unmet.push_back({"userns", "nested user namespaces cannot be forbidden here", "",
                             need_of("userns")});
        }
    }
    if (!kernel) {
        for (auto& d : s.degraded) d.need = need_of(d.dimension);
        for (auto& d : s.degraded) unmet.push_back(d);
        s.degraded.clear();
        if (s.backend == Backend::HomeRedirect)
            unmet.push_back({"fs", "this platform redirects the home directory only (advisory)",
                             "not implemented on this platform yet", need_of("fs")});
    }
    // ── mounts (design §11) and named grants (§21.2) ─────────────────
    // Each grant opens exactly one thing: a socket file bound in, its
    // variable pointed at it -- never the host's whole runtime directory.
    std::map<std::string, std::string> grant_env;
    for (const auto& m : policy.mounts) {
        const auto dst = m.dst.empty() ? m.src : m.dst;
        if (auto why = mount_refusal(m.src, dst, home)) {
            unmet.push_back({"mount", m.src + ": " + *why, "", policy::Need::Must});
            continue;
        }
        if (!exists(r, m.src)) {
            unmet.push_back({"mount", m.src + ": does not exist on the host", "", policy::Need::Must});
            continue;
        }
        if (!m.rw && !kernel)
            unmet.push_back({"mount", m.src + ": read-only needs bwrap; mapped read-write",
                             "xlings self doctor --isolation", need_of("fs")});
        s.mounts.push_back({m.rw ? MountKind::Bind : MountKind::RoBind, posix(m.src), posix(dst)});
    }
    {
        std::set<std::string, std::less<>> grants = policy.grants;
        grants.insert(r.grants.begin(), r.grants.end());
        auto host_env = [&](std::string_view k) -> std::string {
            auto it = r.host_env.find(std::string(k));
            return it == r.host_env.end() ? std::string{} : it->second;
        };
        const std::string runtime = "/tmp/.xlings-runtime";
        bool used_runtime = false;
        auto bind_socket = [&](const std::string& src, const std::string& dst) {
            s.mounts.push_back({MountKind::Bind, src, dst});
        };
        if (grants.contains("display")) {
            bool any = false;
            if (auto d = host_env("DISPLAY"); !d.empty() && exists(r, "/tmp/.X11-unix")) {
                bind_socket("/tmp/.X11-unix", "/tmp/.X11-unix");
                grant_env["DISPLAY"] = d;
                if (auto xa = host_env("XAUTHORITY"); !xa.empty() && exists(r, xa)) {
                    s.mounts.push_back({MountKind::RoBind, xa, "/tmp/.xlings-xauthority"});
                    grant_env["XAUTHORITY"] = "/tmp/.xlings-xauthority";
                }
                any = true;
            }
            if (auto w = host_env("WAYLAND_DISPLAY"), xdg = host_env("XDG_RUNTIME_DIR");
                !w.empty() && !xdg.empty() && exists(r, xdg + "/" + w)) {
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
            if (!xdg.empty() && exists(r, xdg + "/pulse/native")) {
                bind_socket(xdg + "/pulse/native", runtime + "/pulse/native");
                grant_env["PULSE_SERVER"] = "unix:" + runtime + "/pulse/native";
                used_runtime = any = true;
            }
            if (!xdg.empty() && exists(r, xdg + "/pipewire-0")) {
                bind_socket(xdg + "/pipewire-0", runtime + "/pipewire-0");
                used_runtime = any = true;
            }
            if (!any) unmet.push_back({"audio", "no PulseAudio or PipeWire socket on this host", "", policy::Need::Should});
        }
        if (grants.contains("camera")) {
            bool any = false;
            for (int i = 0; i < 10; ++i) {
                auto dev = std::format("/dev/video{}", i);
                if (exists(r, dev)) { s.mounts.push_back({MountKind::DevBind, dev, dev}); any = true; }
            }
            if (!any) unmet.push_back({"camera", "no /dev/video* on this host", "", policy::Need::Should});
        }
        if (grants.contains("ssh-agent")) {
            if (auto sock = host_env("SSH_AUTH_SOCK"); !sock.empty() && exists(r, sock)) {
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
            if (!path.empty() && exists(r, path)) {
                bind_socket(path, "/tmp/.xlings-dbus");
                grant_env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/tmp/.xlings-dbus";
            } else {
                unmet.push_back({"dbus", addr.empty() ? "no session bus on this host"
                                                      : "the session bus is not a socket file (abstract address)",
                                 "", policy::Need::Should});
            }
        }
        if (used_runtime) grant_env["XDG_RUNTIME_DIR"] = runtime;
    }

    Refusal refusal;
    for (auto& u : unmet) {
        if (u.need == policy::Need::Must) refusal.missing.push_back(u);
        else s.degraded.push_back(u);
    }
    if (!refusal.missing.empty()) return std::unexpected(std::move(refusal));

    // ── environment ──────────────────────────────────────────────────
    s.clear_env = !policy.env_inherit;
    if (s.clear_env) {
        std::vector<std::string> pass(policy::kBaseEnvPass.begin(), policy::kBaseEnvPass.end());
        pass.insert(pass.end(), policy.env_pass.begin(), policy.env_pass.end());
        for (auto& [k, v] : r.host_env)
            if (policy::env_name_matches(k, pass)) s.env[k] = v;
    }
    s.env["XLINGS_ACTIVE_SUBOS"] = name;
    s.env["XLINGS_SUBOS_MODE"] = "sandbox";
    const bool windows = caps.platform == "windows";
    auto native = [&](const fs::path& p) { return windows ? p.string() : posix(p); };
    s.env["XLINGS_SUBOS_LIB"] = native(dir / "lib");

    if (s.backend == Backend::HomeRedirect) {
        const auto sandbox_home = native(dir / "home" / r.user);
        const auto sandbox_tmp = native(dir / "tmp");
        if (caps.platform == "windows") {
            s.env["USERPROFILE"] = sandbox_home;
            s.env["APPDATA"] = sandbox_home + "\\AppData\\Roaming";
            s.env["LOCALAPPDATA"] = sandbox_home + "\\AppData\\Local";
            s.env["TEMP"] = sandbox_tmp;
            s.env["TMP"] = sandbox_tmp;
            s.env["XDG_CONFIG_HOME"] = sandbox_home + "\\.config";
            s.env["XDG_DATA_HOME"] = sandbox_home + "\\.local\\share";
            s.env["XDG_CACHE_HOME"] = sandbox_home + "\\.cache";
        } else {
            s.env["HOME"] = sandbox_home;
            s.env["TMPDIR"] = sandbox_tmp;
            s.env["XDG_CONFIG_HOME"] = sandbox_home + "/.config";
            s.env["XDG_DATA_HOME"] = sandbox_home + "/.local/share";
            s.env["XDG_CACHE_HOME"] = sandbox_home + "/.cache";
            s.env["XDG_STATE_HOME"] = sandbox_home + "/.local/state";
        }
        s.cwd = sandbox_home;
        s.argv = r.argv;
        return s;
    }

    if (neutral) {
        s.env["TZ"] = policy.tz.empty() ? "UTC" : policy.tz;
        s.env["LANG"] = "C.UTF-8";
        std::erase_if(s.env, [](const auto& kv) { return kv.first.starts_with("LC_"); });
    }
    for (auto& [k, v] : grant_env) s.env[k] = v;
    s.env["HOME"] = user_home;
    s.env["XLINGS_HOME"] = posix(home.home);
    // /run/xlings last: the client that hosts the session, there whatever the
    // home holds -- every instance has an xlings (design §8). The home's own
    // entry comes first and stays the authority on what dispatches.
    s.env["PATH"] = std::format("{0}/subos/{1}/bin:{0}/bin:/usr/local/bin:/usr/bin:/bin:/run/xlings",
                                posix(home.home), name);
    if (s.backend == Backend::Proot) s.env["PROOT_NO_SECCOMP"] = "1";
    if (s.clear_env) {
        s.env["USER"] = user;
        s.env["LOGNAME"] = user;
        s.env["SHELL"] = r.shell;
    }

    {
        std::vector<std::string> pass(policy::kBaseEnvPass.begin(), policy::kBaseEnvPass.end());
        pass.insert(pass.end(), policy.env_pass.begin(), policy.env_pass.end());
        for (auto& [k, v] : r.explicit_env) {
            if (policy.env_explicit_any || policy::env_name_matches(k, pass)) s.env[k] = v;
            else s.degraded.push_back({"env", "--env " + k + " is not in the policy's env_pass",
                                       "xlings subos config <name> --env-pass " + k,
                                       policy::Need::Should});
        }
    }

    // ── command ──────────────────────────────────────────────────────
    s.cwd = r.cwd.empty() ? fs::path(user_home) : fs::path(r.cwd);
    if (r.argv.empty()) {
        s.argv = {r.shell};
        // `sh -i` prints prompts and job-control warnings into a pipe; -i only
        // on a terminal, and never under proot (it did not pass it before).
        if (r.interactive && s.backend != Backend::Proot) s.argv.push_back("-i");
    } else {
        s.argv = r.argv;
    }
    return s;
}

}  // namespace xlings::subos::spec
