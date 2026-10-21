// A system's bus under the host's (design 2026-10-22 §6.4, §6.5): the
// agent's router linked through the protocol's L frames -- here the agent on
// a relay's standard streams (luban __agent-stdio), this test as its
// supervisor -- and the host's router, one per home, started by the first
// that needs it.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.platform;
import luban.vm;
import xbus.router;
import xbus.node;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

std::optional<fs::path> luban_binary() {
    if (const char* forced = std::getenv("LUBAN_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& candidate : {xlings.parent_path() / "luban", xlings.parent_path() / "luban-tool" / "luban"})
        if (fs::is_regular_file(candidate)) return candidate;
    return std::nullopt;
}

struct RunningRouter {
    xbus::Router router;
    std::atomic<bool> stop { false };
    std::jthread thread;
    explicit RunningRouter(xbus::RouterOptions o) : router(std::move(o)) {
        if (auto l = router.listen(); !l) throw std::runtime_error(l.error());
        thread = std::jthread([this] { router.run(stop); });
    }
    ~RunningRouter() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
};

}  // namespace

XTEST(LubanBus, TheSystemsBusIsLinkedUnderTheHostsThroughItsAgent, .area = "luban", .covers = {"LUBAN-BUS-LINK"},
      .requires_ = {"linux", "xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = tk::Home::isolated("luban-bus-link");
    const xbus::Address at{"local", (home.root() / "host-bus.sock").string()};
    RunningRouter host({.root = "host/test", .listen = {at}});
    auto client = luban::vm::Client::through({bin->string(), "__agent-stdio"});
    ASSERT_TRUE(client) << client.error();
    // This test is the supervisor: the host's half of the link is a pipe it moves.
    std::shared_ptr<xbus::PipeEnd> end;
    int links = 0;
    std::atomic<bool> pumping{true};
    std::mutex m;
    auto& c = *client;
    const auto pump = [&](std::chrono::milliseconds for_) {
        const auto until = std::chrono::steady_clock::now() + for_;
        while (std::chrono::steady_clock::now() < until) {
            auto frames = c.poll(50);
            if (!frames) return false;
            for (const auto& [type, body] : *frames) {
                if (type == 'H')
                    (void)c.send_frame('A', 0, Json{{"provides", Json{{"openbus", Json{{"root", "host/test/system/m"}}}}}}.dump());
                if (type != 'L') continue;
                std::lock_guard l(m);
                if (body.second.empty()) { if (end) end->closed(); end.reset(); continue; }
                if (!end) {
                    end = host.router.adopt({.send = [&c](std::string_view b) { return c.send_frame('L', 0, b); }});
                    ++links;
                }
                end->feed(body.second);
            }
        }
        return true;
    };
    auto n = xbus::Node::connect({.name = "test", .router = at, .telemetry = false});
    ASSERT_TRUE(n.has_value()) << n.error();
    auto& bus = **n;
    const auto seen = [&] {
        for (int k = 0; k < 60; ++k) {
            if (!pump(100ms)) return std::vector<xbus::Resolved>{};
            auto found = bus.resolve("host/test/system/m/bus/*").value_or(std::vector<xbus::Resolved>{});
            if (!found.empty()) return found;
        }
        return std::vector<xbus::Resolved>{};
    };
    const auto first = seen();
    ASSERT_FALSE(first.empty()) << "the system's router, its own endpoints offered over the link";
    EXPECT_FALSE(first.front().via.empty());
    EXPECT_EQ(links, 1);
    // The system's router is an object of the bus like any: its telemetry is the agent's.
    std::expected<std::vector<std::string>, xbus::CallError> dump = std::unexpected(xbus::CallError{});
    std::jthread caller([&] { dump = bus.call("host/test/system/m/bus/telemetry", "dump", 5s); pumping = false; });
    while (pumping) ASSERT_TRUE(pump(50ms));
    caller.join();
    ASSERT_TRUE(dump.has_value()) << dump.error().message;
    std::string text;
    for (const auto& p : *dump) text += p;
    const auto d = Json::parse(text, nullptr, false);
    EXPECT_EQ(d.value("program", ""), "luban-agent") << text.substr(0, 300);
    // The host's side ends the link (its router went away): the guest links again by itself.
    ASSERT_TRUE(c.send_frame('L', 0, ""));
    {
        std::lock_guard l(m);
        if (end) end->closed();
        end.reset();
    }
    for (int k = 0; k < 20 && !bus.resolve("host/test/system/m/bus/*").value_or(std::vector<xbus::Resolved>{}).empty(); ++k)
        ASSERT_TRUE(pump(100ms));
    EXPECT_TRUE(bus.resolve("host/test/system/m/bus/*").value_or(std::vector<xbus::Resolved>{}).empty()) << "withdrawn with the link";
    const auto again = seen();
    EXPECT_FALSE(again.empty()) << "linked again";
    EXPECT_EQ(links, 2);
}

XTEST(LubanBus, TheHostsRouterIsOneAHomeStartedByTheFirstThatNeedsIt, .area = "luban", .covers = {"LUBAN-BUS-HOST"},
      .requires_ = {"linux", "xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = tk::Home::isolated("luban-bus-host");
    xlings::platform::set_env_variable("LUBAN_BIN", bin->string());
    const auto fd = luban::vm::connect_bus(home.root());
    ASSERT_TRUE(fd.has_value()) << fd.error();
    std::vector<int> opened{*fd};
    xlings::platform::close_fds(opened);
    const xbus::Address at{"unix", luban::vm::bus_socket(home.root()).string()};
    auto n = xbus::Node::connect({.name = "test", .router = at, .telemetry = false});
    ASSERT_TRUE(n.has_value()) << n.error();
    EXPECT_TRUE((*n)->root().starts_with("host/")) << (*n)->root();
    // A second is not a second router: it finds the first and leaves.
    tk::RunOptions o;
    o.argv = {bin->string(), "__bus-router", home.root().string()};
    o.timeout = 10s;
    const auto second = tk::run(o);
    EXPECT_EQ(second.exit_code, 0) << second.transcript();
    EXPECT_NE(second.err.find("already answers"), std::string::npos) << second.transcript();
    EXPECT_TRUE((*n)->call("bus/stats").has_value()) << "the first still serves";
    // Its log is the home's.
    EXPECT_TRUE(fs::exists(home.root() / "logs" / "openbus" / "router.ndjson"));
    // Ended for the test: the router serving this home.
    for (const auto& p : xlings::platform::own_processes())
        if (std::ranges::find(p.argv, home.root().string()) != p.argv.end() && std::ranges::find(p.argv, "__bus-router") != p.argv.end())
            (void)xlings::platform::send_signal(p.pid, xlings::platform::sig::terminate);
}
