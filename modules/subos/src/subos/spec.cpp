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

void instance_files(std::vector<MountOp>& m, const HomeView& home, const Request& r,
                    const fs::path& etc) {
    const auto dir = r.instance_dir;
    if (r.storage == Storage::Shared) {
        m.push_back({MountKind::Bind, posix(dir / "home"), "/home"});
        m.push_back({MountKind::Bind, posix(dir / "tmp"), "/tmp"});
    } else if (r.storage == Storage::Image) {
        m.push_back({MountKind::Bind, posix(r.image_mountpoint), "/home"});
    }
    // The xlings home at its own absolute path: xvm alias targets, RPATH and
    // INTERP are absolute host paths baked at install time.
    m.push_back({MountKind::Bind, posix(home.home), posix(home.home)});
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
        instance_files(s.mounts, home, r, etc_dir);
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
        instance_files(s.mounts, home, shared, etc_dir);
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
        for (auto dim : {"pid", "ipc", "terminal"})
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
    s.env["HOME"] = user_home;
    s.env["XLINGS_HOME"] = posix(home.home);
    s.env["PATH"] = std::format("{0}/subos/{1}/bin:{0}/bin:/usr/local/bin:/usr/bin:/bin",
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
