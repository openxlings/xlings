export module xlings.platform:network;

import std;

export namespace xlings::platform::network {

// Linux session sockets. Unlike :tcp's fixture API, only the supervisor may
// use connect_to(): it takes the already resolved, declared proxy endpoint.
struct Address {
    int family { 0 };
    std::vector<std::uint8_t> bytes;
    std::string numeric;
};
std::expected<std::vector<Address>, std::string> resolve_proxy(std::string_view host, std::uint16_t port);
std::expected<void, std::string> enable_loopback();
std::expected<int, std::string> listen_loopback(std::uint16_t port);
std::expected<std::uint16_t, std::string> bound_port(int listener);
std::expected<std::optional<int>, std::string> accept(int listener);
struct Connecting { int fd { -1 }; bool ready { false }; };
std::expected<Connecting, std::string> connect_to(const Address& proxy);
// nullopt until writable; false/error is never successful establishment.
std::expected<std::optional<bool>, std::string> connected(int fd);
// nullopt is EAGAIN; zero is EOF. Operations never block.
std::expected<std::optional<std::size_t>, std::string> receive(int fd, std::span<char> buffer);
std::expected<std::optional<std::size_t>, std::string> send(int fd, std::string_view bytes);
bool shutdown_write(int fd);

}  // namespace xlings::platform::network
