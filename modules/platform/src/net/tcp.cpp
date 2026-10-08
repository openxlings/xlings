module;

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

module xlings.platform;

import :tcp;

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
#endif

using Clock = std::chrono::steady_clock;

std::expected<bool, std::string> wait_socket(NativeSocket socket, bool writing, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() < 0) return false;
        const auto timeout = static_cast<int>(std::min<long long>(remaining.count(), std::numeric_limits<int>::max()));
#if defined(_WIN32)
        WSAPOLLFD fd { socket, static_cast<short>(writing ? POLLWRNORM : POLLRDNORM), 0 };
        const int ready = ::WSAPoll(&fd, 1, timeout);
#else
        pollfd fd { socket, static_cast<short>(writing ? POLLOUT : POLLIN), 0 };
        const int ready = ::poll(&fd, 1, timeout);
#endif
        if (ready > 0) return true; // recv/accept/SO_ERROR attributes hang-up and error.
        if (ready == 0) return false;
        const int error = socket_error();
        if (retryable(error)) continue;
        return std::unexpected(socket_error_text(error));
    }
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

std::expected<NativeSocket, std::string> new_socket() {
    if (!initialize()) return std::unexpected("cannot initialize loopback sockets");
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID) return std::unexpected(socket_error_text(socket_error()));
    if (!configure(socket)) {
        const auto error = socket_error_text(socket_error());
        close_socket(socket);
        return std::unexpected("cannot configure loopback socket: " + error);
    }
    return socket;
}

}  // namespace

TcpStream::TcpStream(std::intptr_t handle) : handle_{handle} {}
TcpStream::TcpStream(TcpStream&& other) noexcept : handle_{std::exchange(other.handle_, -1)} {}
TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) { close(); handle_ = std::exchange(other.handle_, -1); }
    return *this;
}
TcpStream::~TcpStream() { close(); }
void TcpStream::close() noexcept {
    if (handle_ != -1) close_socket(static_cast<NativeSocket>(std::exchange(handle_, -1)));
}

std::expected<TcpStream, std::string> TcpStream::connect_loopback(std::uint16_t port,
                                                               std::chrono::milliseconds timeout) {
    const auto socket = new_socket();
    if (!socket) return std::unexpected(socket.error());
    TcpStream stream{static_cast<std::intptr_t>(*socket)};
    const auto address = loopback_address(port);
    if (::connect(*socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const auto error = socket_error();
#if defined(_WIN32)
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
#else
        if (error != EINPROGRESS && !retryable(error))
#endif
            return std::unexpected("loopback connect failed: " + socket_error_text(error));
        const auto ready = wait_socket(*socket, true, Clock::now() + timeout);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::unexpected("loopback connect timed out");
        int failure { 0 };
#if defined(_WIN32)
        int length = sizeof(failure);
        const int checked = ::getsockopt(*socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&failure), &length);
#else
        socklen_t length = sizeof(failure);
        const int checked = ::getsockopt(*socket, SOL_SOCKET, SO_ERROR, &failure, &length);
#endif
        if (checked != 0 || failure != 0)
            return std::unexpected("loopback connect failed: " + socket_error_text(failure ? failure : socket_error()));
    }
    return stream;
}

std::expected<std::size_t, std::string> TcpStream::receive(std::span<char> bytes,
    std::chrono::milliseconds timeout) {
    auto read = poll_receive(bytes, timeout);
    if (!read) return std::unexpected(read.error());
    if (!*read) return std::unexpected("loopback receive timed out");
    return **read;
}

std::expected<std::optional<std::size_t>, std::string> TcpStream::poll_receive(std::span<char> bytes,
                                                        std::chrono::milliseconds timeout) {
    if (handle_ == -1) return std::unexpected("socket is closed");
    if (bytes.empty()) return 0;
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

std::expected<void, std::string> TcpStream::send_all(std::string_view bytes,
                                                  std::chrono::milliseconds timeout) {
    if (handle_ == -1) return std::unexpected("socket is closed");
    const auto socket = static_cast<NativeSocket>(handle_);
    const auto deadline = Clock::now() + timeout;
    while (!bytes.empty()) {
        const auto ready = wait_socket(socket, true, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::unexpected("loopback send timed out");
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

LoopbackListener::LoopbackListener(std::intptr_t handle, std::uint16_t port) : handle_{handle}, port_{port} {}
LoopbackListener::LoopbackListener(LoopbackListener&& other) noexcept
    : handle_{std::exchange(other.handle_, -1)}, port_{std::exchange(other.port_, 0)} {}
LoopbackListener& LoopbackListener::operator=(LoopbackListener&& other) noexcept {
    if (this != &other) { close(); handle_ = std::exchange(other.handle_, -1); port_ = std::exchange(other.port_, 0); }
    return *this;
}
LoopbackListener::~LoopbackListener() { close(); }
void LoopbackListener::close() noexcept {
    if (handle_ != -1) close_socket(static_cast<NativeSocket>(std::exchange(handle_, -1)));
    port_ = 0;
}
std::uint16_t LoopbackListener::port() const { return port_; }

std::expected<LoopbackListener, std::string> LoopbackListener::open(std::uint16_t port) {
    const auto socket = new_socket();
    if (!socket) return std::unexpected(socket.error());
    LoopbackListener listener{static_cast<std::intptr_t>(*socket), port};
    auto address = loopback_address(port);
#if defined(_WIN32)
    const BOOL exclusive { TRUE };
    if (::setsockopt(*socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
        return std::unexpected("cannot reserve exclusive loopback address");
#endif
    if (::bind(*socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || ::listen(*socket, 32) != 0)
        return std::unexpected("cannot listen on loopback: " + socket_error_text(socket_error()));
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (::getsockname(*socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return std::unexpected(socket_error_text(socket_error()));
    listener.port_ = ntohs(address.sin_port);
    return listener;
}

std::expected<std::optional<TcpStream>, std::string>
LoopbackListener::accept(std::chrono::milliseconds timeout) {
    if (handle_ == -1) return std::unexpected("listener is closed");
    const auto socket = static_cast<NativeSocket>(handle_);
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        const auto ready = wait_socket(socket, false, deadline);
        if (!ready) return std::unexpected(ready.error());
        if (!*ready) return std::nullopt;
        const auto client = ::accept(socket, nullptr, nullptr);
        if (client != INVALID) {
            TcpStream stream{static_cast<std::intptr_t>(client)};
            if (!configure(client)) return std::unexpected("cannot configure accepted loopback stream");
            return std::optional<TcpStream>{std::move(stream)};
        }
        const auto error = socket_error();
        if (!retryable(error)) return std::unexpected(socket_error_text(error));
    }
}

}  // namespace xlings::platform
