module;

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#define XLINGS_ENVIRON (*_NSGetEnviron())
#else
// glibc and musl declare environ in <unistd.h> under _GNU_SOURCE
#define XLINGS_ENVIRON environ
#endif
#elif defined(_WIN32)
#include <io.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

module xlings.platform;

import std;

namespace xlings::platform {

namespace sig {
#if defined(_WIN32)
extern const int interrupt = 2, quit = 3, terminate = 15, hangup = 1, pipe = 13, kill = 9;
#else
extern const int interrupt = SIGINT, quit = SIGQUIT, terminate = SIGTERM, hangup = SIGHUP,
          pipe = SIGPIPE, kill = SIGKILL;
#endif
}  // namespace sig

int exit_code(const ExitStatus& s) {
    return s.exited ? s.code : 128 + s.signal;
}

int last_error() { return errno; }
std::string error_text(int error) { return std::strerror(error); }
bool is_not_found(int error) { return error == ENOENT; }
bool is_not_executable(int error) { return error == EACCES || error == ENOEXEC || error == EISDIR; }

void close_fds(std::vector<int>& fds) {
    for (int fd : fds) close_fd(fd);
    fds.clear();
}

#if defined(__linux__) || defined(__APPLE__)

namespace {

// The descriptor flags Linux sets atomically; macOS sets them after the fact
// (nothing here is threaded between the two calls).
void cloexec(int fd) { if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC); }

std::vector<std::string> env_strings(const std::map<std::string, std::string>& env) {
    std::vector<std::string> out;
    out.reserve(env.size());
    for (const auto& [k, v] : env) out.push_back(k + "=" + v);
    return out;
}

std::vector<char*> c_array(std::vector<std::string>& v) {
    std::vector<char*> out;
    for (auto& s : v) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

ExitStatus decode(int status) {
    if (WIFEXITED(status)) return {true, WEXITSTATUS(status), 0};
    if (WIFSIGNALED(status)) return {false, 0, WTERMSIG(status)};
    return {false, 0, 0};
}

int g_signal_pipe[2] = {-1, -1};

extern "C" void on_routed_signal(int s) {
    const int saved = errno;
    const unsigned char b = static_cast<unsigned char>(s);
    (void)::write(g_signal_pipe[1], &b, 1);
    errno = saved;
}

// sockaddr_un holds ~108 bytes; a longer path goes through its directory
// (/proc/self/fd/<n>/name on Linux).
struct UnixAddr {
    sockaddr_un addr{};
    socklen_t len{};
    int dirfd { -1 };
    ~UnixAddr() { if (dirfd >= 0) ::close(dirfd); }
};

bool make_addr(const std::filesystem::path& path, UnixAddr& a) {
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

int seqpacket_socket() {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    cloexec(fd);
    return fd;
}

}  // namespace

bool send_signal(int pid, int signal) { return pid > 0 && ::kill(pid, signal) == 0; }
bool send_signal_group(int pgid, int signal) { return pgid > 0 && ::kill(-pgid, signal) == 0; }

int route_signals(std::initializer_list<int> forward, std::initializer_list<int> ignore) {
    if (g_signal_pipe[0] < 0) {
        if (::pipe(g_signal_pipe) != 0) return -1;
        for (int fd : g_signal_pipe) {
            cloexec(fd);
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
    }
    struct sigaction sa{};
    sa.sa_handler = on_routed_signal;
    sigemptyset(&sa.sa_mask);
    for (int s : forward) ::sigaction(s, &sa, nullptr);
    ignore_signals(ignore);
    return g_signal_pipe[0];
}

std::vector<int> pending_signals() {
    std::vector<int> out;
    unsigned char b;
    while (g_signal_pipe[0] >= 0 && ::read(g_signal_pipe[0], &b, 1) == 1) out.push_back(b);
    return out;
}

void ignore_signals(std::initializer_list<int> signals) {
    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    for (int s : signals) ::sigaction(s, &ign, nullptr);
}

void reset_signals() {
    for (int s : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD}) ::signal(s, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
}

int fork_process() {
    std::fflush(nullptr);
    return static_cast<int>(::fork());
}

void exit_now(int code) { ::_exit(code); }

int exec_path(const std::vector<std::string>& argv, const std::map<std::string, std::string>& env) {
    if (argv.empty()) return EINVAL;
    auto args = argv;
    auto envs = env_strings(env);
    auto av = c_array(args);
    auto ev = c_array(envs);
    ::execve(av[0], av.data(), ev.data());
    return errno;
}

int exec_program(const std::vector<std::string>& argv, const std::map<std::string, std::string>& env) {
    if (argv.empty()) return EINVAL;
    if (argv[0].find('/') != std::string::npos) return exec_path(argv, env);
    auto args = argv;
    auto envs = env_strings(env);
    auto av = c_array(args);
    auto ev = c_array(envs);
    std::string path = "/usr/local/bin:/usr/bin:/bin";
    if (auto it = env.find("PATH"); it != env.end()) path = it->second;
    int saved = ENOENT;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        auto dir = path.substr(start, end - start);
        if (dir.empty()) dir = ".";
        const auto candidate = dir + "/" + argv[0];
        ::execve(candidate.c_str(), av.data(), ev.data());
        if (errno == EACCES) saved = EACCES;
        else if (errno != ENOENT && errno != ENOTDIR) saved = errno;
        start = end + 1;
    }
    return saved;
}

std::optional<ExitStatus> wait_process(int pid, bool block) {
    int status = 0;
    pid_t r;
    do { r = ::waitpid(pid, &status, block ? 0 : WNOHANG); } while (r < 0 && errno == EINTR);
    if (r != pid) return std::nullopt;
    return decode(status);
}

std::optional<std::pair<int, ExitStatus>> reap_child() {
    int status = 0;
    const pid_t r = ::waitpid(-1, &status, WNOHANG);
    if (r <= 0) return std::nullopt;
    return std::pair{static_cast<int>(r), decode(status)};
}

bool new_session() { return ::setsid() >= 0; }
bool take_controlling_terminal() { return ::ioctl(0, TIOCSCTTY, 0) == 0; }

void redirect_stdio(std::span<const int> fds) {
    for (std::size_t i = 0; i < 3 && i < fds.size(); ++i) ::dup2(fds[i], static_cast<int>(i));
    for (int fd : fds) if (fd > 2) ::close(fd);
}

int run_argv_with_timeout(const std::vector<std::string>& argv, std::chrono::milliseconds limit) {
    if (argv.empty()) return 127;
    const int pid = fork_process();
    if (pid < 0) return 126;
    if (pid == 0) {
        ::setpgid(0, 0);
        const int e = exec_program(argv, environment());
        ::_exit(e == ENOENT ? 127 : 126);
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (true) {
        if (auto s = wait_process(pid, false)) return exit_code(*s);
        if (std::chrono::steady_clock::now() >= deadline) {
            send_signal_group(pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            send_signal_group(pid, SIGKILL);
            (void)wait_process(pid, true);
            return 124;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

UserIds user_ids() {
    return {static_cast<unsigned>(::getuid()), static_cast<unsigned>(::getgid()),
            static_cast<unsigned>(::geteuid())};
}

bool stdout_is_terminal() { return ::isatty(1) == 1; }

std::map<std::string, std::string> environment() {
    std::map<std::string, std::string> env;
    for (char** e = XLINGS_ENVIRON; e && *e; ++e) {
        std::string_view kv(*e);
        if (auto eq = kv.find('='); eq != std::string_view::npos)
            env[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
    }
    return env;
}

void unset_env_variable(const std::string& name) { ::unsetenv(name.c_str()); }

bool set_inheritable(int fd, bool inheritable) {
    return fd >= 0 && ::fcntl(fd, F_SETFD, inheritable ? 0 : FD_CLOEXEC) == 0;
}

std::optional<std::array<int, 2>> make_pipe(bool close_on_exec) {
    int p[2];
    if (::pipe(p) != 0) return std::nullopt;
    if (close_on_exec) { cloexec(p[0]); cloexec(p[1]); }
    return std::array<int, 2>{p[0], p[1]};
}

bool read_exact(int fd, void* buf, std::size_t size) {
    auto* p = static_cast<char*>(buf);
    while (size > 0) {
        const auto n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

int open_null() {
    const int fd = ::open("/dev/null", O_RDWR);
    cloexec(fd);
    return fd;
}

int open_for_append(const std::filesystem::path& path) {
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
}

int poll_fds(std::span<PollFd> fds, int timeout_ms) {
    std::vector<pollfd> p;
    p.reserve(fds.size());
    for (auto& f : fds) p.push_back({f.fd, POLLIN, 0});
    int n;
    do { n = ::poll(p.data(), p.size(), timeout_ms); } while (n < 0 && errno == EINTR);
    for (std::size_t i = 0; i < fds.size(); ++i) {
        fds[i].readable = (p[i].revents & POLLIN) != 0;
        fds[i].closed = (p[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
    return n < 0 ? 0 : n;
}

int unix_listen(const std::filesystem::path& path) {
    const int fd = seqpacket_socket();
    if (fd < 0) return -1;
    UnixAddr a;
    if (!make_addr(path, a)) { ::close(fd); return -1; }
    ::unlink(path.c_str());
    const auto old = ::umask(0077);
    const int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a.addr), a.len);
    ::umask(old);
    if (rc != 0 || ::listen(fd, 16) != 0) { ::close(fd); return -1; }
    return fd;
}

int unix_connect(const std::filesystem::path& path) {
    const int fd = seqpacket_socket();
    if (fd < 0) return -1;
    UnixAddr a;
    if (!make_addr(path, a) || ::connect(fd, reinterpret_cast<sockaddr*>(&a.addr), a.len) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int unix_accept(int listen_fd) {
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    cloexec(fd);
    return fd;
}

std::optional<std::array<int, 2>> unix_pair() {
    int p[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, p) != 0) return std::nullopt;
    cloexec(p[0]);
    cloexec(p[1]);
    return std::array<int, 2>{p[0], p[1]};
}

bool send_message(int sock, std::string_view data, std::span<const int> fds) {
    iovec iov{ const_cast<char*>(data.data()), data.size() };
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    std::vector<char> control;
    if (!fds.empty()) {
        control.resize(CMSG_SPACE(sizeof(int) * fds.size()));
        msg.msg_control = control.data();
        msg.msg_controllen = static_cast<decltype(msg.msg_controllen)>(control.size());
        cmsghdr* cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(cm), fds.data(), sizeof(int) * fds.size());
    }
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;    // macOS: SIGPIPE is ignored by the callers
#endif
    while (true) {
        const auto n = ::sendmsg(sock, &msg, flags);
        if (n >= 0) return static_cast<std::size_t>(n) == data.size();
        if (errno != EINTR) return false;
    }
}

std::optional<Message> receive_message(int sock, std::size_t max) {
    constexpr int kMaxFds = 8;
    std::vector<char> buf(max);
    iovec iov{ buf.data(), buf.size() };
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxFds)];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
#if defined(MSG_CMSG_CLOEXEC)
    constexpr int flags = MSG_CMSG_CLOEXEC;
#else
    constexpr int flags = 0;
#endif
    ssize_t n;
    do { n = ::recvmsg(sock, &msg, flags); } while (n < 0 && errno == EINTR);
    if (n <= 0) return std::nullopt;
    Message out;
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        const auto count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; ++i) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
            if constexpr (flags == 0) cloexec(fd);
            out.fds.push_back(fd);
        }
    }
    out.data.assign(buf.data(), static_cast<std::size_t>(n));
    return out;
}

#else  // no POSIX process model here

bool send_signal(int, int) { return false; }
bool send_signal_group(int, int) { return false; }
int route_signals(std::initializer_list<int>, std::initializer_list<int>) { return -1; }
std::vector<int> pending_signals() { return {}; }
void ignore_signals(std::initializer_list<int>) {}
void reset_signals() {}
int fork_process() { return -1; }
void exit_now(int code) { std::_Exit(code); }
int exec_program(const std::vector<std::string>&, const std::map<std::string, std::string>&) { return ENOSYS; }
int exec_path(const std::vector<std::string>&, const std::map<std::string, std::string>&) { return ENOSYS; }
std::optional<ExitStatus> wait_process(int, bool) { return std::nullopt; }
std::optional<std::pair<int, ExitStatus>> reap_child() { return std::nullopt; }
bool new_session() { return false; }
bool take_controlling_terminal() { return false; }
void redirect_stdio(std::span<const int>) {}
int run_argv_with_timeout(const std::vector<std::string>& argv, std::chrono::milliseconds) {
    return run_argv(argv);
}
UserIds user_ids() { return {}; }
bool stdout_is_terminal() { return ::_isatty(1) != 0; }
std::map<std::string, std::string> environment() {
    std::map<std::string, std::string> env;
    wchar_t* block = ::GetEnvironmentStringsW();
    auto utf8 = [](std::wstring_view w) {
        if (w.empty()) return std::string{};
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                              nullptr, nullptr);
        return out;
    };
    for (const wchar_t* p = block; p && *p; p += std::wcslen(p) + 1) {
        std::wstring_view kv(p);
        const auto eq = kv.find(L'=', 1);   // "=C:=C:\" entries start with '='
        if (eq == std::wstring_view::npos) continue;
        env[utf8(kv.substr(0, eq))] = utf8(kv.substr(eq + 1));
    }
    if (block) ::FreeEnvironmentStringsW(block);
    return env;
}
void unset_env_variable(const std::string& name) { set_env_variable(name, ""); }
bool set_inheritable(int, bool) { return false; }
std::optional<std::array<int, 2>> make_pipe(bool) { return std::nullopt; }
bool read_exact(int, void*, std::size_t) { return false; }
int open_null() { return -1; }
int open_for_append(const std::filesystem::path&) { return -1; }
int poll_fds(std::span<PollFd>, int) { return 0; }
int unix_listen(const std::filesystem::path&) { return -1; }
int unix_connect(const std::filesystem::path&) { return -1; }
int unix_accept(int) { return -1; }
std::optional<std::array<int, 2>> unix_pair() { return std::nullopt; }
bool send_message(int, std::string_view, std::span<const int>) { return false; }
std::optional<Message> receive_message(int, std::size_t) { return std::nullopt; }

#endif

}  // namespace xlings::platform
