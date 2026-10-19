module;

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <afunix.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

module xlings.platform;

import :local_socket;

import std;

namespace xlings::platform {

namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr auto INVALID = INVALID_SOCKET;
int socket_error() { return ::WSAGetLastError(); }
std::string socket_error_text(int error) { return "Winsock error " + std::to_string(error); }
bool retryable(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
void close_socket(NativeSocket socket) { ::closesocket(socket); }
bool initialize() {
    struct Winsock {
        bool ready;
        Winsock() { WSADATA data{}; ready = ::WSAStartup(MAKEWORD(2, 2), &data) == 0; }
        ~Winsock() { if (ready) ::WSACleanup(); }
    };
    static Winsock instance;
    return instance.ready;
}
bool configure(NativeSocket socket) {
    u_long mode { 1 };
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0
        && ::SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
}
void remove_file(const std::filesystem::path& path) { ::DeleteFileW(path.c_str()); }
#else
using NativeSocket = int;
constexpr auto INVALID = -1;
int socket_error() { return errno; }
std::string socket_error_text(int error) { return std::strerror(error); }
bool retryable(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
void close_socket(NativeSocket socket) { ::close(socket); }
bool initialize() { return true; }
bool configure(NativeSocket socket) {
    if (::fcntl(socket, F_SETFD, FD_CLOEXEC) < 0) return false;
    const auto flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) return false;
#if defined(SO_NOSIGPIPE)
    const int enabled { 1 };
    if (::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return false;
#endif
    return true;
}
void remove_file(const std::filesystem::path& path) { ::unlink(path.c_str()); }
#endif

using Clock = std::chrono::steady_clock;

int clamp_ms(std::chrono::milliseconds ms) {
    return static_cast<int>(std::clamp<long long>(ms.count(), 0, std::numeric_limits<int>::max()));
}

std::expected<bool, std::string> wait_socket(NativeSocket socket, bool writing, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() < 0) return false;
#if defined(_WIN32)
        WSAPOLLFD fd { socket, static_cast<short>(writing ? POLLWRNORM : POLLRDNORM), 0 };
        const int ready = ::WSAPoll(&fd, 1, clamp_ms(remaining));
#else
        pollfd fd { socket, static_cast<short>(writing ? POLLOUT : POLLIN), 0 };
        const int ready = ::poll(&fd, 1, clamp_ms(remaining));
#endif
        if (ready > 0) return true;
        if (ready == 0) return false;
        const int error = socket_error();
        if (retryable(error)) continue;
        return std::unexpected(socket_error_text(error));
    }
}

std::expected<sockaddr_un, std::string> address_of(const std::filesystem::path& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto text = path.string();
    if (text.size() >= sizeof(address.sun_path))
        return std::unexpected(std::format("{}: longer than a local socket's address ({} bytes)", text,
                                           sizeof(address.sun_path) - 1));
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    return address;
}

std::expected<NativeSocket, std::string> new_socket() {
    if (!initialize()) return std::unexpected("cannot initialize sockets");
    const auto socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket == INVALID) return std::unexpected("no local socket: " + socket_error_text(socket_error()));
    if (!configure(socket)) {
        const auto error = socket_error_text(socket_error());
        close_socket(socket);
        return std::unexpected("cannot configure a local socket: " + error);
    }
    return socket;
}

}  // namespace

std::size_t local_socket_path_max() { return sizeof(sockaddr_un{}.sun_path) - 1; }

LocalStream::LocalStream(std::intptr_t handle) : handle_{handle} {}
LocalStream::LocalStream(LocalStream&& other) noexcept : handle_{std::exchange(other.handle_, -1)} {}
LocalStream& LocalStream::operator=(LocalStream&& other) noexcept {
    if (this != &other) { close(); handle_ = std::exchange(other.handle_, -1); }
    return *this;
}
LocalStream::~LocalStream() { close(); }
void LocalStream::close() noexcept {
    if (handle_ != -1) close_socket(static_cast<NativeSocket>(std::exchange(handle_, -1)));
}

std::expected<LocalStream, std::string> LocalStream::connect(const std::filesystem::path& path,
                                                          std::chrono::milliseconds timeout) {
    const auto address = address_of(path);
    if (!address) return std::unexpected(address.error());
    const auto socket = new_socket();
    if (!socket) return std::unexpected(socket.error());
    LocalStream stream{static_cast<std::intptr_t>(*socket)};
    if (::connect(*socket, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address)) != 0) {
        const auto error = socket_error();
#if defined(_WIN32)
        const bool pending = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
        const bool pending = error == EINPROGRESS || error == EAGAIN;
#endif
        if (!pending) return std::unexpected(std::format("nothing listens at {}: {}", path.string(), socket_error_text(error)));
        const auto ready = wait_socket(*socket, true, Clock::now() + timeout);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::unexpected(std::format("{}: no answer in {} ms", path.string(), timeout.count()));
    }
    return stream;
}

std::expected<std::optional<std::size_t>, std::string> LocalStream::poll_receive(std::span<char> bytes,
                                                                              std::chrono::milliseconds timeout) {
    if (handle_ == -1) return std::unexpected("the stream is closed");
    if (bytes.empty()) return std::optional<std::size_t>{0};
    const auto socket = static_cast<NativeSocket>(handle_);
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        const auto ready = wait_socket(socket, false, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::nullopt;
        const auto length = static_cast<int>(std::min<std::size_t>(bytes.size(), std::numeric_limits<int>::max()));
        const auto read = ::recv(socket, bytes.data(), length, 0);
        if (read >= 0) return static_cast<std::size_t>(read);
        const auto error = socket_error();
        if (!retryable(error)) return std::unexpected(socket_error_text(error));
    }
}

std::expected<void, std::string> LocalStream::send_all(std::string_view bytes, std::chrono::milliseconds timeout) {
    if (handle_ == -1) return std::unexpected("the stream is closed");
    const auto socket = static_cast<NativeSocket>(handle_);
    const auto deadline = Clock::now() + timeout;
    while (!bytes.empty()) {
        const auto ready = wait_socket(socket, true, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::unexpected("the other end did not read in time");
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto length = static_cast<int>(std::min<std::size_t>(bytes.size(), std::numeric_limits<int>::max()));
        const auto sent = ::send(socket, bytes.data(), length, flags);
        if (sent > 0) { bytes.remove_prefix(static_cast<std::size_t>(sent)); continue; }
        const auto error = socket_error();
        if (sent == 0 || !retryable(error)) return std::unexpected(socket_error_text(error));
    }
    return {};
}

LocalListener::LocalListener(std::intptr_t handle, std::filesystem::path path)
    : handle_{handle}, path_{std::move(path)} {}
LocalListener::LocalListener(LocalListener&& other) noexcept
    : handle_{std::exchange(other.handle_, -1)}, path_{std::exchange(other.path_, {})} {}
LocalListener& LocalListener::operator=(LocalListener&& other) noexcept {
    if (this != &other) { close(); handle_ = std::exchange(other.handle_, -1); path_ = std::exchange(other.path_, {}); }
    return *this;
}
LocalListener::~LocalListener() { close(); }
void LocalListener::close() noexcept {
    if (handle_ != -1) {
        close_socket(static_cast<NativeSocket>(std::exchange(handle_, -1)));
        if (!path_.empty()) remove_file(path_);
    }
}

std::expected<LocalListener, std::string> LocalListener::listen(const std::filesystem::path& path) {
    const auto address = address_of(path);
    if (!address) return std::unexpected(address.error());
    const auto socket = new_socket();
    if (!socket) return std::unexpected(socket.error());
    remove_file(path);
#if defined(_WIN32)
    const int bound = ::bind(*socket, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address));
#else
    const auto old = ::umask(0077);
    const int bound = ::bind(*socket, reinterpret_cast<const sockaddr*>(&*address), sizeof(*address));
    ::umask(old);
#endif
    if (bound != 0 || ::listen(*socket, 16) != 0) {
        const auto error = socket_error_text(socket_error());
        close_socket(*socket);
        return std::unexpected(std::format("cannot listen at {}: {}", path.string(), error));
    }
    return LocalListener{static_cast<std::intptr_t>(*socket), path};
}

std::expected<std::optional<LocalStream>, std::string> LocalListener::accept(std::chrono::milliseconds timeout) {
    if (handle_ == -1) return std::unexpected("the listener is closed");
    const auto socket = static_cast<NativeSocket>(handle_);
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        const auto ready = wait_socket(socket, false, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::nullopt;
        const auto client = ::accept(socket, nullptr, nullptr);
        if (client != INVALID) {
            LocalStream stream{static_cast<std::intptr_t>(client)};
            if (!configure(client)) return std::unexpected("cannot configure an accepted local stream");
            return std::optional<LocalStream>{std::move(stream)};
        }
        const auto error = socket_error();
        if (!retryable(error)) return std::unexpected(socket_error_text(error));
    }
}

std::vector<std::size_t> wait_readable(std::span<const std::intptr_t> handles, std::chrono::milliseconds timeout) {
    std::vector<std::size_t> ready;
    if (handles.empty()) {
        std::this_thread::sleep_for(timeout);
        return ready;
    }
#if defined(_WIN32)
    std::vector<WSAPOLLFD> fds;
    for (auto h : handles) fds.push_back({static_cast<SOCKET>(h), POLLRDNORM, 0});
    int n;
    do { n = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), clamp_ms(timeout)); }
    while (n < 0 && retryable(socket_error()));
#else
    std::vector<pollfd> fds;
    for (auto h : handles) fds.push_back({static_cast<int>(h), POLLIN, 0});
    int n;
    do { n = ::poll(fds.data(), fds.size(), clamp_ms(timeout)); } while (n < 0 && errno == EINTR);
#endif
    for (std::size_t i = 0; n > 0 && i < fds.size(); ++i)
        if (fds[i].revents != 0) ready.push_back(i);
    return ready;
}

}  // namespace xlings::platform
