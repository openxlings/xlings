module;

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

module xlings.platform;

import :network;

import std;

namespace xlings::platform::network {
#if defined(__linux__)
namespace {
std::string error_text_() { return std::strerror(errno); }
bool retry_() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
}
std::expected<std::vector<Address>, std::string> resolve_proxy(std::string_view host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result { nullptr };
    const auto status = ::getaddrinfo(std::string(host).c_str(), std::to_string(port).c_str(), &hints, &result);
    if (status != 0) return std::unexpected("cannot resolve declared proxy: " + std::string(::gai_strerror(status)));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owner{result, &::freeaddrinfo};
    std::vector<Address> addresses;
    for (auto* item = result; item; item = item->ai_next) {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) continue;
        Address address{.family = item->ai_family};
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(item->ai_addr);
        address.bytes.assign(bytes, bytes + item->ai_addrlen);
        std::array<char, NI_MAXHOST> numeric{};
        if (::getnameinfo(item->ai_addr, item->ai_addrlen, numeric.data(), numeric.size(), nullptr, 0,
                          NI_NUMERICHOST) == 0) address.numeric = numeric.data();
        addresses.push_back(std::move(address));
    }
    if (addresses.empty()) return std::unexpected("declared proxy has no IPv4 or IPv6 address");
    return addresses;
}
std::expected<void, std::string> enable_loopback() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::unexpected(error_text_());
    ifreq request{};
    std::memcpy(request.ifr_name, "lo", 3);
    bool ok = ::ioctl(fd, SIOCGIFFLAGS, &request) == 0;
    request.ifr_flags |= IFF_UP;
    if (ok) ok = ::ioctl(fd, SIOCSIFFLAGS, &request) == 0;
    const auto error = ok ? std::string{} : error_text_();
    ::close(fd);
    if (!ok) return std::unexpected("cannot enable private loopback: " + error);
    return {};
}
std::expected<int, std::string> listen_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) return std::unexpected(error_text_());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd, 64) != 0) {
        const auto error = error_text_();
        ::close(fd);
        return std::unexpected("cannot listen on private loopback: " + error);
    }
    return fd;
}
std::expected<std::uint16_t, std::string> bound_port(int listener) {
    sockaddr_in address{};
    socklen_t size = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        return std::unexpected(error_text_());
    return ntohs(address.sin_port);
}
std::expected<std::optional<int>, std::string> accept(int listener) {
    const int fd = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) return std::optional<int>{fd};
    if (retry_()) return std::nullopt;
    return std::unexpected(error_text_());
}
std::expected<Connecting, std::string> connect_to(const Address& proxy) {
    if ((proxy.family != AF_INET && proxy.family != AF_INET6)
        || proxy.bytes.size() != (proxy.family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6)))
        return std::unexpected("invalid resolved proxy endpoint");
    const int fd = ::socket(proxy.family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) return std::unexpected(error_text_());
    if (::connect(fd, reinterpret_cast<const sockaddr*>(proxy.bytes.data()), proxy.bytes.size()) == 0)
        return Connecting{fd, true};
    if (errno == EINPROGRESS) return Connecting{fd, false};
    const auto error = error_text_();
    ::close(fd);
    return std::unexpected(error);
}
std::expected<std::optional<bool>, std::string> connected(int fd) {
    pollfd ready{fd, POLLOUT, 0};
    const int status = ::poll(&ready, 1, 0);
    if (status == 0 || (status < 0 && retry_())) return std::nullopt;
    if (status < 0) return std::unexpected(error_text_());
    int failure { 0 };
    socklen_t size = sizeof(failure);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size) != 0) return std::unexpected(error_text_());
    if (failure != 0) return std::unexpected(std::string(::strerror(failure)));
    return std::optional<bool>{true};
}
std::expected<std::optional<std::size_t>, std::string> receive(int fd, std::span<char> buffer) {
    const auto count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (count >= 0) return std::optional<std::size_t>{static_cast<std::size_t>(count)};
    if (retry_()) return std::nullopt;
    return std::unexpected(error_text_());
}
std::expected<std::optional<std::size_t>, std::string> send(int fd, std::string_view bytes) {
    const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count >= 0) return std::optional<std::size_t>{static_cast<std::size_t>(count)};
    if (retry_()) return std::nullopt;
    return std::unexpected(error_text_());
}
bool shutdown_write(int fd) { return ::shutdown(fd, SHUT_WR) == 0; }
#else
std::expected<std::vector<Address>, std::string> resolve_proxy(std::string_view, std::uint16_t) { return std::unexpected("proxy network sessions need Linux"); }
std::expected<void, std::string> enable_loopback() { return std::unexpected("proxy network sessions need Linux"); }
std::expected<int, std::string> listen_loopback(std::uint16_t) { return std::unexpected("proxy network sessions need Linux"); }
std::expected<std::uint16_t, std::string> bound_port(int) { return std::unexpected("proxy network sessions need Linux"); }
std::expected<std::optional<int>, std::string> accept(int) { return std::unexpected("proxy network sessions need Linux"); }
std::expected<Connecting, std::string> connect_to(const Address&) { return std::unexpected("proxy network sessions need Linux"); }
std::expected<std::optional<bool>, std::string> connected(int) { return std::unexpected("proxy network sessions need Linux"); }
std::expected<std::optional<std::size_t>, std::string> receive(int, std::span<char>) { return std::unexpected("proxy network sessions need Linux"); }
std::expected<std::optional<std::size_t>, std::string> send(int, std::string_view) { return std::unexpected("proxy network sessions need Linux"); }
bool shutdown_write(int) { return false; }
#endif
}  // namespace xlings::platform::network
