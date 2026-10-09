module xlings.subos.session;

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.platform;
import xlings.platform.root_mount;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.broker;
import xlings.subos.network;

// Every system call here is xlings.platform's (:process, :isolation): this
// file is the session's logic -- who talks to whom, what is recorded, when a
// session ends -- and nothing of how a POSIX kernel spells it.
namespace xlings::subos::session {

namespace sig = platform::sig;

nlohmann::json to_json(const Info& i) {
    return {{"instance", i.instance}, {"id", i.id}, {"supervisor_pid", i.supervisor_pid},
            {"sandbox_pid", i.sandbox_pid}, {"started", i.started}, {"backend", i.backend},
            {"digest", i.digest}, {"ttl", i.ttl}, {"detached", i.detached}};
}

Info info_from_json(const nlohmann::json& j) {
    Info i;
    i.instance = j.value("instance", "");
    i.id = j.value("id", "");
    i.supervisor_pid = j.value("supervisor_pid", 0);
    i.sandbox_pid = j.value("sandbox_pid", 0);
    i.started = j.value("started", "");
    i.backend = j.value("backend", "");
    i.digest = j.value("digest", "");
    i.ttl = j.value("ttl", 0);
    i.detached = j.value("detached", false);
    return i;
}

namespace {

// Sessions need a SessionHost (design §17): fork, a local socket that carries
// descriptors, a process that outlives its parent. Where there is none,
// `subos status` says so and every entry point below declines.
// macOS has one too since part 3 (§6.3): fork, and a framed SOCK_STREAM in
// place of SEQPACKET (xlings.platform). Windows has neither yet.
constexpr bool kSessions = platform::is_linux || platform::is_macos;

// ── messages: one JSON object per datagram, descriptors attached ─────

struct Msg {
    nlohmann::json json;
    std::vector<int> fds;
};

bool send_msg(int sock, const nlohmann::json& j, std::span<const int> fds = {}) {
    return platform::send_message(sock, j.dump(), fds);
}

// nullopt on EOF or error; a message that is not JSON is dropped (and its
// descriptors closed) rather than trusted.
std::optional<Msg> recv_msg(int sock) {
    auto m = platform::receive_message(sock);
    if (!m) return std::nullopt;
    Msg out{ nlohmann::json::parse(m->data, nullptr, false), std::move(m->fds) };
    if (out.json.is_discarded() || !out.json.is_object()) {
        platform::close_fds(out.fds);
        out.json = nlohmann::json::object();
    }
    return out;
}

fs::path sock_path(const HomeView& home, std::string_view instance) {
    return home.run_dir(instance) / "exec.sock";
}
fs::path info_path(const HomeView& home, std::string_view instance) {
    return home.run_dir(instance) / "session.json";
}

std::string new_id() {
    std::random_device rd;
    return std::format("{:08x}{:04x}", rd(), rd() & 0xffff);
}

bool write_atomic(const fs::path& path, const std::string& text) {
    auto tmp = fs::path(path.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        out.flush();
        if (!out) return false;
        out.close();
        if (out.fail()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

bool env_allowed(std::string_view name, const std::vector<std::string>& pass) {
    for (const auto& p : pass) {
        if (!p.empty() && p.back() == '*') {
            if (name.starts_with(std::string_view(p).substr(0, p.size() - 1))) return true;
        } else if (name == p) {
            return true;
        }
    }
    return false;
}

void report(std::string_view what) {
    std::println(std::cerr, "[xlings:subos] {}", what);
}

}  // namespace

// ── discovery ────────────────────────────────────────────────────────

std::optional<Info> find(const HomeView& home, std::string_view instance) {
    if constexpr (!kSessions) return std::nullopt;
    auto ip = info_path(home, instance);
    std::error_code ec;
    if (!fs::exists(ip, ec)) return std::nullopt;
    std::ifstream in(ip);
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    auto info = info_from_json(j);
    const bool alive = platform::is_process_alive(info.supervisor_pid);
    if (alive) {
        // The pid could have been reused; the supervisor answering on its
        // socket is the proof.
        if (int fd = platform::unix_connect(sock_path(home, instance)); fd >= 0) {
            platform::close_fd(fd);
            return info;
        }
    } else {
        fs::remove(ip, ec);
        fs::remove(sock_path(home, instance), ec);
    }
    return std::nullopt;
}

std::vector<Info> list(const HomeView& home) {
    std::vector<Info> out;
    auto root = home.home / "run" / "subos";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != std::default_sentinel;
         it.increment(ec)) {
        if (auto i = find(home, it->path().filename().string())) out.push_back(*i);
    }
    std::ranges::sort(out, {}, &Info::instance);
    return out;
}

// ── the supervisor ───────────────────────────────────────────────────

namespace {

// Files changed under `root` since `since`, relative, at most `limit`.
std::pair<std::vector<std::string>, bool> changed_since(const fs::path& root,
                                                        fs::file_time_type since,
                                                        std::size_t limit) {
    std::vector<std::string> out;
    bool truncated = false;
    std::error_code ec;
    std::size_t visited = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        if (++visited > 200000) { truncated = true; break; }
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (it->last_write_time(e2) < since) continue;
        if (out.size() >= limit) { truncated = true; break; }
        out.push_back(it->path().lexically_relative(root).generic_string());
    }
    return {out, truncated};
}

struct BrokerClient {
    int fd { -1 };
    int pid { -1 };
    std::string program;
};

struct Client {
    int fd { -1 };
    std::optional<std::int64_t> exec_id;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool timed_out { false };
    std::string argv0;
    std::chrono::steady_clock::time_point started{};
};

int supervise(const HomeView& home, Launch& L, Info& info, int listen_fd, std::array<int, 2> ctl,
              int ready_fd) {
    struct RootContext {
        Launch& launch;
        std::array<int, 3> descriptors{-1, -1, -1};
        ~RootContext() {
            for (const int descriptor : descriptors)
                platform::close_fd(descriptor);
            if (launch.finalize_root) {
                try { launch.finalize_root(); }
                catch (...) { report("private root view cleanup failed"); }
            }
        }
    } rootContext{L};
    int pid = -1;
    bool root_failed = false;
    bool audit_failed = false;
    bool audit_warned = false;
    std::vector<BrokerClient> brokered;
    auto audit = [&](nlohmann::json fields, observe::Kind kind = observe::Kind::Lifecycle) {
        if (audit_failed) return false;
        fields["instance"] = L.instance;
        observe::trace(kind == observe::Kind::Perm ? "broker" : "session", fields.dump());
        const auto file = home.logs_dir(L.instance) / "events.ndjson";
        const observe::Event event{.kind = kind, .ts = observe::utc_now(), .fields = std::move(fields)};
        auto failed_file = file;
        bool written = observe::append_checked(file, event);
        if (written) {
            failed_file = home.logs_dir(L.instance) / "sessions" / (info.id + ".ndjson");
            written = observe::append_checked(failed_file, event);
        }
        if (!written) {
            if (!audit_warned)
                report(std::format("E_AUDIT_WRITE: cannot write {}; {}", failed_file.string(),
                    L.audit_required ? "locked session refused or terminated"
                                     : "continuing without a complete audit"));
            audit_warned = true;
            if (L.audit_required) {
                audit_failed = true;
                if (pid > 0) platform::send_signal(pid, sig::kill);
                for (auto& b : brokered)
                    if (b.pid > 0) platform::send_signal(b.pid, sig::kill);
            }
        }
        return written || !L.audit_required;
    };
    auto audit_spec = L.spec;
    if (audit_spec.is_object() && audit_spec.contains("argv")) {
        const auto& argv = audit_spec["argv"];
        audit_spec["argc"] = argv.is_array() ? argv.size() : 0;
        if (argv.is_array() && !argv.empty() && argv[0].is_string())
            audit_spec["program"] = argv[0];
        audit_spec.erase("argv");
    }
    if (!audit({{"event", "session-start"}, {"session", info.id},
                {"backend", L.backend}, {"detached", L.detached}, {"ttl", L.ttl},
                {"exec_trace", L.trace_exec}, {"spec", audit_spec}})) {
        platform::close_fd(listen_fd);
        for (int fd : ctl) platform::close_fd(fd);
        platform::close_fds(L.keep_fds);
        platform::close_fd(ready_fd);
        std::error_code ec;
        fs::remove(sock_path(home, L.instance), ec);
        return kExitSetup;
    }

    std::optional<network::Relay> proxyRelay;
    std::optional<std::array<int, 2>> proxyBridge;
    if (!L.proxy_url.empty()) {
        const auto resolved = network::resolve_proxy(L.proxy_url);
        if (!resolved || !(proxyBridge = platform::unix_pair())) {
            report(resolved ? "cannot create the private proxy bridge" : resolved.error());
            (void)audit({{"event", "session-setup-failed"}, {"session", info.id}, {"reason", "proxy"}});
            platform::close_fd(listen_fd);
            for (int fd : ctl) platform::close_fd(fd);
            platform::close_fds(L.keep_fds);
            platform::close_fd(ready_fd);
            std::error_code ec;
            fs::remove(sock_path(home, L.instance), ec);
            return kExitSetup;
        }
        // Only this declared endpoint is resolved on the host. Sandbox target
        // domain names remain SOCKS wire bytes and go to the remote proxy.
        if (!audit({{"event", "proxy-endpoint-resolved"}, {"session", info.id},
                    {"proxy_host", resolved->declared.host}, {"proxy_port", resolved->declared.port},
                    {"resolution", "host-proxy-endpoint-only"}}, observe::Kind::Net)) return kExitSetup;
        proxyRelay.emplace(*resolved);
    }

    // Like system(): while the sandbox runs, Ctrl-C is the command's. SIGTERM
    // and SIGHUP end the session (forwarded to the sandbox).
    const int signal_fd = platform::route_signals({sig::terminate, sig::hangup, sig::child},
                                                  {sig::interrupt, sig::quit, sig::pipe});

    // The broker's socket exists before the backend starts: bwrap binds it
    // into the sandbox at /run/xlings/broker.sock.
    int broker_fd = -1;
    if (L.broker_policy) {
        broker_fd = platform::unix_listen(home.broker_socket(L.instance));
        if (broker_fd < 0)
            report(std::format("the broker could not listen ({}); changes from inside will be refused",
                               platform::error_text(platform::last_error())));
    }

    // net=nat handshake: the child makes its namespaces and says so; pasta
    // attaches; the child is told to go on (1) or to give up (0).
    const bool nat = !L.pasta.empty();
    const bool proxy = proxyRelay.has_value();
    std::optional<std::array<int, 2>> ready_pipe, go_pipe;
    if (proxy) L.env[std::string(kProxyFdEnv)] = std::to_string((*proxyBridge)[1]);
    if (nat) {
        ready_pipe = platform::make_pipe();
        go_pipe = platform::make_pipe();
        if (!ready_pipe || !go_pipe) {
            report(std::format("pipe failed: {}", platform::error_text(platform::last_error())));
            return kExitSetup;
        }
    }
    const auto ids = platform::user_ids();

    pid = platform::fork_process();
    if (pid < 0) {
        report(std::format("fork failed: {}", platform::error_text(platform::last_error())));
        return kExitSetup;
    }
    if (pid == 0) {
        platform::reset_signals();
        platform::close_fd(listen_fd);
        platform::close_fd(ctl[0]);
        platform::close_fd(broker_fd);
        if (proxy) {
            // bwrap makes the network namespace (only lo); session-init runs
            // the gateway inside it on this end of the bridge.
            platform::close_fd((*proxyBridge)[0]);
            platform::set_inheritable((*proxyBridge)[1], true);
        }
        if (nat) {
            // pasta attaches to a namespace made here, before bwrap starts.
            if (!platform::enter_private_network(ids.uid, ids.gid)) platform::exit_now(kExitSetup);
            const char ready = 1;
            (void)platform::write_fd((*ready_pipe)[1], std::string_view(&ready, 1));
            char go = 0;
            if (!platform::read_exact((*go_pipe)[0], &go, 1) || go != 1) platform::exit_now(kExitSetup);
        }
        // The control socket and the caller's descriptors (a seccomp filter)
        // cross exec; nothing else of the supervisor's does.
        platform::set_inheritable(ctl[1], true);
        for (int fd : L.keep_fds) platform::set_inheritable(fd, true);
        const int e = platform::exec_path(L.argv, L.env);
        report(std::format("cannot run {}: {}", L.argv.empty() ? "" : L.argv[0], platform::error_text(e)));
        platform::exit_now(kExitSetup);
    }
    platform::close_fd(ctl[1]);
    for (int fd : L.keep_fds) platform::close_fd(fd);

    if (proxy) platform::close_fd((*proxyBridge)[1]);
    const auto pasta_pidfile = home.run_dir(L.instance) / "pasta.pid";
    if (nat) {
        platform::close_fd((*ready_pipe)[1]);
        platform::close_fd((*go_pipe)[0]);
        platform::PollFd rp{ (*ready_pipe)[0] };
        char b = 0;
        const bool ready = platform::poll_fds(std::span(&rp, 1), 10000) > 0
                           && platform::read_exact((*ready_pipe)[0], &b, 1);
        int pasta_rc = -1;
        if (ready && nat) {
            auto pargv = L.pasta;
            pargv.insert(pargv.end(), {"--pid", pasta_pidfile.string(), std::to_string(pid)});
            const int pp = platform::fork_process();
            if (pp == 0) {
                platform::reset_signals();
                const int null = platform::open_null();
                const int stdio[3] = {null, 1, 2};
                platform::redirect_stdio(stdio);
                (void)platform::exec_path(pargv, platform::environment());
                platform::exit_now(127);
            }
            if (pp > 0)
                if (auto st = platform::wait_process(pp, true)) pasta_rc = platform::exit_code(*st);
        }
        const char go = (ready && pasta_rc == 0) ? 1 : 0;
        (void)platform::write_fd((*go_pipe)[1], std::string_view(&go, 1));
        platform::close_fd((*go_pipe)[1]);
        platform::close_fd((*ready_pipe)[0]);
        if (!go) {
            report(std::format("net=nat: pasta could not set up the network ({})",
                               ready ? std::format("pasta exited {}", pasta_rc)
                                     : std::string("the namespace was not created")));
            audit({{"event", "session-setup-failed"}, {"session", info.id},
                                     {"reason", "pasta"}, {"exit", pasta_rc}});
            if (proxyBridge) platform::close_fd((*proxyBridge)[0]);
            (void)platform::wait_process(pid, true);
            std::error_code rec;
            fs::remove(sock_path(home, L.instance), rec);
            platform::close_fd(listen_fd);
            platform::close_fd(ctl[0]);
            return kExitSetup;
        }
    }

    if (L.refresh_root) {
        platform::PollFd control{ctl[0]};
        auto message = platform::poll_fds(std::span(&control, 1), 10000) > 0
            ? recv_msg(ctl[0]) : std::nullopt;
        const bool valid = message && message->json.value("op", "") == "root-mount-context" &&
                           message->fds.size() == 3;
        std::string namespaceFailure;
        if (valid) {
            std::ranges::copy(message->fds, rootContext.descriptors.begin());
            message->fds.clear();
            if (auto normalized = platform::root_mount::normalize(rootContext.descriptors); !normalized)
                namespaceFailure = normalized.error();
        }
        if (!valid || !namespaceFailure.empty()) {
            const auto reason = !namespaceFailure.empty() ? namespaceFailure :
                message ? message->json.value("error", "invalid namespace descriptors")
                        : "trusted init did not supply namespace descriptors";
            if (message)
                platform::close_fds(message->fds);
            report("cannot supervise root mount refresh: " + reason);
            (void)audit({{"event", "root-refresh-unavailable"}, {"reason", reason},
                         {"session", info.id}});
            platform::send_signal(pid, sig::kill);
            (void)platform::wait_process(pid, true);
            platform::close_fd(listen_fd);
            platform::close_fd(ctl[0]);
            platform::close_fd(broker_fd);
            platform::close_fd(ready_fd);
            if (proxyRelay) proxyRelay->stop();
            if (proxyBridge) platform::close_fd((*proxyBridge)[0]);
            std::error_code ignored;
            if (nat) {
                std::ifstream file(pasta_pidfile);
                int helper = 0;
                if (file >> helper && helper > 0)
                    platform::send_signal(helper, sig::terminate);
                fs::remove(pasta_pidfile, ignored);
            }
            fs::remove(sock_path(home, L.instance), ignored);
            fs::remove(home.broker_socket(L.instance), ignored);
            return kExitSetup;
        }
        for (const int descriptor : rootContext.descriptors)
            platform::set_inheritable(descriptor, false);
    }

    // observe=full: session-init installs the exec filter inside and sends its
    // listener over the control socket (handled in the loop below). Inside,
    // so bwrap itself never runs under no_new_privs -- which would keep an
    // AppArmor profile (self doctor --isolation --fix) from applying to it.
    int net_notify_fd = -1;
    const auto fs_since = fs::file_time_type::clock::now();

    const auto started = std::chrono::steady_clock::now();
    info.sandbox_pid = pid;
    write_atomic(info_path(home, L.instance), to_json(info).dump(2));
    if (ready_fd >= 0) {
        (void)platform::write_fd(ready_fd, "1");
        platform::close_fd(ready_fd);
    }

    std::vector<Client> clients;
    std::int64_t next_id = 1;
    platform::ExitStatus status{};
    bool reaped = false;
    bool ctl_open = true;
    bool timed_out = false;
    std::optional<std::chrono::steady_clock::time_point> kill_at;
    const auto deadline = L.timeout ? std::optional(started + *L.timeout) : std::nullopt;

    auto finish_client = [&](Client& c, nlohmann::json reply) {
        if (c.timed_out) reply = {{"exit", kExitTimeout}, {"timeout", true}};
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - c.started).count();
        nlohmann::json ev{{"event", "exec-end"}, {"session", info.id}, {"exec", *c.exec_id},
                          {"program", c.argv0}, {"ms", ms}};
        for (auto k : {"exit", "signal", "error", "timeout"})
            if (reply.contains(k)) ev[k] = reply[k];
        if (!audit(ev, observe::Kind::Ops))
            reply = {{"error", "E_AUDIT_WRITE: session ended"}, {"phase", "setup"}};
        send_msg(c.fd, reply);
        platform::close_fd(c.fd);
        c.fd = -1;
    };
    auto drop = [](int& fd) { platform::close_fd(fd); fd = -1; };

    while (true) {
        // An audit failure ends admission immediately. Draining the normal
        // control channel can wait on a backend blocked by our listeners.
        if (audit_failed || root_failed) break;
        const auto signals = platform::pending_signals();
        if (!reaped) {
            if (auto st = platform::wait_process(pid, false)) { status = *st; reaped = true; pid = -1; }
        }
        for (int s : signals) {
            if ((s == sig::terminate || s == sig::hangup) && !reaped) platform::send_signal(pid, sig::terminate);
        }
        if (reaped && !ctl_open) break;
        if (reaped) {
            // Drain what session-init said before it went.
            platform::PollFd p{ ctl[0] };
            if (platform::poll_fds(std::span(&p, 1), 0) <= 0) break;
        }

        if (proxyRelay) proxyRelay->tick([&](const network::Event& event) {
            return audit({{"event", event.event}, {"session", info.id}, {"connection", event.connection},
                {"target", event.target}, {"port", event.port}, {"remote_dns", event.remote_dns},
                {"reason", event.reason}, {"mode", "proxy"}}, observe::Kind::Net);
        });
        std::vector<platform::PollFd> pfds{{listen_fd}, {signal_fd}};
        if (proxyBridge && (*proxyBridge)[0] >= 0) pfds.push_back({(*proxyBridge)[0]});
        if (proxyRelay) {
            const auto networkFds = proxyRelay->fds();
            pfds.insert(pfds.end(), networkFds.begin(), networkFds.end());
        }
        if (ctl_open) pfds.push_back({ctl[0]});
        if (broker_fd >= 0) pfds.push_back({broker_fd});
        if (net_notify_fd >= 0) pfds.push_back({net_notify_fd});
        for (auto& b : brokered) if (b.fd >= 0 && b.pid < 0) pfds.push_back({b.fd});
        for (auto& c : clients) if (c.fd >= 0) pfds.push_back({c.fd});
        platform::poll_fds(pfds, 200);

        const auto now = std::chrono::steady_clock::now();
        if (deadline && !timed_out && !reaped && now >= *deadline) {
            timed_out = true;
            platform::send_signal(pid, sig::terminate);
            kill_at = now + std::chrono::seconds(2);
            audit({{"event", "session-timeout"}, {"session", info.id}});
        }
        if (kill_at && !reaped && now >= *kill_at) {
            platform::send_signal(pid, sig::kill);
            kill_at.reset();
        }
        for (auto& c : clients) {
            if (c.fd >= 0 && c.exec_id && c.deadline && !c.timed_out && now >= *c.deadline) {
                c.timed_out = true;
                send_msg(ctl[0], {{"op", "kill"}, {"id", *c.exec_id}});
            }
        }

        // Brokered commands that finished: their exit goes back to the caller.
        for (auto& b : brokered) {
            if (b.pid <= 0) continue;
            auto st = platform::wait_process(b.pid, false);
            if (!st) continue;
            int code = platform::exit_code(*st);
            b.pid = 0;
            std::string refreshError;
            if (code == 0 && L.refresh_root) {
                try {
                    if (auto refreshed = L.refresh_root(rootContext.descriptors); !refreshed)
                        refreshError = refreshed.error();
                } catch (const std::exception& error) {
                    refreshError = error.what();
                } catch (...) {
                    refreshError = "owner root refresh failed";
                }
                if (!refreshError.empty()) {
                    code = kExitSetup;
                    root_failed = true;
                    report("root refresh refused: " + refreshError);
                    (void)audit({{"event", "root-refresh-failed"}, {"session", info.id},
                                 {"reason", refreshError}}, observe::Kind::Ops);
                    if (pid > 0) platform::send_signal(pid, sig::kill);
                    for (const auto& pending : brokered)
                        if (pending.pid > 0) platform::send_signal(pending.pid, sig::kill);
                } else {
                    (void)audit({{"event", "root-refresh-done"}, {"session", info.id}}, observe::Kind::Ops);
                }
            }
            const bool recorded = audit({{"event", "broker-done"}, {"session", info.id},
                                         {"program", b.program}, {"exit", code}}, observe::Kind::Perm);
            auto reply = recorded ? nlohmann::json{{"exit", code}}
                                  : nlohmann::json{{"exit", kExitSetup}, {"error", "E_AUDIT_WRITE: session ended"}};
            if (!refreshError.empty()) reply["error"] = refreshError;
            send_msg(b.fd, reply);
            drop(b.fd);
        }
        std::erase_if(brokered, [](const BrokerClient& b) { return b.fd < 0; });
        if (root_failed || audit_failed) break;

        for (auto& p : pfds) {
            if (audit_failed || root_failed) break;
            if (!p.readable && !p.closed) continue;
            if (proxyBridge && p.fd == (*proxyBridge)[0]) {
                const auto client = platform::receive_message(p.fd, 128);
                if (!client) {
                    audit({{"event", "proxy-gateway-lost"}, {"session", info.id}}, observe::Kind::Net);
                    if (pid > 0) platform::send_signal(pid, sig::kill);
                    drop((*proxyBridge)[0]);
                } else {
                    auto descriptors = client->fds;
                    if (client->data == "proxy-client" && descriptors.size() == 1) {
                        if (!proxyRelay->add(descriptors.front()))
                            audit({{"event", "denied"}, {"session", info.id}, {"mode", "proxy"},
                                   {"reason", "gateway connection limit"}}, observe::Kind::Net);
                        descriptors.clear();
                    }
                    platform::close_fds(descriptors);
                }
                continue;
            }
            if (net_notify_fd >= 0 && p.fd == net_notify_fd) {
                if (p.closed && !p.readable) { drop(net_notify_fd); continue; }
                if (const auto request = platform::net_notify::next(net_notify_fd)) {
                    bool allowed{};
                    if (request->kind == platform::net_notify::Kind::Exec) {
                        allowed = audit({{"event", "exec"}, {"session", info.id},
                            {"path", request->execPath.empty() ? std::string("?") : request->execPath},
                            {"pid", request->pid}}, observe::Kind::Exec);
                    } else {
                        // A destination-less sendmsg may be control traffic;
                        // it remains attempted I/O, never a connection success.
                        allowed = audit({{"event", "net-attempt"}, {"session", info.id},
                            {"mode", proxyRelay ? "proxy" : "nat"}, {"syscall", request->syscall}, {"pid", request->pid},
                            {"address", request->address}, {"port", request->port},
                            {"address_readable", request->address_readable}, {"result", "unknown"}}, observe::Kind::Net);
                    }
                    (void)platform::net_notify::complete(net_notify_fd, request->id, allowed);
                }
                continue;
            }
            if (broker_fd >= 0 && p.fd == broker_fd) {
                if (int c = platform::unix_accept(broker_fd); c >= 0) brokered.push_back({.fd = c});
                continue;
            }
            if (auto bit = std::ranges::find(brokered, p.fd, &BrokerClient::fd); bit != brokered.end()) {
                auto m = recv_msg(bit->fd);
                if (!m) { drop(bit->fd); continue; }
                std::vector<std::string> argv;
                for (auto& a : m->json.value("argv", nlohmann::json::array()))
                    if (a.is_string()) argv.push_back(a.get<std::string>());
                if (argv.empty() || m->fds.size() != 3) {
                    send_msg(bit->fd, {{"exit", broker::kExitPermission}, {"error", "malformed request"}});
                    platform::close_fds(m->fds);
                    drop(bit->fd);
                    continue;
                }
                // The decision that counts: the same function the client ran,
                // with the policy file as the supervisor read it at start.
                auto cls = broker::classify(argv, L.instance);
                auto d = broker::decide(*L.broker_policy, cls);
                nlohmann::json ev{{"event", "decision"}, {"session", info.id}, {"program", argv[0]},
                                  {"argc", argv.size()},
                                  {"action", std::string(policy::to_string(d.action))},
                                  {"reason", d.reason}};
                if (cls.route == broker::Route::Owner) d.action = policy::Action::Deny;
                // Deny by default: the broker runs, as the owner, only what
                // changes this instance on its behalf. Anything else -- what
                // the client runs locally, or never heard of -- is refused
                // here, whoever sent it: a process inside can talk to this
                // socket without the client.
                if (cls.route == broker::Route::Local) {
                    d.action = policy::Action::Deny;
                    d.reason = std::format("`{}` is not something the broker runs; it runs inside the sandbox",
                                           argv[0]);
                }
                if (d.action == policy::Action::Deny) {
                    audit(ev, observe::Kind::Perm);
                    send_msg(bit->fd, {{"exit", broker::kExitPermission},
                                       {"error", "E_PERMISSION: " + d.reason},
                                       {"hint", d.owner_command.empty() ? std::string{}
                                                : "outside the sandbox: " + d.owner_command}});
                    platform::close_fds(m->fds);
                    drop(bit->fd);
                    continue;
                }
                if (d.action == policy::Action::Ask) {
                    auto id = broker::enqueue(home, L.instance, argv, d.reason);
                    ev["request"] = id;
                    audit(ev, observe::Kind::Perm);
                    send_msg(bit->fd, {{"exit", broker::kExitPending},
                                       {"error", "waiting for the owner's approval: request " + id},
                                       {"hint", std::format("outside the sandbox: xlings subos approve {} {}",
                                                            L.instance, id)},
                                       {"request", id}});
                    platform::close_fds(m->fds);
                    drop(bit->fd);
                    continue;
                }
                if (!audit(ev, observe::Kind::Perm)) {
                    send_msg(bit->fd, {{"exit", kExitSetup}, {"error", "E_AUDIT_WRITE: session ended"}});
                    platform::close_fds(m->fds);
                    drop(bit->fd);
                    continue;
                }
                auto full = L.broker_exe;
                full.insert(full.end(), argv.begin(), argv.end());
                if (std::ranges::find(full, std::string("-y")) == full.end()) full.push_back("-y");
                const int bp = platform::fork_process();
                if (bp == 0) {
                    platform::reset_signals();
                    // From the home: a directory that is a home ends the
                    // project search, so the session owner's cwd (a project,
                    // perhaps) never becomes the scope this runs in.
                    if (auto h = L.broker_env.find("XLINGS_HOME"); h != L.broker_env.end()) {
                        std::error_code cec;
                        fs::current_path(h->second, cec);
                    }
                    platform::redirect_stdio(m->fds);
                    (void)platform::exec_path(full, L.broker_env);
                    platform::exit_now(kExitCannotRun);
                }
                platform::close_fds(m->fds);
                bit->pid = bp > 0 ? bp : 0;
                bit->program = argv[0];
                if (bp <= 0) {
                    send_msg(bit->fd, {{"exit", kExitSetup}, {"error", "the broker could not start it"}});
                    drop(bit->fd);
                } else {
                    send_msg(bit->fd, {{"started", true}});
                }
                continue;
            }
            if (p.fd == listen_fd) {
                if (int c = platform::unix_accept(listen_fd); c >= 0) clients.push_back({.fd = c});
                continue;
            }
            if (p.fd == signal_fd) continue;   // drained at the top
            if (ctl_open && p.fd == ctl[0]) {
                auto m = recv_msg(ctl[0]);
                if (!m) {
                    ctl_open = false;
                    continue;
                }
                if (m->json.value("op", "") == "root-mount-context") {
                    platform::close_fds(m->fds);
                    report("duplicate root namespace context refused");
                    root_failed = true;
                    if (pid > 0) platform::send_signal(pid, sig::kill);
                    continue;
                }
                if (m->json.value("op", "") == "net-listener" ||
                    m->json.value("op", "") == "exec-listener") {
                    if (m->fds.size() == 1 && net_notify_fd < 0) {
                        net_notify_fd = m->fds.front();
                        m->fds.clear();
                    } else {
                        (void)audit({{"event", "audit-trace-unavailable"}, {"session", info.id},
                            {"reason", m->json.value("reason", "invalid notification listener handoff")}});
                        report("required syscall audit listener is unavailable");
                        if (pid > 0) platform::send_signal(pid, sig::kill);
                    }
                    platform::close_fds(m->fds);
                    continue;
                }
                platform::close_fds(m->fds);
                auto id = m->json.value("id", std::int64_t{0});
                auto it = std::ranges::find_if(clients, [&](const Client& c) {
                    return c.fd >= 0 && c.exec_id == id;
                });
                if (it == clients.end()) continue;
                if (m->json.contains("started")) {
                    send_msg(it->fd, {{"started", true}});
                } else {
                    finish_client(*it, m->json);
                }
                continue;
            }
            auto it = std::ranges::find(clients, p.fd, &Client::fd);
            if (it == clients.end()) continue;
            auto m = recv_msg(it->fd);
            if (!m) {
                // The client went away: what it started goes too.
                if (it->exec_id && ctl_open)
                    send_msg(ctl[0], {{"op", "kill"}, {"id", *it->exec_id}});
                drop(it->fd);
                continue;
            }
            const auto op = m->json.value("op", "");
            if (op == "ping") {
                send_msg(it->fd, to_json(info));
                platform::close_fds(m->fds);
            } else if (op == "stop") {
                audit({{"event", "session-stop-requested"}, {"session", info.id}});
                if (!reaped) platform::send_signal(pid, sig::terminate);
                send_msg(it->fd, {{"ok", true}});
                platform::close_fds(m->fds);
            } else if (op == "signal" && it->exec_id && ctl_open) {
                send_msg(ctl[0], {{"op", "signal"}, {"id", *it->exec_id},
                                  {"sig", m->json.value("sig", sig::terminate)}});
                platform::close_fds(m->fds);
            } else if (op == "exec" && !it->exec_id && ctl_open) {
                auto argv_j = m->json.value("argv", nlohmann::json::array());
                if (!argv_j.is_array() || argv_j.empty() || m->fds.size() != 3) {
                    send_msg(it->fd, {{"error", "malformed exec request"}, {"phase", "setup"}});
                    platform::close_fds(m->fds);
                    continue;
                }
                // The joined command's environment: the session's own, plus
                // what the caller passed that the policy lets in.
                auto env = L.exec_env;
                const auto req_env = m->json.value("env", nlohmann::json::object());
                for (auto e = req_env.begin(); e != req_env.end(); ++e) {
                    if (e.value().is_string() && env_allowed(e.key(), L.env_pass))
                        env[e.key()] = e.value().get<std::string>();
                }
                std::string cwd = m->json.value("cwd", "");
                if (cwd.empty()) cwd = L.default_cwd;
                auto id = next_id++;
                it->exec_id = id;
                it->started = now;
                it->argv0 = argv_j[0].is_string() ? argv_j[0].get<std::string>() : "";
                if (auto t = m->json.value("timeout_ms", 0LL); t > 0)
                    it->deadline = now + std::chrono::milliseconds(t);
                nlohmann::json ev{{"event", "exec"}, {"session", info.id}, {"exec", id},
                                  {"program", it->argv0}, {"argc", argv_j.size()}, {"cwd", cwd},
                                  {"tty", m->json.value("tty", false)}};
                ev["env"] = nlohmann::json::array();
                for (auto& [k, v] : env) ev["env"].push_back(k);   // names, never values
                if (!audit(ev, observe::Kind::Ops)) {
                    send_msg(it->fd, {{"error", "E_AUDIT_WRITE: session ended"}, {"phase", "setup"}});
                    platform::close_fds(m->fds);
                    drop(it->fd);
                    continue;
                }
                send_msg(ctl[0], {{"op", "exec"}, {"id", id}, {"argv", argv_j}, {"env", env},
                                  {"cwd", cwd}, {"tty", m->json.value("tty", false)}},
                         m->fds);
                platform::close_fds(m->fds);
            } else {
                send_msg(it->fd, {{"error", "unsupported request"}, {"phase", "setup"}});
                platform::close_fds(m->fds);
            }
        }
        std::erase_if(clients, [](const Client& c) { return c.fd < 0; });
    }

    for (auto& c : clients) {
        if (c.fd < 0) continue;
        if (c.exec_id) send_msg(c.fd, {{"error", "the session ended"}, {"phase", "setup"}});
        platform::close_fd(c.fd);
    }
    platform::close_fd(net_notify_fd);
    if (proxyRelay) proxyRelay->stop();
    if (proxyBridge) platform::close_fd((*proxyBridge)[0]);
    // What changed in the host paths mapped read-write (design §22, fs).
    for (const auto& root : L.rw_paths) {
        auto [files, truncated] = changed_since(root, fs_since, 200);
        if (files.empty()) continue;
        audit({{"event", "changed"}, {"session", info.id}, {"mount", root},
                                 {"count", files.size()}, {"truncated", truncated}, {"files", files}},
              observe::Kind::Fs);
    }
    std::error_code ec;
    if (nat) {
        // pasta outlives nothing it served.
        std::ifstream pf(pasta_pidfile);
        int ppid = 0;
        if (pf >> ppid && ppid > 0) platform::send_signal(ppid, sig::terminate);
        fs::remove(pasta_pidfile, ec);
    }
    fs::remove(sock_path(home, L.instance), ec);
    fs::remove(info_path(home, L.instance), ec);
    if (broker_fd >= 0) {
        for (auto& b : brokered) {
            if ((root_failed || audit_failed) && b.pid > 0)
                platform::send_signal(b.pid, sig::kill);
            if (b.pid > 0) (void)platform::wait_process(b.pid, true);
            platform::close_fd(b.fd);
        }
        platform::close_fd(broker_fd);
        fs::remove(home.broker_socket(L.instance), ec);
    }
    platform::close_fd(listen_fd);
    if (ctl_open) platform::close_fd(ctl[0]);
    if ((audit_failed || root_failed) && !reaped && pid > 0) {
        platform::send_signal(pid, sig::kill);
        if (const auto exit = platform::wait_process(pid, true)) {
            status = *exit;
            reaped = true;
            pid = -1;
        }
    }
    const int code = (audit_failed || root_failed) ? kExitSetup : timed_out ? kExitTimeout : reaped ? platform::exit_code(status) : kExitSetup;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started).count();
    audit({{"event", "session-end"}, {"session", info.id}, {"exit", code},
                             {"ms", ms}});
    return (audit_failed || root_failed) ? kExitSetup : code;
}

}  // namespace

int host(const HomeView& home, Launch L) {
    if constexpr (!kSessions) return kExitSetup;
    std::error_code ec;
    fs::create_directories(home.run_dir(L.instance), ec);
    fs::permissions(home.run_dir(L.instance), fs::perms::owner_all, fs::perm_options::replace, ec);
    if (find(home, L.instance)) {
        report(std::format("'{}' already has a session", L.instance));
        return kExitSetup;
    }
    const int listen_fd = platform::unix_listen(sock_path(home, L.instance));
    if (listen_fd < 0) {
        report(std::format("cannot listen on {}: {}", sock_path(home, L.instance).string(),
                           platform::error_text(platform::last_error())));
        return kExitSetup;
    }
    auto ctl = platform::unix_pair();
    if (!ctl) {
        platform::close_fd(listen_fd);
        return kExitSetup;
    }
    L.env[std::string(kControlFdEnv)] = std::to_string((*ctl)[1]);
    L.env[std::string(kTtlEnv)] = std::to_string(L.ttl);
    if (L.refresh_root) L.env[std::string(kRootViewEnv)] = "1";
    else L.env.erase(std::string(kRootViewEnv));
    if (L.trace_exec) L.env["XLINGS_SESSION_TRACE"] = "1";
    if (L.trace_net) L.env["XLINGS_SESSION_NET_TRACE"] = "1";

    Info info{ .instance = L.instance, .id = new_id(), .supervisor_pid = platform::get_pid(),
               .started = observe::utc_now(), .backend = L.backend, .digest = L.digest,
               .ttl = L.ttl, .detached = L.detached };

    if (!L.detached) return supervise(home, L, info, listen_fd, *ctl, -1);

    // Detached: a supervisor that outlives this command. Readiness comes back
    // on a pipe; EOF without it means the session never started.
    auto ready = platform::make_pipe();
    if (!ready) return kExitSetup;
    const int first = platform::fork_process();
    if (first < 0) return kExitSetup;
    if (first == 0) {
        platform::close_fd((*ready)[0]);
        platform::new_session();
        const int second = platform::fork_process();
        if (second != 0) platform::exit_now(second < 0 ? 1 : 0);
        // stdio of a daemon: nothing to read, its own log to write.
        const int null = platform::open_null();
        auto log = home.logs_dir(L.instance) / "session.log";
        fs::create_directories(log.parent_path(), ec);
        const int logfd = platform::open_for_append(log);
        const int stdio[3] = {null, logfd >= 0 ? logfd : null, logfd >= 0 ? logfd : null};
        platform::redirect_stdio(stdio);
        info.supervisor_pid = platform::get_pid();
        platform::exit_now(supervise(home, L, info, listen_fd, *ctl, (*ready)[1]));
    }
    platform::close_fd((*ready)[1]);
    platform::close_fd(listen_fd);
    platform::close_fd((*ctl)[0]);
    platform::close_fd((*ctl)[1]);
    for (int fd : L.keep_fds) platform::close_fd(fd);
    (void)platform::wait_process(first, true);
    platform::PollFd p{ (*ready)[0] };
    char b = 0;
    const bool ok = platform::poll_fds(std::span(&p, 1), 15000) > 0 && platform::read_exact((*ready)[0], &b, 1);
    platform::close_fd((*ready)[0]);
    return ok ? 0 : kExitSetup;
}

// ── joining ──────────────────────────────────────────────────────────

ExecResult join(const HomeView& home, std::string_view instance, const ExecRequest& r) {
    if constexpr (!kSessions)
        return {kExitSetup, "setup", "sessions are not implemented on this platform yet"};
    if (r.argv.empty()) return {kExitSetup, "setup", "no command"};
    const int fd = platform::unix_connect(sock_path(home, instance));
    if (fd < 0) return {kExitSetup, "setup", "no running session for '" + std::string(instance) + "'"};

    nlohmann::json req{{"op", "exec"}, {"argv", r.argv}, {"env", r.env}, {"cwd", r.cwd},
                       {"tty", r.tty}};
    if (r.timeout) req["timeout_ms"] = r.timeout->count();
    const int stdio[3] = {0, 1, 2};
    if (!send_msg(fd, req, stdio)) {
        platform::close_fd(fd);
        return {kExitSetup, "setup", "could not reach the session"};
    }
    // Ctrl-C and friends belong to the command, not to this client.
    const int signal_fd = platform::route_signals({sig::interrupt, sig::terminate, sig::hangup, sig::quit},
                                                  {sig::pipe});
    ExecResult result{kExitSetup, "setup", "the session ended"};
    while (true) {
        platform::PollFd p[2] = {{fd}, {signal_fd}};
        platform::poll_fds(p, -1);
        for (int s : platform::pending_signals()) send_msg(fd, {{"op", "signal"}, {"sig", s}});
        if (!p[0].readable && !p[0].closed) continue;
        auto m = recv_msg(fd);
        if (!m) break;
        platform::close_fds(m->fds);
        const auto& j = m->json;
        if (j.contains("started")) continue;
        if (j.contains("exit")) {
            result = {j["exit"].get<int>(), "", ""};
        } else if (j.contains("signal")) {
            result = {128 + j["signal"].get<int>(), "", ""};
        } else if (j.contains("error")) {
            const int e = j.value("errno", 0);
            const int code = platform::is_not_found(e) ? kExitNotFound
                           : platform::is_not_executable(e) ? kExitCannotRun : kExitSetup;
            result = {code, code == kExitSetup ? "setup" : "exec", j.value("error", "")};
        }
        break;
    }
    platform::close_fd(fd);
    return result;
}

bool stop(const HomeView& home, std::string_view instance) {
    auto info = find(home, instance);
    if (!info) return false;
    if (const int fd = platform::unix_connect(sock_path(home, instance)); fd >= 0) {
        send_msg(fd, {{"op", "stop"}});
        platform::PollFd p{ fd };
        platform::poll_fds(std::span(&p, 1), 5000);
        platform::close_fd(fd);
    }
    using namespace std::chrono_literals;
    for (int i = 0; i < 100 && find(home, instance); ++i) std::this_thread::sleep_for(50ms);
    if (find(home, instance)) {
        platform::send_signal(info->sandbox_pid, sig::kill);
        for (int i = 0; i < 40 && find(home, instance); ++i) std::this_thread::sleep_for(50ms);
    }
    return !find(home, instance);
}

// ── inside ───────────────────────────────────────────────────────────

int session_init(std::span<const std::string> args) {
    if constexpr (!kSessions) return kExitSetup;
    auto env_int = [](std::string_view name, int fallback) {
        const auto v = platform::environment();
        auto it = v.find(std::string(name));
        return it == v.end() ? fallback : std::atoi(it->second.c_str());
    };
    const int ctl = env_int(kControlFdEnv, -1);
    const int ttl = env_int(kTtlEnv, 0);
    platform::set_inheritable(ctl, false);
    // Neither variable means anything to the commands that run here.
    platform::unset_env_variable(std::string(kControlFdEnv));
    platform::unset_env_variable(std::string(kTtlEnv));

    // net=proxy: the loopback gateway, here, in bwrap's network namespace --
    // for this session's whole life (a thread: everything started here is
    // forked and exec'd, so it never runs in a child).
    if (const int bridge = env_int(kProxyFdEnv, -1); bridge >= 0) {
        platform::unset_env_variable(std::string(kProxyFdEnv));
        platform::set_inheritable(bridge, false);
        (void)platform::network::enable_loopback();
        const auto listener = platform::network::listen_loopback(network::GATEWAY_PORT);
        if (!listener) {
            if (ctl >= 0) (void)send_msg(ctl, {{"op", "proxy-gateway"}, {"error", listener.error()}});
            return kExitSetup;
        }
        std::thread([bridge, fd = *listener] { (void)network::gateway_run(bridge, fd); }).detach();
    }

    const bool rootView = env_int(kRootViewEnv, 0) == 1;
    platform::unset_env_variable(std::string(kRootViewEnv));
    if (rootView) {
        auto captured = platform::root_mount::capture();
        if (!captured) {
            if (ctl >= 0)
                (void)send_msg(ctl, {{"op", "root-mount-context"}, {"error", captured.error()}});
            return kExitSetup;
        }
        const bool sent = ctl >= 0 && send_msg(ctl, {{"op", "root-mount-context"}}, *captured);
        for (const int descriptor : *captured)
            platform::close_fd(descriptor);
        if (!sent)
            return kExitSetup;
    }

    // --sandbox landlock: the write fence, before anything is started, so
    // everything here and everything that joins later is inside it. Failing
    // to put it up is a failure to set up, never a run without it.
    {
        const auto env = platform::environment();
        if (auto rw = env.find(std::string(kLandlockRwEnv)); rw != env.end()) {
            std::vector<fs::path> paths;
            std::istringstream in{rw->second};
            for (std::string line; std::getline(in, line);) if (!line.empty()) paths.emplace_back(line);
            platform::unset_env_variable(std::string(kLandlockRwEnv));
            if (auto r = platform::landlock::restrict_writes(paths); !r) {
                std::println(std::cerr, "xlings: {}", r.error());
                return kExitSetup;
            }
        }
        const bool traceNet = env_int("XLINGS_SESSION_NET_TRACE", 0) == 1;
        const bool traceExec = env_int("XLINGS_SESSION_TRACE", 0) == 1;
        platform::unset_env_variable("XLINGS_SESSION_NET_TRACE");
        platform::unset_env_variable("XLINGS_SESSION_TRACE");
        if (traceNet || traceExec) {
            // One filter/listener carries both classes. The pre-filter helper
            // makes its sole SCM_RIGHTS handoff without auditing its own sendmsg.
            std::promise<std::pair<int, std::string>> handoff;
            auto pending = handoff.get_future();
            bool transferred { false };
            std::jthread helper([&, result = std::move(pending)] () mutable {
                const auto [listener, reason] = result.get();
                if (listener >= 0 && ctl >= 0) {
                    const int descriptor[1] = {listener};
                    transferred = send_msg(ctl, {{"op", "net-listener"}}, descriptor);
                    platform::close_fd(listener);
                } else if (ctl >= 0) transferred = send_msg(ctl, {{"op", "net-listener"}, {"reason", reason}});
            });
            const int listener = platform::net_notify::listener({.network = traceNet, .exec = traceExec});
            handoff.set_value({listener, listener < 0 ? platform::error_text(platform::last_error()) : std::string{}});
            helper.join();
            if (listener < 0 || !transferred) return kExitSetup;
        }
    }

    std::vector<std::string> main_argv;
    if (auto it = std::ranges::find(args, std::string("--")); it != args.end())
        main_argv.assign(std::next(it), args.end());

    int main_pid = -1;
    platform::ExitStatus main_status{};
    bool main_done = main_argv.empty();
    // A child can finish between reaping and polling. Its signal stays on
    // the self-pipe until the next reap, rather than waiting for the TTL tick.
    const int signal_fd = platform::route_signals({sig::child}, {});
    if (signal_fd < 0) return kExitSetup;
    if (!main_argv.empty()) {
        main_pid = platform::fork_process();
        if (main_pid == 0) {
            platform::close_fd(ctl);
            platform::reset_signals();
            const int e = platform::exec_program(main_argv, platform::environment());
            std::println(std::cerr, "xlings: {}: {}", main_argv[0], platform::error_text(e));
            platform::exit_now(platform::is_not_found(e) ? kExitNotFound : kExitCannotRun);
        }
        if (main_pid < 0) return kExitSetup;
    }

    // While the main command owns the terminal, Ctrl-C is its business.
    platform::ignore_signals({sig::interrupt, sig::quit, sig::pipe});

    std::map<int, std::int64_t> joined;
    auto last_activity = std::chrono::steady_clock::now();
    bool ctl_open = ctl >= 0;

    auto kill_all = [&](int s) {
        for (auto& [pid, id] : joined) platform::send_signal_group(pid, s);
        if (main_pid > 0 && !main_done) platform::send_signal(main_pid, s);
    };

    while (true) {
        (void)platform::pending_signals();
        while (auto child = platform::reap_child()) {
            const auto [w, st] = *child;
            if (w == main_pid) {
                main_done = true;
                main_status = st;
            } else if (auto it = joined.find(w); it != joined.end()) {
                nlohmann::json reply{{"id", it->second}};
                if (st.exited) reply["exit"] = st.code;
                else reply["signal"] = st.signal;
                if (ctl_open) send_msg(ctl, reply);
                joined.erase(it);
                last_activity = std::chrono::steady_clock::now();
            }
        }
        if (main_done && joined.empty()) {
            if (!main_argv.empty()) return platform::exit_code(main_status);
            if (!ctl_open) return 0;
            if (ttl > 0 && std::chrono::steady_clock::now() - last_activity >= std::chrono::seconds(ttl))
                return 0;
        }
        platform::PollFd watched[2]{{ctl_open ? ctl : -1}, {signal_fd}};
        if (platform::poll_fds(watched, 200) <= 0) continue;
        if (!ctl_open || (!watched[0].readable && !watched[0].closed)) continue;
        auto m = recv_msg(ctl);
        if (!m) {
            // The supervisor is gone: nothing outside watches any more.
            ctl_open = false;
            kill_all(sig::terminate);
            continue;
        }
        const auto op = m->json.value("op", "");
        const auto id = m->json.value("id", std::int64_t{0});
        if (op == "exec" && m->fds.size() == 3) {
            std::vector<std::string> argv;
            for (auto& a : m->json["argv"]) if (a.is_string()) argv.push_back(a.get<std::string>());
            std::map<std::string, std::string> env;
            const auto req_env = m->json.value("env", nlohmann::json::object());
            for (auto e = req_env.begin(); e != req_env.end(); ++e)
                if (e.value().is_string()) env[e.key()] = e.value().get<std::string>();
            const auto cwd = m->json.value("cwd", std::string("/"));
            const bool tty = m->json.value("tty", false);

            auto report_pipe = platform::make_pipe();
            if (!report_pipe) {
                send_msg(ctl, {{"id", id}, {"error", "pipe failed"}, {"errno", platform::last_error()}});
                platform::close_fds(m->fds);
                continue;
            }
            const int pid = platform::fork_process();
            if (pid == 0) {
                platform::close_fd((*report_pipe)[0]);
                platform::close_fd(ctl);
                platform::reset_signals();
                // A session of its own: no controlling terminal to inject
                // into. A terminal it was handed is tried as its controlling
                // one; when the terminal already belongs to another session
                // (the common case) it runs without job control.
                platform::new_session();
                platform::redirect_stdio(m->fds);
                if (tty) (void)platform::take_controlling_terminal();
                std::error_code cec;
                fs::current_path(cwd, cec);
                if (cec) fs::current_path("/", cec);
                int e = platform::exec_program(argv, env);
                (void)platform::write_fd((*report_pipe)[1],
                                         std::string_view(reinterpret_cast<const char*>(&e), sizeof(e)));
                platform::exit_now(platform::is_not_found(e) ? kExitNotFound : kExitCannotRun);
            }
            platform::close_fd((*report_pipe)[1]);
            platform::close_fds(m->fds);
            if (pid < 0) {
                platform::close_fd((*report_pipe)[0]);
                send_msg(ctl, {{"id", id}, {"error", "fork failed"}, {"errno", platform::last_error()}});
                continue;
            }
            int e = 0;
            if (platform::read_exact((*report_pipe)[0], &e, sizeof(e))) {
                platform::close_fd((*report_pipe)[0]);
                (void)platform::wait_process(pid, true);
                send_msg(ctl, {{"id", id}, {"error", platform::error_text(e)}, {"errno", e}});
                continue;
            }
            platform::close_fd((*report_pipe)[0]);
            joined[pid] = id;
            last_activity = std::chrono::steady_clock::now();
            send_msg(ctl, {{"id", id}, {"started", true}, {"pid", pid}});
        } else if (op == "kill" || op == "signal") {
            const int s = op == "kill" ? sig::kill : m->json.value("sig", sig::terminate);
            for (auto& [pid, jid] : joined) if (jid == id) platform::send_signal_group(pid, s);
            platform::close_fds(m->fds);
        } else if (op == "stop") {
            kill_all(sig::terminate);
            platform::close_fds(m->fds);
            return 0;
        } else {
            platform::close_fds(m->fds);
        }
    }
}

}  // namespace xlings::subos::session
