// XBus, openbus's implementation (luban/services/xbus; design 2026-10-22):
// its router passes openbus's conformance; its nodes offer, call,
// subscribe, publish and get answers through the public interface only;
// two routers linked over a pipe see each other's endpoints; a node finds
// its router again after a restart; the router says what it carries.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import openbus;
import openbus.conformance;
import xbus.router;
import xbus.node;
import xbus.raw;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// A router on a thread of its own, at a local address in the test's home.
struct Running {
    std::unique_ptr<xbus::Router> router;
    std::atomic<bool> stop { false };
    std::jthread thread;
    xbus::Address at;
    explicit Running(const fs::path& dir, std::string root = "host/test") {
        at = xbus::Address{"local", (dir / "router.sock").string()};
        router = std::make_unique<xbus::Router>(xbus::RouterOptions{.root = std::move(root), .listen = {at}, .log = dir / "router.ndjson"});
        if (auto l = router->listen(); !l) throw std::runtime_error(l.error());
        thread = std::jthread([this] { router->run(stop); });
    }
    ~Running() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
};

struct Raw final : openbus::conformance::Connection {
    std::unique_ptr<xbus::RawConnection> c;
    bool send(std::string_view b) override { return c->send(b); }
    std::optional<std::string> receive(std::chrono::milliseconds w) override { return c->receive(w); }
};

std::shared_ptr<xbus::Node> node(const xbus::Address& at, std::string name) {
    auto n = xbus::Node::connect({.name = std::move(name), .router = at});
    if (!n) throw std::runtime_error(n.error());
    return *n;
}

}  // namespace

XTEST(Xbus, TheRouterPassesOpenbusConformance, .area = "luban", .covers = {"OPENBUS-CONFORMANCE"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "the Windows backend's local sockets are checked on Windows runners";
    auto home = tk::Home::isolated("xbus-conformance");
    Running r(home.root());
    const auto report = openbus::conformance::check([&]() -> std::unique_ptr<openbus::conformance::Connection> {
        auto c = xbus::RawConnection::connect(r.at);
        if (!c) return nullptr;
        auto raw = std::make_unique<Raw>();
        raw->c = std::move(*c);
        return raw;
    });
    for (const auto& f : report.failed) ADD_FAILURE() << f;
    EXPECT_GT(report.held, 25);
}

XTEST(Xbus, NodesOfferCallSubscribeAndPublish, .area = "luban", .covers = {"XBUS-NODE"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-node");
    Running r(home.root());
    auto server = node(r.at, "server");
    auto client = node(r.at, "client");
    EXPECT_EQ(client->root(), "host/test");

    // A service: three replies, streamed; who called, said.
    std::string caller_actor;
    ASSERT_TRUE(server->offer("service/demo/count", {.verb = "count", .effect = "read"}, [&](xbus::Call c) {
        caller_actor = c.actor();
        for (int k = 1; k <= 3; ++k) c.reply(std::to_string(k));
        c.end();
    }));
    const auto parts = client->call("service/demo/count");
    ASSERT_TRUE(parts.has_value()) << parts.error().message;
    EXPECT_EQ(*parts, (std::vector<std::string>{"1", "2", "3"}));
    EXPECT_EQ(caller_actor, "program:client");
    std::vector<std::string> each;
    ASSERT_TRUE(client->call_each("service/demo/count", {}, [&](std::string_view p) { each.emplace_back(p); }));
    EXPECT_EQ(each.size(), 3u);
    const auto missing = client->call("service/demo/nothing", {}, 2s);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, openbus::ErrorCode::not_found);
    // One node per path.
    const auto twice = client->offer("service/demo/count");
    ASSERT_FALSE(twice.has_value());
    EXPECT_EQ(twice.error().code, openbus::ErrorCode::taken);

    // Resolve: this one, absolute.
    const auto found = client->resolve("**/demo/*");
    ASSERT_TRUE(found.has_value());
    ASSERT_EQ(found->size(), 1u);
    EXPECT_EQ(found->front().path, "host/test/service/demo/count");
    EXPECT_EQ(found->front().name, "server");

    // Publish/subscribe; a retained topic's last word to a late subscriber; events.
    ASSERT_TRUE(server->offer("topic/demo/news", {.kind = openbus::EndpointKind::topic, .retained = true}));
    std::mutex m;
    std::condition_variable cv;
    std::vector<xbus::Message> got;
    const auto sub = client->subscribe("topic/demo/*", [&](const xbus::Message& msg) {
        std::lock_guard l(m);
        got.push_back(msg);
        cv.notify_all();
    });
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(server->publish("topic/demo/news", "first"));
    ASSERT_TRUE(server->event("topic/demo/tick", 42));
    {
        std::unique_lock l(m);
        ASSERT_TRUE(cv.wait_for(l, 3s, [&] { return got.size() >= 2; })) << got.size();
    }
    EXPECT_EQ(got[0].body, "first");
    EXPECT_EQ(got[0].path, "host/test/topic/demo/news");
    EXPECT_TRUE(got[1].event);
    EXPECT_EQ(got[1].value, 42u);
    client->unsubscribe(sub);
    std::promise<std::string> late;
    auto later = node(r.at, "late");
    later->subscribe("topic/demo/news", [&](const xbus::Message& msg) { late.set_value(msg.body); });
    auto f = late.get_future();
    ASSERT_EQ(f.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(f.get(), "first");

    // A broadcast; a ping's round trip.
    std::promise<std::string> heard;
    server->on_broadcast([&](const xbus::Message& msg) { heard.set_value(msg.body); });
    ASSERT_TRUE(client->broadcast("hello all"));
    auto h = heard.get_future();
    ASSERT_EQ(h.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(h.get(), "hello all");
    const auto rtt = client->ping();
    ASSERT_TRUE(rtt.has_value());
    EXPECT_LT(*rtt, 50ms);

    // A node that leaves: its endpoints with it.
    server->close();
    std::this_thread::sleep_for(200ms);
    const auto after = client->call("service/demo/count", {}, 2s);
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, openbus::ErrorCode::not_found);
}

XTEST(Xbus, ANodeFindsItsRouterAgainAndSaysEverythingAgain, .area = "luban", .covers = {"XBUS-NODE"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-reconnect");
    auto r = std::make_unique<Running>(home.root());
    auto server = node(r->at, "server");
    ASSERT_TRUE(server->offer("service/demo/up", {}, [](xbus::Call c) { c.reply("yes"); c.end(); }));
    r.reset();                                   // the router goes away
    std::this_thread::sleep_for(300ms);
    r = std::make_unique<Running>(home.root());  // and comes back at the same address
    auto client = node(r->at, "client");
    std::expected<std::vector<std::string>, xbus::CallError> answer = std::unexpected(xbus::CallError{});
    for (int k = 0; k < 50 && !answer; ++k) {
        answer = client->call("service/demo/up", {}, 1s);
        if (!answer) std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(answer.has_value()) << "the server offered again by itself";
    EXPECT_EQ(answer->front(), "yes");
}

XTEST(Xbus, TwoRoutersLinkedOverAPipeSeeEachOther, .area = "luban", .covers = {"XBUS-LINK"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-link");
    fs::create_directories(home.root() / "host");
    fs::create_directories(home.root() / "guest");
    Running host(home.root() / "host", "host/test");
    Running guest(home.root() / "guest", "host/test/system/m");
    // The pipe: what one router sends, the other is fed -- as a carrier's channel would.
    std::shared_ptr<xbus::PipeEnd> to_host, to_guest;
    std::mutex wire;
    to_guest = guest.router->adopt({.send = [&](std::string_view b) {
        std::lock_guard l(wire);
        if (to_host) to_host->feed(b);
        return true;
    }});
    {
        std::lock_guard l(wire);
        to_host = host.router->link({.send = [&](std::string_view b) { to_guest->feed(b); return true; }});
    }
    auto inside = node(guest.at, "inside");
    ASSERT_TRUE(inside->offer("service/xdesk/state", {.verb = "state", .effect = "read"}, [](xbus::Call c) { c.reply("{\"windows\":3}"); c.end(); }));
    ASSERT_TRUE(inside->offer("topic/xdesk/events", {.kind = openbus::EndpointKind::topic}));
    auto outside = node(host.at, "outside");
    std::vector<xbus::Resolved> seen;
    for (int k = 0; k < 50 && seen.size() < 2; ++k) {
        seen = outside->resolve("host/test/system/m/**").value_or(std::vector<xbus::Resolved>{});
        std::erase_if(seen, [](const xbus::Resolved& e) { return e.path.find("/bus/") != std::string::npos; });
        if (seen.size() < 2) std::this_thread::sleep_for(50ms);
    }
    ASSERT_EQ(seen.size(), 2u) << "the guest's endpoints, offered over the link";
    EXPECT_FALSE(seen.front().via.empty()) << "and said where they came through";
    const auto state = outside->call("host/test/system/m/service/xdesk/state");
    ASSERT_TRUE(state.has_value()) << state.error().message;
    EXPECT_EQ(state->front(), "{\"windows\":3}");
    // A subscription on the host, a publication in the system.
    std::promise<std::string> event;
    std::once_flag once;
    outside->subscribe("host/test/system/*/topic/xdesk/events", [&](const xbus::Message& msg) {
        std::call_once(once, [&] { event.set_value(msg.body); });
    });
    std::this_thread::sleep_for(200ms);
    ASSERT_TRUE(inside->publish("topic/xdesk/events", "focus"));
    auto e = event.get_future();
    ASSERT_EQ(e.wait_for(3s), std::future_status::ready) << "a publication crossed the link";
    EXPECT_EQ(e.get(), "focus");
    EXPECT_EQ(host.router->links(), 1u);
    // The link goes: what came through it is withdrawn.
    to_host->closed();
    std::this_thread::sleep_for(300ms);
    const auto gone = outside->call("host/test/system/m/service/xdesk/state", {}, 2s);
    EXPECT_FALSE(gone.has_value());
}

XTEST(Xbus, TheRouterSaysWhatItCarries, .area = "luban", .covers = {"XBUS-OBSERVE"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-observe");
    Running r(home.root());
    auto server = node(r.at, "server");
    auto client = node(r.at, "client");
    ASSERT_TRUE(server->offer("service/demo/echo", {.verb = "echo", .effect = "read"}, [](xbus::Call c) { c.reply(c.body()); c.end(); }));
    // The trace: every frame routed, while somebody listens.
    std::atomic<int> traced{0};
    auto monitor = node(r.at, "monitor");
    monitor->subscribe("bus/trace", [&](const xbus::Message&) { ++traced; });
    std::this_thread::sleep_for(100ms);
    for (int k = 0; k < 20; ++k) ASSERT_TRUE(client->call("service/demo/echo", "x"));
    std::this_thread::sleep_for(200ms);
    EXPECT_GE(traced.load(), 40) << "a call and its reply-end each traced";

    const auto stats = client->call("bus/stats");
    ASSERT_TRUE(stats.has_value());
    const auto s = Json::parse(stats->front());
    bool counted = false;
    for (const auto& e : s["endpoints"])
        if (e["path"] == "host/test/service/demo/echo") {
            counted = true;
            EXPECT_EQ(e["calls"], 20);
            EXPECT_EQ(e["latency_ns"]["count"], 20);
            EXPECT_GT(e["latency_ns"]["p50"].get<std::uint64_t>(), 0u);
        }
    EXPECT_TRUE(counted) << s.dump(1);
    const auto topo = Json::parse(client->call("bus/topology")->front());
    EXPECT_EQ(topo["root"], "host/test");
    EXPECT_GE(topo["nodes"].size(), 3u);
    EXPECT_EQ(topo["nodes"][0]["transport"], "local");
    const auto verbs = Json::parse(client->call("bus/verbs")->front());
    EXPECT_TRUE(std::ranges::any_of(verbs, [](const Json& v) { return v["verb"] == "echo" && v["effect"] == "read"; })) << verbs.dump();
    // Its log: who came and went.
    const auto log = tk::read_file(home.root() / "router.ndjson");
    EXPECT_NE(log.find("\"event\":\"connect\""), std::string::npos) << log;
    EXPECT_NE(log.find("\"event\":\"offer\""), std::string::npos) << log;
}

XTEST(Xbus, ObjectsAreFoundByWhatTheyDoAndACompositeIsOneOfThem, .area = "luban", .covers = {"XBUS-OBJECTS"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-objects");
    Running r(home.root());
    // Two memory devices on two nodes, and a node that makes one of them.
    auto a = node(r.at, "device-a");
    auto b = node(r.at, "device-b");
    auto fuser = node(r.at, "fuser");
    const xbus::OfferOptions memory{.kind = openbus::EndpointKind::device, .interfaces = {"openhal.memory@0.3"}};
    auto with = [](xbus::OfferOptions o, std::string size) { o.properties = {{"capacity", std::move(size)}}; return o; };
    ASSERT_TRUE(a->offer("device/memory/0", with(memory, "16g")));
    ASSERT_TRUE(b->offer("device/memory/1", with(memory, "32g")));
    auto fused = with(memory, "48g");
    fused.members = {"host/test/device/memory/0", "host/test/device/memory/1"};
    fused.policy = "tiered";
    ASSERT_TRUE(fuser->offer("device/memory/fused0", fused, [](xbus::Call c) { c.reply("48g"); c.end(); }));
    ASSERT_TRUE(a->offer("service/other", {}));
    // Whoever needs memory asks for the interface, not for a node.
    auto user = node(r.at, "user");
    const auto found = user->resolve("**", "openhal.memory@0.3");
    ASSERT_TRUE(found.has_value());
    ASSERT_EQ(found->size(), 3u) << "every memory device, the composite one too";
    EXPECT_TRUE(std::ranges::all_of(*found, [](const xbus::Resolved& e) {
        return std::ranges::contains(e.interfaces, std::string("openhal.memory@0.3"));
    }));
    EXPECT_EQ(user->call("device/memory/fused0")->front(), "48g");
    // Every node both offers and uses: the one that made the composite uses its members.
    const auto members = fuser->resolve("device/memory/*", "openhal.memory@0.3");
    ASSERT_TRUE(members.has_value());
    EXPECT_EQ(members->size(), 3u);
}
