module xlings.subos.network;

import std;
import xlings.platform;

namespace xlings::subos::network {

namespace net = platform::network;
using Clock = std::chrono::steady_clock;

std::expected<Proxy, std::string> parse_proxy(std::string_view url) {
    constexpr std::string_view PREFIX { "socks5h://" };
    if (!url.starts_with(PREFIX)) return std::unexpected("net=proxy requires socks5h://HOST:PORT");
    auto authority = url.substr(PREFIX.size());
    if (authority.ends_with('/')) authority.remove_suffix(1);
    if (authority.empty() || authority.find_first_of("/@?#\\") != authority.npos
        || std::ranges::any_of(authority, [](unsigned char c) { return c <= 32 || c >= 127; }))
        return std::unexpected("proxy URL needs a host and port; credentials and paths are unsupported");
    std::string_view host, port;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == authority.npos || end + 1 >= authority.size() || authority[end + 1] != ':')
            return std::unexpected("IPv6 proxy needs [ADDRESS]:PORT");
        host = authority.substr(1, end - 1);
        port = authority.substr(end + 2);
    } else {
        const auto colon = authority.find(':');
        if (colon == authority.npos || colon != authority.rfind(':'))
            return std::unexpected("proxy needs HOST:PORT (bracket IPv6 addresses)");
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    unsigned value { 0 };
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
    if (host.empty() || error != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535)
        return std::unexpected("proxy needs a nonempty host and port in 1..65535");
    return Proxy{std::string(host), static_cast<std::uint16_t>(value)};
}

std::expected<ResolvedProxy, std::string> resolve_proxy(std::string_view url) {
    const auto parsed = parse_proxy(url);
    if (!parsed) return std::unexpected(parsed.error());
    const auto addresses = net::resolve_proxy(parsed->host, parsed->port);
    if (!addresses) return std::unexpected(addresses.error());
    return ResolvedProxy{*parsed, *addresses};
}

namespace {

constexpr std::size_t MAX_BUFFER { 1024 * 1024 };
constexpr std::size_t MAX_CLIENTS { 128 };
unsigned byte(std::string_view data, std::size_t index) { return static_cast<unsigned char>(data[index]); }
std::string failure(unsigned code) { return std::string{'\5', static_cast<char>(code), '\0', '\1', '\0', '\0', '\0', '\0', '\0', '\0'}; }

struct Target { std::string host; unsigned port { 0 }; bool domain { false }; std::size_t size { 0 }; };
// Wire addresses are never resolved here. Domain bytes are forwarded to SOCKS5h.
std::expected<std::optional<Target>, std::string> target(std::string_view input) {
    if (input.size() < 4) return std::nullopt;
    if (byte(input, 0) != 5 || byte(input, 1) != 1 || byte(input, 2) != 0)
        return std::unexpected("only SOCKS5 CONNECT is permitted");
    Target result;
    const auto type = byte(input, 3);
    std::size_t offset { 4 }, length { 0 };
    if (type == 1) length = 4;
    else if (type == 4) length = 16;
    else if (type == 3) {
        if (input.size() < 5) return std::nullopt;
        length = byte(input, 4);
        offset = 5;
        result.domain = true;
        if (length == 0) return std::unexpected("SOCKS target domain is empty");
    } else return std::unexpected("unsupported SOCKS address type");
    if (input.size() < offset + length + 2) return std::nullopt;
    if (type == 3) {
        result.host = input.substr(offset, length);
        if (std::ranges::any_of(result.host, [](unsigned char c) { return c <= 32 || c >= 127; }))
            return std::unexpected("SOCKS target domain contains invalid bytes");
    } else if (type == 1) {
        result.host = std::format("{}.{}.{}.{}", byte(input, 4), byte(input, 5), byte(input, 6), byte(input, 7));
    } else {
        for (std::size_t index = 4; index < 20; index += 2) {
            if (!result.host.empty()) result.host += ':';
            result.host += std::format("{:x}", byte(input, index) * 256 + byte(input, index + 1));
        }
    }
    result.port = byte(input, offset + length) * 256 + byte(input, offset + length + 1);
    if (result.port == 0) return std::unexpected("SOCKS target port must be positive");
    result.size = offset + length + 2;
    return std::optional<Target>{std::move(result)};
}

bool flush(int fd, std::string& output) {
    while (!output.empty()) {
        const auto sent = net::send(fd, output);
        if (!sent || (sent->has_value() && **sent == 0)) return false;
        if (!sent->has_value()) return true;
        output.erase(0, **sent);
    }
    return true;
}
bool read_into(int fd, std::string& input, bool& ended) {
    if (ended) return true;
    std::array<char, 16384> bytes{};
    while (input.size() < MAX_BUFFER) {
        const auto size = std::min(bytes.size(), MAX_BUFFER - input.size());
        const auto read = net::receive(fd, std::span(bytes.data(), size));
        if (!read) return false;
        if (!read->has_value()) return true;
        if (**read == 0) { ended = true; return true; }
        input.append(bytes.data(), **read);
    }
    return true;
}

}  // namespace

struct Relay::Impl {
    enum class Stage { Greeting, Request, Connecting, ProxyMethod, ProxyReply, Forward, Closing };
    struct Peer {
        std::uint64_t id { 0 };
        int client { -1 }, remote { -1 };
        Stage stage { Stage::Greeting };
        Target requested;
        std::string request, clientInput, clientOutput, remoteInput, remoteOutput;
        bool clientEnded { false }, remoteEnded { false };
        bool clientShutdown { false }, remoteShutdown { false };
        std::size_t candidate { 0 };
        Clock::time_point deadline { Clock::now() + std::chrono::seconds(10) };
        ~Peer() { platform::close_fd(client); platform::close_fd(remote); }
    };
    ResolvedProxy proxy;
    std::vector<std::unique_ptr<Peer>> peers;
    std::uint64_t next { 1 };

    explicit Impl(ResolvedProxy p) : proxy{std::move(p)} {}
    Event event(const Peer& p, std::string name, std::string reason = {}) const {
        return {p.id, std::move(name), p.requested.host, p.requested.port, p.requested.domain, std::move(reason)};
    }
    void fail(Peer& p, const std::function<bool(const Event&)>& audit, unsigned code, std::string reason) {
        const bool recorded = audit(event(p, "denied", std::move(reason)));
        if (recorded) p.clientOutput += failure(code);
        else p.clientOutput.clear();
        p.stage = Stage::Closing;
        p.deadline = Clock::now() + std::chrono::seconds(2);
        platform::close_fd(p.remote);
        p.remote = -1;
    }
    bool dial(Peer& p) {
        while (p.candidate < proxy.addresses.size()) {
            const auto connection = net::connect_to(proxy.addresses[p.candidate++]);
            if (!connection) continue;
            p.remote = connection->fd;
            p.stage = connection->ready ? Stage::ProxyMethod : Stage::Connecting;
            p.remoteOutput = std::string{'\5', '\1', '\0'};
            return true;
        }
        return false;
    }
    bool step(Peer& p, const std::function<bool(const Event&)>& audit) {
        if (p.stage == Stage::Closing)
            return Clock::now() < p.deadline && flush(p.client, p.clientOutput) && !p.clientOutput.empty();
        if (p.stage != Stage::Forward && Clock::now() >= p.deadline) {
            fail(p, audit, 6, "proxy handshake timed out");
            return true;
        }
        if (!flush(p.client, p.clientOutput)) return false;
        if (p.stage == Stage::Greeting || p.stage == Stage::Request) {
            if (!read_into(p.client, p.clientInput, p.clientEnded) || p.clientEnded) return false;
        }
        if (p.stage == Stage::Greeting) {
            if (p.clientInput.size() < 2) return true;
            const auto methods = byte(p.clientInput, 1);
            if (p.clientInput.size() < methods + 2) return true;
            if (byte(p.clientInput, 0) != 5 || methods == 0
                || p.clientInput.substr(2, methods).find('\0') == std::string::npos) {
                (void)audit(event(p, "denied", "SOCKS5 no-auth method required"));
                p.clientOutput = std::string{'\5', static_cast<char>(255)};
                p.stage = Stage::Closing;
                return true;
            }
            p.clientInput.erase(0, methods + 2);
            p.clientOutput = std::string{'\5', '\0'};
            p.stage = Stage::Request;
        }
        if (p.stage == Stage::Request) {
            const auto parsed = target(p.clientInput);
            if (!parsed) { fail(p, audit, 7, parsed.error()); return true; }
            if (!parsed->has_value()) return true;
            p.requested = **parsed;
            p.request = p.clientInput.substr(0, p.requested.size);
            p.clientInput.erase(0, p.requested.size);
            if (!audit(event(p, "connect-attempt"))) { p.clientOutput.clear(); return false; }
            if (!dial(p)) { fail(p, audit, 5, "declared proxy is unreachable"); return true; }
        }
        if (p.stage == Stage::Connecting) {
            const auto ready = net::connected(p.remote);
            if (!ready) {
                platform::close_fd(p.remote);
                p.remote = -1;
                if (!dial(p)) { fail(p, audit, 5, ready.error()); return true; }
            } else if (ready->has_value()) p.stage = Stage::ProxyMethod;
            if (p.stage == Stage::Connecting) return true;
        }
        if (p.stage == Stage::ProxyMethod || p.stage == Stage::ProxyReply) {
            if (!flush(p.remote, p.remoteOutput) || !read_into(p.remote, p.remoteInput, p.remoteEnded)
                || p.remoteEnded) { fail(p, audit, 5, "declared proxy closed its handshake"); return true; }
        }
        if (p.stage == Stage::ProxyMethod) {
            if (p.remoteInput.size() < 2) return true;
            if (byte(p.remoteInput, 0) != 5 || byte(p.remoteInput, 1) != 0) {
                fail(p, audit, 2, "declared proxy does not accept no-auth SOCKS5"); return true;
            }
            p.remoteInput.erase(0, 2);
            p.remoteOutput = p.request;
            p.stage = Stage::ProxyReply;
            return true;
        }
        if (p.stage == Stage::ProxyReply) {
            if (p.remoteInput.size() < 4) return true;
            const auto status = byte(p.remoteInput, 1);
            auto normalized = p.remoteInput;
            normalized[1] = '\1';
            // The reply has the request's address framing; port zero is valid
            // for BND.PORT. Validate framing independently of a target port.
            std::size_t size { 0 };
            const auto type = byte(normalized, 3);
            if (type == 1) size = 10;
            else if (type == 4) size = 22;
            else if (type == 3) {
                if (normalized.size() < 5) return true;
                size = 7 + byte(normalized, 4);
            } else { fail(p, audit, 1, "declared proxy sent an invalid address type"); return true; }
            if (normalized.size() < size) return true;
            if (byte(normalized, 0) != 5 || byte(normalized, 2) != 0) {
                fail(p, audit, 1, "declared proxy sent a malformed reply"); return true;
            }
            if (!audit(event(p, "proxy-result", status == 0 ? "accepted" : "refused:" + std::to_string(status))))
                return false;
            p.clientOutput += p.remoteInput.substr(0, size);
            p.remoteInput.erase(0, size);
            if (status != 0) { p.stage = Stage::Closing; return true; }
            p.stage = Stage::Forward;
        }
        if (p.stage == Stage::Forward) {
            p.remoteOutput += std::exchange(p.clientInput, {});
            p.clientOutput += std::exchange(p.remoteInput, {});
            if (!flush(p.remote, p.remoteOutput) || !flush(p.client, p.clientOutput)) return false;
            if (!read_into(p.client, p.remoteOutput, p.clientEnded)
                || !read_into(p.remote, p.clientOutput, p.remoteEnded)) return false;
            if (p.clientEnded && p.remoteOutput.empty() && !p.remoteShutdown) {
                (void)net::shutdown_write(p.remote);
                p.remoteShutdown = true;
            }
            if (p.remoteEnded && p.clientOutput.empty() && !p.clientShutdown) {
                (void)net::shutdown_write(p.client);
                p.clientShutdown = true;
            }
            return !(p.clientEnded && p.remoteEnded && p.clientOutput.empty() && p.remoteOutput.empty());
        }
        return true;
    }
};

Relay::Relay(ResolvedProxy proxy) : impl_{std::make_unique<Impl>(std::move(proxy))} {}
Relay::Relay(Relay&&) noexcept = default;
Relay& Relay::operator=(Relay&&) noexcept = default;
Relay::~Relay() = default;
bool Relay::add(int client) {
    if (!impl_ || impl_->peers.size() >= MAX_CLIENTS) { platform::close_fd(client); return false; }
    auto peer = std::make_unique<Impl::Peer>();
    peer->id = impl_->next++;
    peer->client = client;
    impl_->peers.push_back(std::move(peer));
    return true;
}
std::vector<platform::PollFd> Relay::fds() const {
    std::vector<platform::PollFd> fds;
    if (!impl_) return fds;
    for (const auto& peer : impl_->peers) {
        if (!peer->clientEnded) fds.push_back({peer->client});
        if (peer->remote >= 0 && !peer->remoteEnded) fds.push_back({peer->remote});
    }
    return fds;
}
void Relay::tick(const std::function<bool(const Event&)>& audit) {
    if (!impl_) return;
    std::erase_if(impl_->peers, [&](const auto& peer) { return !impl_->step(*peer, audit); });
}
void Relay::stop() { impl_.reset(); }

int gateway_run(int channel, int listener) {
    for (;;) {
        std::array<platform::PollFd, 2> watched{{{channel}, {listener}}};
        if (platform::poll_fds(watched, 1000) < 0) return 125;
        if (watched[0].closed || watched[0].readable) {
            // The supervisor has no reason to send data. Readiness is its
            // shutdown/EOF, so the gate never outlives its owner.
            return 0;
        }
        if (!watched[1].readable) continue;
        for (;;) {
            const auto client = net::accept(listener);
            if (!client) return 125;
            if (!client->has_value()) break;
            const int fd = **client;
            const bool sent = platform::send_message(channel, "proxy-client", std::span(&fd, 1));
            platform::close_fd(fd);
            if (!sent) return 0;
        }
    }
}

}  // namespace xlings::subos::network
