// xbus.detail.backend on Windows: AF_UNIX stream sockets (Windows 10 1803
// and later) and WSAPoll; shared memory by a file mapping. A handle does not
// travel with a frame here (no SCM_RIGHTS): a write with handles fails, and
// XBus falls back to copying, counted (design 2026-10-22 §10).
module;

#if defined(_WIN32)   // platform-if-ok: xbus's Windows backend; one per platform
#define XBUS_WINDOWS 1
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#include <process.h>
#endif

module xbus.detail.backend;

import std;

#if defined(XBUS_WINDOWS)   // platform-if-ok: see above
namespace xbus::detail::backend {

namespace {

namespace fs = std::filesystem;

SOCKET sock_(const Handle& h) { return static_cast<SOCKET>(h.value()); }

bool started_() {
    static const bool ok = [] {
        WSADATA d;
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

fs::path runtime_() {
    std::error_code ec;
    return fs::temp_directory_path(ec) / "openbus";
}

std::optional<fs::path> path_(std::string_view scheme, std::string_view where) {
    if (scheme == "unix") return fs::path(std::string(where));
    if (scheme != "local") return std::nullopt;
    const fs::path given{std::string(where)};
    if (given.is_absolute()) return given;
    return runtime_() / (std::string(where) + ".sock");
}

bool address_(const fs::path& path, sockaddr_un& a) {
    a.sun_family = AF_UNIX;
    const auto s = path.string();
    if (s.size() >= sizeof a.sun_path) return false;
    std::memcpy(a.sun_path, s.c_str(), s.size() + 1);
    return true;
}

void nonblocking_(SOCKET s) {
    u_long on = 1;
    ::ioctlsocket(s, FIONBIO, &on);
}

}  // namespace

void Handle::reset() {
    if (v_ < 0) return;
    if (::closesocket(static_cast<SOCKET>(v_)) != 0) ::CloseHandle(reinterpret_cast<HANDLE>(v_));
    v_ = -1;
}

std::expected<Handle, std::string> listen(std::string_view scheme, std::string_view where, bool shared) {
    (void)shared;   // a user's socket under the user's temporary place: the user's own
    if (!started_()) return std::unexpected("Winsock did not start");
    const auto path = path_(scheme, where);
    if (!path) return std::unexpected(std::format("this backend has no {}: addresses", scheme));
    if (connect(scheme, where)) return std::unexpected("a router already answers at " + path->string());
    std::error_code ec;
    fs::create_directories(path->parent_path(), ec);
    const SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return std::unexpected("this Windows has no AF_UNIX sockets (Windows 10 1803 or later)");
    Handle h(static_cast<std::intptr_t>(s));
    sockaddr_un a{};
    if (!address_(*path, a)) return std::unexpected("the path is too long: " + path->string());
    fs::remove(*path, ec);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 64) != 0)
        return std::unexpected("cannot listen at " + path->string());
    nonblocking_(s);
    return h;
}

std::expected<Handle, std::string> connect(std::string_view scheme, std::string_view where) {
    if (!started_()) return std::unexpected("Winsock did not start");
    const auto path = path_(scheme, where);
    if (!path) return std::unexpected(std::format("this backend has no {}: addresses", scheme));
    const SOCKET s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return std::unexpected("this Windows has no AF_UNIX sockets");
    Handle h(static_cast<std::intptr_t>(s));
    sockaddr_un a{};
    if (!address_(*path, a) || ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        return std::unexpected("nothing answers at " + path->string());
    nonblocking_(s);
    return h;
}

Handle accept(const Handle& listener) {
    const SOCKET s = ::accept(sock_(listener), nullptr, nullptr);
    if (s == INVALID_SOCKET) return Handle();
    nonblocking_(s);
    return Handle(static_cast<std::intptr_t>(s));
}

// No socketpair: a listener on a fresh local path, one connect, one accept.
std::optional<std::array<Handle, 2>> pair() {
    const auto name = std::format("pair-{}-{}", ::_getpid(), std::random_device{}());
    auto l = listen("local", name);
    if (!l) return std::nullopt;
    auto c = connect("local", name);
    if (!c) return std::nullopt;
    Handle a;
    for (int k = 0; k < 100 && !a.valid(); ++k) {
        a = accept(*l);
        if (!a.valid()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    forget("local", name);
    if (!a.valid()) return std::nullopt;
    return std::array<Handle, 2>{std::move(a), std::move(*c)};
}

std::string location(std::string_view scheme, std::string_view where) {
    const auto p = path_(scheme, where);
    return p ? p->string() : std::string(where);
}

void forget(std::string_view scheme, std::string_view where) {
    std::error_code ec;
    if (const auto p = path_(scheme, where)) fs::remove(*p, ec);
}

std::optional<Received> read(const Handle& h, std::span<char> buf) {
    const int n = ::recv(sock_(h), buf.data(), static_cast<int>(buf.size()), 0);
    if (n == SOCKET_ERROR) {
        if (::WSAGetLastError() == WSAEWOULDBLOCK) return Received{.would_block = true};
        return std::nullopt;
    }
    return Received{.bytes = static_cast<std::size_t>(n)};
}

long write(const Handle& h, std::string_view bytes, std::span<const Handle* const> handles) {
    if (!handles.empty()) return -1;   // no handles travel here
    const int n = ::send(sock_(h), bytes.data(), static_cast<int>(bytes.size()), 0);
    if (n == SOCKET_ERROR) return ::WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
    return n;
}

void wait(std::span<Ready> set, int timeout_ms) {
    std::vector<WSAPOLLFD> p;
    for (const auto& r : set)
        p.push_back({r.handle ? sock_(*r.handle) : INVALID_SOCKET, static_cast<SHORT>(POLLRDNORM | (r.want_write ? POLLWRNORM : 0)), 0});
    if (p.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(timeout_ms, 0)));
        return;
    }
    ::WSAPoll(p.data(), static_cast<ULONG>(p.size()), timeout_ms);
    for (std::size_t k = 0; k < set.size(); ++k) {
        set[k].readable = (p[k].revents & POLLRDNORM) != 0;
        set[k].writable = (p[k].revents & POLLWRNORM) != 0;
        set[k].closed = (p[k].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
}

std::string identity(const Handle&) { return {}; }

std::optional<Shared> create_shared(std::string_view, std::size_t size) {
    HANDLE m = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(static_cast<std::uint64_t>(size) >> 32),
                                    static_cast<DWORD>(size & 0xffffffffu), nullptr);
    if (!m) return std::nullopt;
    return map_shared(Handle(reinterpret_cast<std::intptr_t>(m)), size);
}

std::optional<Shared> map_shared(Handle h, std::size_t size) {
    void* p = ::MapViewOfFile(reinterpret_cast<HANDLE>(h.value()), FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!p) return std::nullopt;
    return Shared{std::move(h), static_cast<std::byte*>(p), size};
}

void unmap(Shared& s) {
    if (s.data) ::UnmapViewOfFile(s.data);
    s.data = nullptr;
    s.handle.reset();
}

Handle duplicate(const Handle& h) {
    HANDLE out = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), reinterpret_cast<HANDLE>(h.value()), ::GetCurrentProcess(), &out, 0, FALSE,
                           DUPLICATE_SAME_ACCESS))
        return Handle();
    return Handle(reinterpret_cast<std::intptr_t>(out));
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string host_name() {
    char buf[256]{};
    DWORD n = sizeof buf;
    return ::GetComputerNameA(buf, &n) ? std::string(buf) : std::string("windows");
}

std::optional<std::string> environment(std::string_view name) {
    const char* v = std::getenv(std::string(name).c_str());
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

bool spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    std::string line;
    for (const auto& a : argv) line += (line.empty() ? "" : " ") + std::string("\"") + a + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

}  // namespace xbus::detail::backend
#endif
