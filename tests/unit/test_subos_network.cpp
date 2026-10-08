#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.platform;
import xlings.subos.network;
import xlings.subos.policy;
import xlings.subos.spec;
import xlings.subos.caps;
import xlings.subos.provider;

namespace network = xlings::subos::network;
namespace platform = xlings::platform;
namespace tk = xlings::testkit;
namespace policy = xlings::subos::policy;
namespace spec = xlings::subos::spec;

XTEST(SubosNetwork, ProxyUrlsRejectCredentialsAndNonRemoteDnsSchemes,
      .area = "subos") {
    for (const auto* url : {"http://proxy:80", "socks5://proxy:1080", "socks5h://user:secret@proxy:1080",
                            "socks5h://proxy:0", "socks5h://proxy:65536", "socks5h://proxy:80/path"})
        EXPECT_FALSE(network::parse_proxy(url)) << url;
    const auto ipv6 = network::parse_proxy("socks5h://[::1]:1080");
    ASSERT_TRUE(ipv6);
    EXPECT_EQ(ipv6->host, "::1");
    EXPECT_EQ(ipv6->port, 1080);
    auto pinned = policy::preset(policy::Preset::Dev);
    pinned.net = policy::Net::Proxy;
    pinned.proxy = "socks5h://declared:1080";
    EXPECT_FALSE(policy::apply(pinned, {.proxy = "socks5h://other:1080"}));
    EXPECT_TRUE(policy::apply(pinned, {.proxy = pinned.proxy}));
}

XTEST(SubosNetwork, CompilerAndProviderKeepTheAlreadyCreatedProxyNamespace,
      .area = "subos") {
    auto requested = policy::preset(policy::Preset::Dev);
    requested.net = policy::Net::Proxy;
    requested.proxy = "socks5h://proxy.example:1080";
    xlings::subos::caps::Caps caps;
    caps.platform = "linux";
    caps.bwrap = xlings::subos::caps::Backend{.name = "bwrap", .bin = "/bwrap", .usable = true};
    caps.userns = true;
    const auto compiled = spec::compile(requested, {.home = "/h"}, caps,
        {.instance = "box", .instance_dir = "/h/subos/box", .argv = {"true"},
         .host_exists = [](std::string_view) { return true; }});
    ASSERT_TRUE(compiled);
    EXPECT_TRUE(compiled->unshare_net);
    EXPECT_TRUE(compiled->net_proxy);
    EXPECT_FALSE(compiled->net_nat);
    EXPECT_EQ(compiled->env.at("ALL_PROXY"), "socks5h://127.0.0.1:1080");
    const auto argv = xlings::subos::provider::bwrap_argv(*compiled);
    EXPECT_EQ(std::ranges::find(argv, "--unshare-net"), argv.end());
    EXPECT_EQ(compiled->describe()["net"]["dns"], "remote");
    for (const auto* other : {"macos", "windows"}) {
        caps.platform = other;
        requested.needs["net"] = policy::Need::Must;
        EXPECT_FALSE(spec::compile(requested, {.home = "/h"}, caps,
            {.instance = "box", .instance_dir = "/h/subos/box", .host_exists = [](std::string_view) { return true; }}));
    }
}

XTEST(SubosNetwork, AuditRefusalPrecedesAnyDialOfTheDeclaredProxy,
      .area = "subos", .requires_ = {"linux"}) {
    auto upstream = platform::LoopbackListener::open();
    ASSERT_TRUE(upstream) << upstream.error();
    auto resolved = network::resolve_proxy("socks5h://127.0.0.1:" + std::to_string(upstream->port()));
    ASSERT_TRUE(resolved) << resolved.error();
    network::Relay relay{std::move(*resolved)};
    const auto listening = platform::network::listen_loopback(0);
    ASSERT_TRUE(listening) << listening.error();
    struct Owned { int fd; ~Owned() { platform::close_fd(fd); } } listener{*listening};
    const auto port = platform::network::bound_port(listener.fd);
    ASSERT_TRUE(port);
    std::atomic<bool> done { false };
    std::string clientError;
    std::jthread client([&] {
        auto socket = platform::TcpStream::connect_loopback(*port);
        if (!socket) { clientError = socket.error(); done = true; return; }
        const std::string greeting{'\5', '\1', '\0'};
        if (!socket->send_all(greeting, std::chrono::seconds(2))) { clientError = "send greeting"; done = true; return; }
        std::array<char, 2> method{};
        auto reply = socket->receive(method, std::chrono::seconds(2));
        if (!reply || *reply != 2 || method[1] != 0) { clientError = "invalid method reply"; done = true; return; }
        const std::string domain { "must-never-resolve.invalid" };
        std::string request{'\5', '\1', '\0', '\3', static_cast<char>(domain.size())};
        request += domain;
        request += std::string{'\0', static_cast<char>(80)};
        if (!socket->send_all(request, std::chrono::seconds(2))) clientError = "send request";
        std::array<char, 16> result{};
        const auto response = socket->receive(result, std::chrono::seconds(2));
        if (!response || *response != 0) clientError = "audit refusal must close without CONNECT acknowledgement";
        done = true;
    });
    std::vector<network::Event> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        auto accepted = platform::network::accept(listener.fd);
        ASSERT_TRUE(accepted);
        if (accepted->has_value()) ASSERT_TRUE(relay.add(**accepted));
        relay.tick([&](const auto& event) { events.push_back(event); return false; });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.join();
    EXPECT_TRUE(clientError.empty()) << clientError;
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].event, "connect-attempt");
    EXPECT_EQ(events[0].target, "must-never-resolve.invalid");
    EXPECT_TRUE(events[0].remote_dns);
    const auto outbound = upstream->accept(std::chrono::milliseconds(20));
    ASSERT_TRUE(outbound);
    EXPECT_FALSE(outbound->has_value()) << "audit failure must precede the host proxy connect";
}
