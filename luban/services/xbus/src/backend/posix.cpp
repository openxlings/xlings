// xbus.detail.backend on Linux and macOS: AF_UNIX stream sockets carrying
// SCM_RIGHTS, poll, SO_PEERCRED / LOCAL_PEERCRED, memfd (Linux) or shm_open
// (macOS).
module;

#if defined(__linux__) || defined(__APPLE__)   // platform-if-ok: xbus's POSIX backend; one per platform
#define XBUS_POSIX 1
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)   // platform-if-ok: macOS names its peer credentials differently
#include <sys/ucred.h>
#endif
#endif

module xbus.detail.backend;

import std;

#if defined(XBUS_POSIX)   // platform-if-ok: see above
namespace xbus::detail::backend {

namespace {

namespace fs = std::filesystem;

int fd_(const Handle& h) { return static_cast<int>(h.value()); }

void cloexec_(int fd) {
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

void nonblocking_(int fd) {
    if (const int flags = ::fcntl(fd, F_GETFL); flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void nosigpipe_(int fd) {
#if defined(__APPLE__)   // platform-if-ok: macOS has no MSG_NOSIGNAL
    if (fd >= 0) { int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#else
    (void)fd;
#endif
}

fs::path runtime_() {
    if (const char* r = std::getenv("XDG_RUNTIME_DIR"); r && *r) return r;
    std::error_code ec;
    return fs::temp_directory_path(ec) / std::format("xbus-{}", ::getuid());
}

// local:<name> under the runtime place, local:/<path> there; unix:<path> exactly.
std::optional<fs::path> path_(std::string_view scheme, std::string_view where) {
    if (scheme == "unix") return fs::path(where);
    if (scheme != "local") return std::nullopt;
    if (!where.empty() && where.front() == '/') return fs::path(where);
    return runtime_() / "openbus" / (std::string(where) + ".sock");
}

// A path longer than sockaddr_un holds is reached through its directory.
struct Address {
    sockaddr_un addr {};
    socklen_t len { 0 };
    int dirfd { -1 };
    ~Address() { if (dirfd >= 0) ::close(dirfd); }
};

bool address_(const fs::path& path, Address& a) {
    a.addr.sun_family = AF_UNIX;
    auto s = path.string();
    if (s.size() >= sizeof a.addr.sun_path) {
#if defined(__linux__)   // platform-if-ok: /proc/self/fd is Linux's
        a.dirfd = ::open(path.parent_path().c_str(), O_PATH | O_CLOEXEC);
        if (a.dirfd < 0) return false;
        s = std::format("/proc/self/fd/{}/{}", a.dirfd, path.filename().string());
        if (s.size() >= sizeof a.addr.sun_path) return false;
#else
        return false;
#endif
    }
    std::memcpy(a.addr.sun_path, s.c_str(), s.size() + 1);
    a.len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + s.size() + 1);
    return true;
}

int socket_() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    cloexec_(fd);
    nosigpipe_(fd);
    return fd;
}

}  // namespace

void Handle::reset() {
    if (v_ >= 0) ::close(static_cast<int>(v_));
    v_ = -1;
}

std::expected<Handle, std::string> listen(std::string_view scheme, std::string_view where, bool shared) {
    const auto path = path_(scheme, where);
    if (!path) return std::unexpected(std::format("this backend has no {}: addresses", scheme));
    if (connect(scheme, where)) return std::unexpected(std::format("a router already answers at {}", path->string()));
    std::error_code ec;
    fs::create_directories(path->parent_path(), ec);
    ::chmod(path->parent_path().c_str(), shared ? 0755 : 0700);
    Handle h(socket_());
    if (!h.valid()) return std::unexpected(std::format("no socket: {}", std::strerror(errno)));
    Address a;
    if (!address_(*path, a)) return std::unexpected("the path is too long: " + path->string());
    ::unlink(path->c_str());
    const auto old = ::umask(shared ? 0 : 0077);
    const int rc = ::bind(fd_(h), reinterpret_cast<sockaddr*>(&a.addr), a.len);
    ::umask(old);
    if (rc != 0 || ::listen(fd_(h), 64) != 0) return std::unexpected(std::format("cannot listen at {}: {}", path->string(), std::strerror(errno)));
    nonblocking_(fd_(h));
    return h;
}

std::expected<Handle, std::string> connect(std::string_view scheme, std::string_view where) {
    const auto path = path_(scheme, where);
    if (!path) return std::unexpected(std::format("this backend has no {}: addresses", scheme));
    Handle h(socket_());
    if (!h.valid()) return std::unexpected(std::format("no socket: {}", std::strerror(errno)));
    Address a;
    if (!address_(*path, a) || ::connect(fd_(h), reinterpret_cast<sockaddr*>(&a.addr), a.len) != 0)
        return std::unexpected(std::format("nothing answers at {}", path->string()));
    nonblocking_(fd_(h));
    return h;
}

Handle accept(const Handle& listener) {
    int fd;
    do { fd = ::accept(fd_(listener), nullptr, nullptr); } while (fd < 0 && errno == EINTR);
    cloexec_(fd);
    nosigpipe_(fd);
    if (fd >= 0) nonblocking_(fd);
    return Handle(fd);
}

std::optional<std::array<Handle, 2>> pair() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return std::nullopt;
    for (const int fd : sv) { cloexec_(fd); nosigpipe_(fd); nonblocking_(fd); }
    return std::array<Handle, 2>{Handle(sv[0]), Handle(sv[1])};
}

std::string location(std::string_view scheme, std::string_view where) {
    const auto p = path_(scheme, where);
    return p ? p->string() : std::string(where);
}

void forget(std::string_view scheme, std::string_view where) {
    if (const auto p = path_(scheme, where)) ::unlink(p->c_str());
}

std::optional<Received> read(const Handle& h, std::span<char> buf) {
    iovec iov{buf.data(), buf.size()};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    ssize_t n;
    do { n = ::recvmsg(fd_(h), &msg, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return Received{.would_block = true};
        return std::nullopt;
    }
    Received r{.bytes = static_cast<std::size_t>(n)};
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t k = 0; k < count; ++k) {
            int got;
            std::memcpy(&got, CMSG_DATA(c) + k * sizeof(int), sizeof(int));
            cloexec_(got);
            r.handles.emplace_back(got);
        }
    }
    return r;
}

long write(const Handle& h, std::string_view bytes, std::span<const Handle* const> handles) {
    iovec iov{const_cast<char*>(bytes.data()), bytes.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    if (!handles.empty()) {
        const auto n = std::min<std::size_t>(handles.size(), 16);
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * n);
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * n);
        for (std::size_t k = 0; k < n; ++k) {
            const int fd = fd_(*handles[k]);
            std::memcpy(CMSG_DATA(c) + k * sizeof(int), &fd, sizeof(int));
        }
    }
#if defined(__linux__)   // platform-if-ok: MSG_NOSIGNAL is Linux's (macOS: SO_NOSIGPIPE)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    ssize_t n;
    do { n = ::sendmsg(fd_(h), &msg, kFlags); } while (n < 0 && errno == EINTR);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    return static_cast<long>(n);
}

void wait(std::span<Ready> set, int timeout_ms) {
    std::vector<pollfd> p;
    p.reserve(set.size());
    for (const auto& r : set) p.push_back({r.handle ? fd_(*r.handle) : -1, static_cast<short>(POLLIN | (r.want_write ? POLLOUT : 0)), 0});
    int n;
    do { n = ::poll(p.data(), p.size(), timeout_ms); } while (n < 0 && errno == EINTR);
    for (std::size_t k = 0; k < set.size(); ++k) {
        set[k].readable = (p[k].revents & POLLIN) != 0;
        set[k].writable = (p[k].revents & POLLOUT) != 0;
        set[k].closed = (p[k].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
}

std::string identity(const Handle& h) {
#if defined(__linux__)   // platform-if-ok: SO_PEERCRED
    ucred c{};
    socklen_t len = sizeof c;
    if (::getsockopt(fd_(h), SOL_SOCKET, SO_PEERCRED, &c, &len) != 0) return {};
    return std::format("uid {}, pid {}", c.uid, c.pid);
#else
    xucred c{};
    socklen_t len = sizeof c;
    if (::getsockopt(fd_(h), SOL_LOCAL, LOCAL_PEERCRED, &c, &len) != 0) return {};
    pid_t pid = -1;
    socklen_t plen = sizeof pid;
    ::getsockopt(fd_(h), SOL_LOCAL, LOCAL_PEERPID, &pid, &plen);
    return std::format("uid {}, pid {}", c.cr_uid, pid);
#endif
}

std::optional<Shared> create_shared(std::string_view name, std::size_t size) {
#if defined(__linux__)   // platform-if-ok: memfd is Linux's
    const int fd = ::memfd_create(std::string(name).c_str(), MFD_CLOEXEC);
#else
    const auto shm = std::format("/xbus-{}-{}", ::getpid(), name);
    const int fd = ::shm_open(shm.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) ::shm_unlink(shm.c_str());
#endif
    if (fd < 0) return std::nullopt;
    cloexec_(fd);
    Handle h(fd);
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) return std::nullopt;
    return map_shared(std::move(h), size);
}

std::optional<Shared> map_shared(Handle h, std::size_t size) {
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_(h), 0);
    if (p == MAP_FAILED) return std::nullopt;
    return Shared{std::move(h), static_cast<std::byte*>(p), size};
}

void unmap(Shared& s) {
    if (s.data) ::munmap(s.data, s.size);
    s.data = nullptr;
    s.handle.reset();
}

Handle duplicate(const Handle& h) {
    const int fd = ::fcntl(fd_(h), F_DUPFD_CLOEXEC, 3);
    return Handle(fd);
}

std::uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::string host_name() {
    char buf[256]{};
    if (::gethostname(buf, sizeof buf - 1) != 0) return "localhost";
    return buf;
}

std::optional<std::string> environment(std::string_view name) {
    const char* v = std::getenv(std::string(name).c_str());
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

bool spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    const pid_t first = ::fork();
    if (first < 0) return false;
    if (first == 0) {
        ::setsid();
        const pid_t second = ::fork();
        if (second != 0) ::_exit(second < 0 ? 1 : 0);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) { ::dup2(null, 0); ::dup2(null, 1); ::dup2(null, 2); }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execv(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(first, &status, 0) < 0 && errno == EINTR) {}
    return true;
}

}  // namespace xbus::detail::backend
#endif
