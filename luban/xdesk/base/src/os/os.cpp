module;

// The POSIX side of xdesk.os (os_windows.cpp is the other).
#if !defined(_WIN32)   // platform-if-ok: xdesk.os is xdesk's own system layer; this file is its POSIX half
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)   // platform-if-ok: macOS gives a program its environment through _NSGetEnviron
#include <crt_externs.h>
#endif
#endif

module xdesk.os;

import std;

#if !defined(_WIN32)   // platform-if-ok: the POSIX half (see above)
namespace xdesk::os {

namespace {

void cloexec_(int fd) {
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

bool address_(const fs::path& path, sockaddr_un& a) {
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    const auto p = path.string();
    if (p.size() >= sizeof a.sun_path) return false;
    std::memcpy(a.sun_path, p.c_str(), p.size() + 1);
    return true;
}

}  // namespace

int poll(std::span<PollFd> fds, int timeout_ms) {
    std::vector<pollfd> p(fds.size());
    for (std::size_t i = 0; i < fds.size(); ++i) p[i] = {fds[i].fd, POLLIN, 0};
    int n;
    do { n = ::poll(p.data(), p.size(), timeout_ms); } while (n < 0 && errno == EINTR);
    for (std::size_t i = 0; i < fds.size(); ++i) {
        fds[i].readable = n > 0 && (p[i].revents & POLLIN);
        fds[i].closed = n > 0 && (p[i].revents & (POLLHUP | POLLERR | POLLNVAL));
    }
    return n < 0 ? 0 : n;
}

std::optional<std::size_t> read_some(int fd, std::span<char> buf) {
    for (;;) {
        const auto n = ::read(fd, buf.data(), buf.size());
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno != EINTR) return std::nullopt;
    }
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const auto n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                pollfd p{fd, POLLOUT, 0};
                ::poll(&p, 1, 1000);
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

void close(int fd) {
    if (fd >= 0) ::close(fd);
}

int open_append(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    return fd;
}

int open_device(const fs::path& path, std::string* error) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0 && error) *error = std::strerror(errno);
    return fd;
}

std::optional<std::array<int, 2>> pipe() {
    int p[2];
    if (::pipe(p) != 0) return std::nullopt;
    cloexec_(p[0]);
    cloexec_(p[1]);
    return std::array<int, 2>{p[0], p[1]};
}

int connect_unix(const fs::path& path) {
    sockaddr_un a;
    if (!address_(path, a)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    cloexec_(fd);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return -1; }
    return fd;
}

std::optional<std::size_t> recv_with_fds(int sock, std::span<char> buf, std::vector<int>& fds) {
    iovec iov{buf.data(), buf.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    std::array<char, CMSG_SPACE(sizeof(int) * 16)> control{};
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
#if defined(MSG_CMSG_CLOEXEC)
    constexpr int flags = MSG_CMSG_CLOEXEC;
#else
    constexpr int flags = 0;        // macOS: each one is made close-on-exec below
#endif
    ssize_t n;
    do { n = ::recvmsg(sock, &msg, flags); } while (n < 0 && errno == EINTR);
    if (n < 0) return std::nullopt;
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        const auto count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t k = 0; k < count; ++k) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(cm) + k * sizeof(int), sizeof(int));
            if constexpr (flags == 0) cloexec_(fd);
            fds.push_back(fd);
        }
    }
    return static_cast<std::size_t>(n);
}

bool send_with_fds(int sock, std::string_view data, std::span<const int> fds) {
    iovec iov{const_cast<char*>(data.data()), data.size()};
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
#if defined(MSG_NOSIGNAL)
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    for (;;) {
        const auto n = ::sendmsg(sock, &msg, flags);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        // The descriptors went with the first byte; the rest is a plain write.
        return write_all(sock, data.substr(static_cast<std::size_t>(n)));
    }
}

int listen_unix(const fs::path& path) {
    sockaddr_un a;
    if (!address_(path, a)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    cloexec_(fd);
    ::unlink(path.c_str());
    const auto old = ::umask(077);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::umask(old);
    if (!bound || ::listen(fd, 16) != 0) { ::close(fd); return -1; }
    return fd;
}

int accept(int listener) {
    int fd;
    do { fd = ::accept(listener, nullptr, nullptr); } while (fd < 0 && errno == EINTR);
    cloexec_(fd);
    return fd;
}

std::optional<SharedMemory> shared_memory(std::size_t size) {
#if defined(__linux__)   // platform-if-ok: xdesk.os is xdesk's own system layer (it does not use xlings.platform); memfd is Linux's
    const int fd = ::memfd_create("xdesk", MFD_CLOEXEC);
#else
    const int fd = -1;
#endif
    if (fd < 0) return std::nullopt;
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) { ::close(fd); return std::nullopt; }
    void* at = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (at == MAP_FAILED) { ::close(fd); return std::nullopt; }
    return SharedMemory{fd, static_cast<std::byte*>(at), size};
}

std::optional<SharedMemory> map_file(const fs::path& path, std::size_t size) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    void* at = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (at == MAP_FAILED) { ::close(fd); return std::nullopt; }
    return SharedMemory{fd, static_cast<std::byte*>(at), size};
}

void release(SharedMemory& m) {
    if (m.data) ::munmap(m.data, m.size);
    if (m.fd >= 0) ::close(m.fd);
    m = SharedMemory{};
}

int pid() { return static_cast<int>(::getpid()); }

fs::path executable() {
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p;
}

std::map<std::string, std::string> environment() {
    std::map<std::string, std::string> out;
#if defined(__APPLE__)   // platform-if-ok: see the include
    char** const all = *::_NSGetEnviron();
#else
    char** const all = environ;
#endif
    for (char** e = all; e && *e; ++e) {
        const std::string_view kv(*e);
        if (const auto eq = kv.find('='); eq != std::string_view::npos)
            out.emplace(std::string(kv.substr(0, eq)), std::string(kv.substr(eq + 1)));
    }
    return out;
}

void ignore_broken_pipes() { ::signal(SIGPIPE, SIG_IGN); }

int spawn(const Spawn& s) {
    if (s.argv.empty()) return -1;
    std::vector<std::string> envs;
    for (const auto& [k, v] : s.env) envs.push_back(k + "=" + v);
    std::vector<char*> argv, envp;
    for (const auto& a : s.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (auto& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    // The search runs here, before fork: the child only execs.
    std::string program = s.argv[0];
    if (program.find('/') == std::string::npos) {
        std::string path = s.env.contains("PATH") ? s.env.at("PATH") : std::string("/usr/bin:/bin");
        for (std::size_t from = 0; from <= path.size();) {
            const auto colon = path.find(':', from);
            const auto dir = path.substr(from, colon == std::string::npos ? std::string::npos : colon - from);
            const auto candidate = (dir.empty() ? std::string(".") : dir) + "/" + s.argv[0];
            if (::access(candidate.c_str(), X_OK) == 0) { program = candidate; break; }
            if (colon == std::string::npos) break;
            from = colon + 1;
        }
    }
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child != 0) return static_cast<int>(child);
    if (s.new_session) ::setsid();
    const int null = ::open("/dev/null", O_RDWR);
    for (int k = 0; k < 3; ++k) ::dup2(s.stdio[k] >= 0 ? s.stdio[k] : null, k);
    if (!s.cwd.empty() && ::chdir(s.cwd.c_str()) != 0) {}
    ::signal(SIGPIPE, SIG_DFL);
    ::execve(program.c_str(), argv.data(), envp.data());
    ::_exit(127);
}

std::optional<int> exited(int pid) {
    int status = 0;
    const auto r = ::waitpid(pid, &status, WNOHANG);
    if (r == 0) return std::nullopt;
    if (r < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int wait(int pid) {
    int status = 0;
    pid_t r;
    do { r = ::waitpid(pid, &status, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

bool terminate(int pid) { return ::kill(pid, SIGTERM) == 0; }
bool kill(int pid) { return ::kill(pid, SIGKILL) == 0; }

}  // namespace xdesk::os
#endif
