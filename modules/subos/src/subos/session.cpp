module;

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
// glibc declares environ in <unistd.h> (_GNU_SOURCE, on by default in g++)
#define XLINGS_SESSION_ENVIRON environ
#endif

module xlings.subos.session;

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.subos.home_view;

namespace xlings::subos::session {

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

#if !defined(__linux__)

// Sessions need a SessionHost (design §17); this platform does not have one
// yet, and `subos status` says so.
std::optional<Info> find(const HomeView&, std::string_view) { return std::nullopt; }
std::vector<Info> list(const HomeView&) { return {}; }
int host(const HomeView&, Launch) { return kExitSetup; }
ExecResult join(const HomeView&, std::string_view, const ExecRequest&) {
    return {kExitSetup, "setup", "sessions are not implemented on this platform yet"};
}
bool stop(const HomeView&, std::string_view) { return false; }
int session_init(std::span<const std::string>) { return kExitSetup; }

#else

namespace {

// ── messages ─────────────────────────────────────────────────────────

constexpr std::size_t kMaxMessage = 1u << 20;   // env can be large; a datagram carries it whole
constexpr int kMaxFds = 8;

struct Msg {
    nlohmann::json json;
    std::vector<int> fds;
};

bool send_msg(int sock, const nlohmann::json& j, std::span<const int> fds = {}) {
    auto text = j.dump();
    iovec iov{ text.data(), text.size() };
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    std::vector<char> control;
    if (!fds.empty()) {
        control.resize(CMSG_SPACE(sizeof(int) * fds.size()));
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();
        cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(cm), fds.data(), sizeof(int) * fds.size());
    }
    while (true) {
        auto n = ::sendmsg(sock, &msg, MSG_NOSIGNAL);
        if (n >= 0) return static_cast<std::size_t>(n) == text.size();
        if (errno != EINTR) return false;
    }
}

// nullopt on EOF or error; a message that is not JSON is dropped (and its
// descriptors closed) rather than trusted.
std::optional<Msg> recv_msg(int sock) {
    std::vector<char> buf(kMaxMessage);
    iovec iov{ buf.data(), buf.size() };
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFds)];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n;
    do { n = ::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
    if (n <= 0) return std::nullopt;
    Msg out;
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            auto count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int fd;
                std::memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
                out.fds.push_back(fd);
            }
        }
    }
    out.json = nlohmann::json::parse(std::string_view(buf.data(), static_cast<std::size_t>(n)),
                                     nullptr, false);
    if (out.json.is_discarded() || !out.json.is_object()) {
        for (int fd : out.fds) ::close(fd);
        out.fds.clear();
        out.json = nlohmann::json::object();
    }
    return out;
}

void close_all(std::vector<int>& fds) {
    for (int fd : fds) if (fd >= 0) ::close(fd);
    fds.clear();
}

// ── sockets ──────────────────────────────────────────────────────────

fs::path sock_path(const HomeView& home, std::string_view instance) {
    return home.run_dir(instance) / "exec.sock";
}
fs::path info_path(const HomeView& home, std::string_view instance) {
    return home.run_dir(instance) / "session.json";
}

// sockaddr_un holds ~108 bytes; a home under a long path would not fit. A
// path that is too long is reached through its directory's descriptor
// (/proc/self/fd/<n>/name on Linux, a chdir elsewhere).
struct UnixAddr {
    sockaddr_un addr{};
    socklen_t len{};
    int dirfd { -1 };
    ~UnixAddr() { if (dirfd >= 0) ::close(dirfd); }
};

bool make_addr(const fs::path& path, UnixAddr& a) {
    a.addr.sun_family = AF_UNIX;
    auto s = path.string();
    if (s.size() >= sizeof(a.addr.sun_path)) {
#if defined(__linux__)
        a.dirfd = ::open(path.parent_path().c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (a.dirfd < 0) return false;
        s = std::format("/proc/self/fd/{}/{}", a.dirfd, path.filename().string());
        if (s.size() >= sizeof(a.addr.sun_path)) return false;
#else
        return false;
#endif
    }
    std::memcpy(a.addr.sun_path, s.c_str(), s.size() + 1);
    a.len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + s.size() + 1);
    return true;
}

int listen_unix(const fs::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    UnixAddr a;
    if (!make_addr(path, a)) { ::close(fd); return -1; }
    ::unlink(path.c_str());
    // Owner only: anyone who can connect can run commands in the sandbox.
    auto old = ::umask(0077);
    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a.addr), a.len);
    ::umask(old);
    if (rc != 0 || ::listen(fd, 16) != 0) { ::close(fd); return -1; }
    return fd;
}

int connect_unix(const fs::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    UnixAddr a;
    if (!make_addr(path, a)
        || ::connect(fd, reinterpret_cast<sockaddr*>(&a.addr), a.len) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool pid_alive(int pid) {
    return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
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
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

void audit(const HomeView& home, std::string_view instance, nlohmann::json fields,
           observe::Kind kind = observe::Kind::Lifecycle) {
    fields["instance"] = std::string(instance);
    observe::append(home.logs_dir(instance) / "events.ndjson",
                    observe::Event{ .kind = kind, .fields = std::move(fields) });
}

int status_to_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return kExitSetup;
}

std::vector<std::string> env_strings(const std::map<std::string, std::string>& env) {
    std::vector<std::string> out;
    out.reserve(env.size());
    for (auto& [k, v] : env) out.push_back(k + "=" + v);
    return out;
}

std::vector<char*> c_array(std::vector<std::string>& v) {
    std::vector<char*> out;
    for (auto& s : v) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

// execvp's search, against the given environment's PATH, with its errno rule:
// a permission problem anywhere beats "not found".
void exec_resolved(std::vector<std::string> argv, const std::map<std::string, std::string>& env) {
    auto envs = env_strings(env);
    auto envp = c_array(envs);
    auto av = c_array(argv);
    if (argv[0].find('/') != std::string::npos) {
        ::execve(argv[0].c_str(), av.data(), envp.data());
        return;
    }
    std::string path = "/usr/local/bin:/usr/bin:/bin";
    if (auto it = env.find("PATH"); it != env.end()) path = it->second;
    int saved = ENOENT;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        auto dir = path.substr(start, end - start);
        if (dir.empty()) dir = ".";
        auto candidate = dir + "/" + argv[0];
        ::execve(candidate.c_str(), av.data(), envp.data());
        if (errno == EACCES) saved = EACCES;
        else if (errno != ENOENT && errno != ENOTDIR) saved = errno;
        start = end + 1;
    }
    errno = saved;
}

std::map<std::string, std::string> current_env() {
    std::map<std::string, std::string> env;
    for (char** e = XLINGS_SESSION_ENVIRON; e && *e; ++e) {
        std::string_view kv(*e);
        auto eq = kv.find('=');
        if (eq != std::string_view::npos)
            env[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
    }
    return env;
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

// ── signals (supervisor and client) ──────────────────────────────────

int g_sig_pipe[2] = {-1, -1};

extern "C" void on_signal(int sig) {
    int saved = errno;
    unsigned char b = static_cast<unsigned char>(sig);
    (void)::write(g_sig_pipe[1], &b, 1);
    errno = saved;
}

void install_signal_pipe(std::initializer_list<int> forward, std::initializer_list<int> ignore) {
    if (g_sig_pipe[0] < 0) {
        if (::pipe(g_sig_pipe) != 0) return;
        for (int fd : g_sig_pipe) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
    }
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    ::sigemptyset(&sa.sa_mask);
    for (int s : forward) ::sigaction(s, &sa, nullptr);
    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    for (int s : ignore) ::sigaction(s, &ign, nullptr);
}

std::vector<int> drain_signals() {
    std::vector<int> out;
    unsigned char b;
    while (g_sig_pipe[0] >= 0 && ::read(g_sig_pipe[0], &b, 1) == 1) out.push_back(b);
    return out;
}

void reset_signals_for_child() {
    for (int s : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD}) ::signal(s, SIG_DFL);
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
}

}  // namespace

// ── discovery ────────────────────────────────────────────────────────

std::optional<Info> find(const HomeView& home, std::string_view instance) {
    auto ip = info_path(home, instance);
    std::error_code ec;
    if (!fs::exists(ip, ec)) return std::nullopt;
    std::ifstream in(ip);
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    auto info = info_from_json(j);
    if (pid_alive(info.supervisor_pid)) {
        // The pid could have been reused; the supervisor answering on its
        // socket is the proof.
        int fd = connect_unix(sock_path(home, instance));
        if (fd >= 0) {
            ::close(fd);
            return info;
        }
    }
    if (!pid_alive(info.supervisor_pid)) {
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

struct Client {
    int fd { -1 };
    std::optional<std::int64_t> exec_id;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool timed_out { false };
    std::string argv0;
    std::chrono::steady_clock::time_point started{};
};

int supervise(const HomeView& home, Launch& L, Info& info, int listen_fd, int ctl[2],
              int ready_fd) {
    // Like system(): while the sandbox runs, Ctrl-C is the command's. SIGTERM
    // and SIGHUP end the session (forwarded to the sandbox).
    install_signal_pipe({SIGTERM, SIGHUP}, {SIGINT, SIGQUIT, SIGPIPE});

    auto envs = env_strings(L.env);
    auto envp = c_array(envs);
    auto argv = L.argv;
    auto av = c_array(argv);

    pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "[xlings:subos] fork failed: %s\n", std::strerror(errno));
        return kExitSetup;
    }
    if (pid == 0) {
        reset_signals_for_child();
        ::close(listen_fd);
        ::close(ctl[0]);
        // The control socket and the caller's descriptors (a seccomp filter)
        // cross exec; nothing else of the supervisor's does.
        ::fcntl(ctl[1], F_SETFD, 0);
        for (int fd : L.keep_fds) ::fcntl(fd, F_SETFD, 0);
        ::execve(av[0], av.data(), envp.data());
        std::fprintf(stderr, "[xlings:subos] cannot run %s: %s\n", av[0], std::strerror(errno));
        ::_exit(kExitSetup);
    }
    ::close(ctl[1]);
    for (int fd : L.keep_fds) ::close(fd);

    const auto started = std::chrono::steady_clock::now();
    info.sandbox_pid = pid;
    write_atomic(info_path(home, L.instance), to_json(info).dump(2));
    audit(home, L.instance, {{"event", "session-start"}, {"session", info.id},
                             {"backend", L.backend}, {"detached", L.detached}, {"ttl", L.ttl},
                             {"spec", L.spec}});
    if (ready_fd >= 0) {
        char ok = '1';
        (void)::write(ready_fd, &ok, 1);
        ::close(ready_fd);
    }

    std::vector<Client> clients;
    std::int64_t next_id = 1;
    int status = 0;
    bool reaped = false;
    bool ctl_open = true;

    auto finish_client = [&](Client& c, nlohmann::json reply) {
        if (c.timed_out) reply = {{"exit", kExitTimeout}, {"timeout", true}};
        send_msg(c.fd, reply);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - c.started).count();
        nlohmann::json ev{{"event", "exec-end"}, {"session", info.id}, {"exec", *c.exec_id},
                          {"program", c.argv0}, {"ms", ms}};
        for (auto k : {"exit", "signal", "error", "timeout"})
            if (reply.contains(k)) ev[k] = reply[k];
        audit(home, L.instance, ev, observe::Kind::Ops);
        ::close(c.fd);
        c.fd = -1;
    };

    while (true) {
        if (!reaped) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) reaped = true;
        }
        for (int sig : drain_signals()) {
            if ((sig == SIGTERM || sig == SIGHUP) && !reaped) ::kill(pid, SIGTERM);
        }
        if (reaped && !ctl_open) break;
        if (reaped) {
            // Drain what session-init said before it went.
            pollfd p{ctl[0], POLLIN, 0};
            if (::poll(&p, 1, 0) <= 0) break;
        }

        std::vector<pollfd> pfds{{listen_fd, POLLIN, 0}, {g_sig_pipe[0], POLLIN, 0}};
        if (ctl_open) pfds.push_back({ctl[0], POLLIN, 0});
        for (auto& c : clients) if (c.fd >= 0) pfds.push_back({c.fd, POLLIN, 0});
        ::poll(pfds.data(), pfds.size(), 200);

        const auto now = std::chrono::steady_clock::now();
        for (auto& c : clients) {
            if (c.fd >= 0 && c.exec_id && c.deadline && !c.timed_out && now >= *c.deadline) {
                c.timed_out = true;
                send_msg(ctl[0], {{"op", "kill"}, {"id", *c.exec_id}});
            }
        }

        for (auto& p : pfds) {
            if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (p.fd == listen_fd) {
                int c = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (c >= 0) clients.push_back({.fd = c});
                continue;
            }
            if (p.fd == g_sig_pipe[0]) continue;   // drained at the top
            if (ctl_open && p.fd == ctl[0]) {
                auto m = recv_msg(ctl[0]);
                if (!m) {
                    ctl_open = false;
                    continue;
                }
                close_all(m->fds);
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
                ::close(it->fd);
                it->fd = -1;
                continue;
            }
            const auto op = m->json.value("op", "");
            if (op == "ping") {
                send_msg(it->fd, to_json(info));
                close_all(m->fds);
            } else if (op == "stop") {
                audit(home, L.instance, {{"event", "session-stop-requested"}, {"session", info.id}});
                if (!reaped) ::kill(pid, SIGTERM);
                send_msg(it->fd, {{"ok", true}});
                close_all(m->fds);
            } else if (op == "signal" && it->exec_id && ctl_open) {
                send_msg(ctl[0], {{"op", "signal"}, {"id", *it->exec_id},
                                  {"sig", m->json.value("sig", SIGTERM)}});
                close_all(m->fds);
            } else if (op == "exec" && !it->exec_id && ctl_open) {
                auto argv_j = m->json.value("argv", nlohmann::json::array());
                if (!argv_j.is_array() || argv_j.empty() || m->fds.size() != 3) {
                    send_msg(it->fd, {{"error", "malformed exec request"}, {"phase", "setup"}});
                    close_all(m->fds);
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
                audit(home, L.instance, ev, observe::Kind::Ops);
                send_msg(ctl[0], {{"op", "exec"}, {"id", id}, {"argv", argv_j}, {"env", env},
                                  {"cwd", cwd}, {"tty", m->json.value("tty", false)}},
                         m->fds);
                close_all(m->fds);
            } else {
                send_msg(it->fd, {{"error", "unsupported request"}, {"phase", "setup"}});
                close_all(m->fds);
            }
        }
        std::erase_if(clients, [](const Client& c) { return c.fd < 0; });
    }

    for (auto& c : clients) {
        if (c.fd < 0) continue;
        if (c.exec_id) send_msg(c.fd, {{"error", "the session ended"}, {"phase", "setup"}});
        ::close(c.fd);
    }
    std::error_code ec;
    fs::remove(sock_path(home, L.instance), ec);
    fs::remove(info_path(home, L.instance), ec);
    ::close(listen_fd);
    if (ctl_open) ::close(ctl[0]);
    const int code = status_to_code(status);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started).count();
    audit(home, L.instance, {{"event", "session-end"}, {"session", info.id}, {"exit", code},
                             {"ms", ms}});
    return code;
}

}  // namespace

int host(const HomeView& home, Launch L) {
    std::error_code ec;
    fs::create_directories(home.run_dir(L.instance), ec);
    fs::permissions(home.run_dir(L.instance), fs::perms::owner_all, fs::perm_options::replace, ec);
    if (find(home, L.instance)) {
        std::fprintf(stderr, "[xlings:subos] '%s' already has a session\n", L.instance.c_str());
        return kExitSetup;
    }
    int listen_fd = listen_unix(sock_path(home, L.instance));
    if (listen_fd < 0) {
        std::fprintf(stderr, "[xlings:subos] cannot listen on %s: %s\n",
                     sock_path(home, L.instance).c_str(), std::strerror(errno));
        return kExitSetup;
    }
    int ctl[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ctl) != 0) {
        ::close(listen_fd);
        return kExitSetup;
    }
    L.env[std::string(kControlFdEnv)] = std::to_string(ctl[1]);
    L.env[std::string(kTtlEnv)] = std::to_string(L.ttl);

    Info info{ .instance = L.instance, .id = new_id(), .supervisor_pid = ::getpid(),
               .started = observe::utc_now(), .backend = L.backend, .digest = L.digest,
               .ttl = L.ttl, .detached = L.detached };

    if (!L.detached) return supervise(home, L, info, listen_fd, ctl, -1);

    // Detached: a supervisor that outlives this command. Readiness comes back
    // on a pipe; EOF without it means the session never started.
    int ready[2];
    if (::pipe(ready) != 0) return kExitSetup;
    pid_t first = ::fork();
    if (first < 0) return kExitSetup;
    if (first == 0) {
        ::close(ready[0]);
        ::setsid();
        pid_t second = ::fork();
        if (second != 0) ::_exit(second < 0 ? 1 : 0);
        // stdio of a daemon: nothing to read, its own log to write.
        int devnull = ::open("/dev/null", O_RDWR);
        auto log = home.logs_dir(L.instance) / "session.log";
        fs::create_directories(log.parent_path(), ec);
        int logfd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        ::dup2(devnull, 0);
        ::dup2(logfd >= 0 ? logfd : devnull, 1);
        ::dup2(logfd >= 0 ? logfd : devnull, 2);
        info.supervisor_pid = ::getpid();
        ::_exit(supervise(home, L, info, listen_fd, ctl, ready[1]));
    }
    ::close(ready[1]);
    ::close(listen_fd);
    ::close(ctl[0]);
    ::close(ctl[1]);
    for (int fd : L.keep_fds) ::close(fd);
    ::waitpid(first, nullptr, 0);
    pollfd p{ready[0], POLLIN, 0};
    char b = 0;
    bool ok = ::poll(&p, 1, 15000) > 0 && ::read(ready[0], &b, 1) == 1;
    ::close(ready[0]);
    return ok ? 0 : kExitSetup;
}

// ── joining ──────────────────────────────────────────────────────────

ExecResult join(const HomeView& home, std::string_view instance, const ExecRequest& r) {
    if (r.argv.empty()) return {kExitSetup, "setup", "no command"};
    int fd = connect_unix(sock_path(home, instance));
    if (fd < 0) return {kExitSetup, "setup", "no running session for '" + std::string(instance) + "'"};

    nlohmann::json req{{"op", "exec"}, {"argv", r.argv}, {"env", r.env}, {"cwd", r.cwd},
                       {"tty", r.tty}};
    if (r.timeout) req["timeout_ms"] = r.timeout->count();
    const int stdio[3] = {0, 1, 2};
    if (!send_msg(fd, req, stdio)) {
        ::close(fd);
        return {kExitSetup, "setup", "could not reach the session"};
    }
    // Ctrl-C and friends belong to the command, not to this client.
    install_signal_pipe({SIGINT, SIGTERM, SIGHUP, SIGQUIT}, {SIGPIPE});
    ExecResult result{kExitSetup, "setup", "the session ended"};
    while (true) {
        pollfd p[2] = {{fd, POLLIN, 0}, {g_sig_pipe[0], POLLIN, 0}};
        ::poll(p, 2, -1);
        for (int sig : drain_signals()) send_msg(fd, {{"op", "signal"}, {"sig", sig}});
        if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        auto m = recv_msg(fd);
        if (!m) break;
        close_all(m->fds);
        const auto& j = m->json;
        if (j.contains("started")) continue;
        if (j.contains("exit")) {
            result = {j["exit"].get<int>(), "", ""};
        } else if (j.contains("signal")) {
            result = {128 + j["signal"].get<int>(), "", ""};
        } else if (j.contains("error")) {
            int e = j.value("errno", 0);
            int code = e == ENOENT ? kExitNotFound
                     : (e == EACCES || e == ENOEXEC || e == EISDIR) ? kExitCannotRun : kExitSetup;
            result = {code, code == kExitSetup ? "setup" : "exec", j.value("error", "")};
        }
        break;
    }
    ::close(fd);
    return result;
}

bool stop(const HomeView& home, std::string_view instance) {
    auto info = find(home, instance);
    if (!info) return false;
    int fd = connect_unix(sock_path(home, instance));
    if (fd >= 0) {
        send_msg(fd, {{"op", "stop"}});
        pollfd p{fd, POLLIN, 0};
        ::poll(&p, 1, 5000);
        ::close(fd);
    }
    for (int i = 0; i < 100 && find(home, instance); ++i) ::usleep(50 * 1000);
    if (find(home, instance)) {
        ::kill(info->sandbox_pid, SIGKILL);
        for (int i = 0; i < 40 && find(home, instance); ++i) ::usleep(50 * 1000);
    }
    return !find(home, instance);
}

// ── inside ───────────────────────────────────────────────────────────

int session_init(std::span<const std::string> args) {
    const char* fd_env = std::getenv(std::string(kControlFdEnv).c_str());
    const int ctl = fd_env ? std::atoi(fd_env) : -1;
    const char* ttl_env = std::getenv(std::string(kTtlEnv).c_str());
    const int ttl = ttl_env ? std::atoi(ttl_env) : 0;
    if (ctl >= 0) ::fcntl(ctl, F_SETFD, FD_CLOEXEC);
    // Neither variable means anything to the commands that run here.
    ::unsetenv(std::string(kControlFdEnv).c_str());
    ::unsetenv(std::string(kTtlEnv).c_str());

    std::vector<std::string> main_argv;
    if (auto it = std::ranges::find(args, std::string("--")); it != args.end())
        main_argv.assign(std::next(it), args.end());

    pid_t main_pid = -1;
    int main_status = 0;
    bool main_done = main_argv.empty();
    if (!main_argv.empty()) {
        main_pid = ::fork();
        if (main_pid == 0) {
            if (ctl >= 0) ::close(ctl);
            exec_resolved(main_argv, current_env());
            std::fprintf(stderr, "xlings: %s: %s\n", main_argv[0].c_str(), std::strerror(errno));
            ::_exit(errno == ENOENT ? kExitNotFound : kExitCannotRun);
        }
        if (main_pid < 0) return kExitSetup;
    }

    // While the main command owns the terminal, Ctrl-C is its business.
    ::signal(SIGINT, SIG_IGN);
    ::signal(SIGQUIT, SIG_IGN);
    ::signal(SIGPIPE, SIG_IGN);

    std::map<pid_t, std::int64_t> joined;
    auto last_activity = std::chrono::steady_clock::now();
    bool ctl_open = ctl >= 0;

    auto kill_all = [&](int sig) {
        for (auto& [pid, id] : joined) ::kill(-pid, sig);
        if (main_pid > 0 && !main_done) ::kill(main_pid, sig);
    };

    while (true) {
        int status = 0;
        pid_t w;
        while ((w = ::waitpid(-1, &status, WNOHANG)) > 0) {
            if (w == main_pid) {
                main_done = true;
                main_status = status;
            } else if (auto it = joined.find(w); it != joined.end()) {
                nlohmann::json reply{{"id", it->second}};
                if (WIFSIGNALED(status)) reply["signal"] = WTERMSIG(status);
                else reply["exit"] = WEXITSTATUS(status);
                if (ctl_open) send_msg(ctl, reply);
                joined.erase(it);
                last_activity = std::chrono::steady_clock::now();
            }
        }
        if (main_done && joined.empty()) {
            if (!main_argv.empty()) return status_to_code(main_status);
            if (!ctl_open) return 0;
            if (ttl > 0 && std::chrono::steady_clock::now() - last_activity >= std::chrono::seconds(ttl))
                return 0;
        }
        if (!ctl_open) {
            ::usleep(100 * 1000);
            continue;
        }

        pollfd p{ctl, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        auto m = recv_msg(ctl);
        if (!m) {
            // The supervisor is gone: nothing outside watches any more.
            ctl_open = false;
            kill_all(SIGTERM);
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

            int report[2];
            if (::pipe2(report, O_CLOEXEC) != 0) {
                send_msg(ctl, {{"id", id}, {"error", "pipe failed"}, {"errno", errno}});
                close_all(m->fds);
                continue;
            }
            pid_t pid = ::fork();
            if (pid == 0) {
                ::close(report[0]);
                ::close(ctl);
                for (int s : {SIGINT, SIGQUIT, SIGPIPE}) ::signal(s, SIG_DFL);
                // A session of its own: no controlling terminal to inject
                // into. A terminal it was handed is tried as its controlling
                // one; when the terminal already belongs to another session
                // (the common case) it runs without job control.
                ::setsid();
                for (int i = 0; i < 3; ++i) ::dup2(m->fds[static_cast<std::size_t>(i)], i);
                for (int fd : m->fds) if (fd > 2) ::close(fd);
                if (tty) (void)::ioctl(0, TIOCSCTTY, 0);
                if (::chdir(cwd.c_str()) != 0) (void)::chdir("/");
                exec_resolved(argv, env);
                int e = errno;
                (void)::write(report[1], &e, sizeof(e));
                ::_exit(e == ENOENT ? kExitNotFound : kExitCannotRun);
            }
            ::close(report[1]);
            close_all(m->fds);
            if (pid < 0) {
                ::close(report[0]);
                send_msg(ctl, {{"id", id}, {"error", "fork failed"}, {"errno", errno}});
                continue;
            }
            int e = 0;
            if (::read(report[0], &e, sizeof(e)) == static_cast<ssize_t>(sizeof(e))) {
                ::close(report[0]);
                ::waitpid(pid, nullptr, 0);
                send_msg(ctl, {{"id", id}, {"error", std::strerror(e)}, {"errno", e}});
                continue;
            }
            ::close(report[0]);
            joined[pid] = id;
            last_activity = std::chrono::steady_clock::now();
            send_msg(ctl, {{"id", id}, {"started", true}, {"pid", pid}});
        } else if (op == "kill" || op == "signal") {
            int sig = op == "kill" ? SIGKILL : m->json.value("sig", SIGTERM);
            for (auto& [pid, jid] : joined) if (jid == id) ::kill(-pid, sig);
            close_all(m->fds);
        } else if (op == "stop") {
            kill_all(SIGTERM);
            close_all(m->fds);
            return 0;
        } else {
            close_all(m->fds);
        }
    }
}

#endif

}  // namespace xlings::subos::session
