export module xlings.subos.network;

import std;
import xlings.platform;

export namespace xlings::subos::network {

inline constexpr std::uint16_t GATEWAY_PORT { 1080 };
struct Proxy {
    std::string host;
    std::uint16_t port { 0 };
};
// No authentication, BIND, UDP ASSOCIATE or local target-name resolution.
// Credentials in the URL are rejected rather than logged or guessed.
std::expected<Proxy, std::string> parse_proxy(std::string_view url);
struct ResolvedProxy {
    Proxy declared;
    std::vector<platform::network::Address> addresses;
};
std::expected<ResolvedProxy, std::string> resolve_proxy(std::string_view url);

struct Event {
    std::uint64_t connection { 0 };
    std::string event;                       // connect-attempt | proxy-result | denied
    std::string target;
    unsigned port { 0 };
    bool remote_dns { false };               // SOCKS domain passed as-is to the declared proxy
    std::string reason;
};

class Relay {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit Relay(ResolvedProxy proxy);
    Relay(Relay&&) noexcept;
    Relay& operator=(Relay&&) noexcept;
    Relay(const Relay&) = delete;
    ~Relay();
    // Takes ownership of accepted socket descriptors from the private netns.
    bool add(int client);
    std::vector<platform::PollFd> fds() const;
    // Every outbound attempt is audited before connecting; result is audited
    // before acknowledging the SOCKS CONNECT. False terminates that stream.
    void tick(const std::function<bool(const Event&)>& audit);
    void stop();
};

// Runs in a dedicated process inside an already private, loopback-only netns.
// Sends accepted fds over the private control channel; never opens an uplink.
// The channel is created before isolation and never reaches sandbox commands.
int gateway_run(int channel, int listener);

}  // namespace xlings::subos::network
