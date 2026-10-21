// XBus, openbus's implementation (luban/services/xbus; design 2026-10-22):
// its router passes openbus's conformance; its nodes offer, call,
// subscribe, publish and get answers through the public interface only;
// two routers linked over a pipe see each other's endpoints; a node finds
// its router again after a restart; the router says what it carries;
// every node and the router answer their process's telemetry.
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
import telemetry;

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

XTEST(Xbus, APatientNodeWaitsForItsRouter, .area = "luban", .covers = {"XBUS-NODE"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-patient");
    const xbus::Address at{"local", (home.root() / "router.sock").string()};
    // A program that starts before the bus: no router answers yet.
    EXPECT_FALSE(xbus::Node::connect({.name = "eager", .router = at, .connect_wait = 200ms}).has_value());
    auto waiting = xbus::Node::connect({.name = "desk", .router = at, .connect_wait = 200ms, .patient = true});
    ASSERT_TRUE(waiting.has_value()) << waiting.error();
    auto desk = *waiting;
    EXPECT_FALSE(desk->connected());
    ASSERT_TRUE(desk->offer("service/xdesk/state", {}, [](xbus::Call c) { c.reply("{}"); c.end(); }));
    std::atomic<int> heard{0};
    desk->subscribe("topic/luban/systems", [&](const xbus::Message&) { ++heard; });
    Running r(home.root());   // the bus comes
    auto client = node(r.at, "client");
    std::expected<std::vector<std::string>, xbus::CallError> answer = std::unexpected(xbus::CallError{});
    for (int k = 0; k < 60 && !answer; ++k) {
        answer = client->call("service/xdesk/state", {}, 1s);
        if (!answer) std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(answer.has_value()) << "what it offered while waiting, offered when the router came";
    EXPECT_EQ(answer->front(), "{}");
    EXPECT_TRUE(desk->connected());
    for (int k = 0; k < 30 && heard == 0; ++k) {
        client->publish("topic/luban/systems", "up");
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_GT(heard.load(), 0) << "and its subscription made then";
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
        // Apart from what every router and node offers of itself (bus/, node/<id>/telemetry).
        std::erase_if(seen, [](const xbus::Resolved& e) {
            return e.path.find("/bus/") != std::string::npos || e.path.find("/node/") != std::string::npos;
        });
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

// openbus is an object of its own bus (SPEC.md §8.2): two machines' buses
// are joined by a call, and then what one machine has the other uses -- an
// application on A finds a compute device on B by its interface, gives it
// work and gets the result back (design 2026-10-22 §14a).
XTEST(Xbus, TwoBusesAreJoinedByACallAndAComputeDeviceIsUsedAcrossThem, .area = "luban", .covers = {"XBUS-BUS-OBJECT"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-join");
    fs::create_directories(home.root() / "a");
    fs::create_directories(home.root() / "b");
    Running a(home.root() / "a", "host/a");
    Running b(home.root() / "b", "host/b");
    // On B: a GPU's driver offers what it computes (here: the sum of the numbers it is given).
    auto gpu = node(b.at, "gpu-driver");
    ASSERT_TRUE(gpu->offer("device/gpu/0",
                           {.kind = openbus::EndpointKind::device, .verb = "launch", .effect = "external",
                            .interfaces = {"openhal.compute@0.3"}, .properties = {{"memory", "24g"}}},
                           [](xbus::Call c) {
                               long sum = 0;
                               std::istringstream in(c.body());
                               for (long x; in >> x;) sum += x;
                               c.state(openbus::TaskState::working);
                               c.reply(std::to_string(sum));
                               c.end();
                           }));
    // On A: the router is an object; joining B's bus is a call to it.
    auto admin = node(a.at, "admin");
    const auto router = admin->resolve("bus/*", "openbus.router@0.1");
    ASSERT_TRUE(router.has_value());
    EXPECT_GE(router->size(), 6u) << "stats, topology, verbs, links, link, unlink, trace";
    const auto joined = admin->call("bus/link", b.at.text());
    ASSERT_TRUE(joined.has_value()) << joined.error().message;
    // An application on A: a compute device, whatever machine has it.
    auto app = node(a.at, "app");
    std::vector<xbus::Resolved> devices;
    for (int k = 0; k < 50 && devices.empty(); ++k) {
        devices = app->resolve("**", "openhal.compute@0.3").value_or(std::vector<xbus::Resolved>{});
        if (devices.empty()) std::this_thread::sleep_for(50ms);
    }
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices.front().path, "host/b/device/gpu/0");
    const auto result = app->call(devices.front().path, "1 2 3 4 5");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u) << "its task state, then its result";
    EXPECT_EQ(result->back(), "15");
    const auto links = Json::parse(admin->call("bus/links")->front());
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0]["root"], "host/b");
    // And apart again, by a call.
    ASSERT_TRUE(admin->call("bus/unlink", "host/b").has_value());
    std::this_thread::sleep_for(300ms);
    EXPECT_FALSE(app->call("host/b/device/gpu/0", "1", 1s).has_value());
}

XTEST(Xbus, EveryNodeAndTheRouterTellTheirTelemetry, .area = "luban", .covers = {"XBUS-TELEMETRY"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-telemetry");
    Running r(home.root());
    auto server = node(r.at, "server");
    auto quiet = xbus::Node::connect({.name = "quiet", .router = r.at, .telemetry = false});
    ASSERT_TRUE(quiet.has_value()) << quiet.error();
    auto client = *quiet;
    ASSERT_TRUE(server->offer("service/demo/echo", {}, [](xbus::Call c) { c.reply(c.body()); c.end(); }));

    // Who can be asked: the router and every node that did not say otherwise.
    const auto told = client->resolve("**", "openbus.telemetry@0.1");
    ASSERT_TRUE(told.has_value());
    std::string server_path;
    bool router = false;
    for (const auto& e : *told) {
        if (e.name == "server") server_path = e.path;
        router = router || e.path == "host/test/bus/telemetry";
        EXPECT_NE(e.name, "quiet");
    }
    ASSERT_FALSE(server_path.empty());
    EXPECT_TRUE(router);
    EXPECT_EQ(server_path, "host/test/node/" + server->id() + "/telemetry");

    const auto ask = [&](const std::string& path, std::string_view request) {
        const auto replies = client->call(path, request);
        if (!replies) throw std::runtime_error(replies.error().message);
        std::string text;
        for (const auto& p : *replies) text += p;
        return Json::parse(text);
    };
    // A node's metrics, profile and trace.
    EXPECT_EQ(ask(server_path, "profile on")["profiling"], true);
    for (int k = 0; k < 20; ++k) ASSERT_TRUE(client->call("service/demo/echo", "x"));
    const auto metrics = ask(server_path, "metrics")["metrics"];
    EXPECT_GE(metrics["counters"]["xbus.node.answered"].get<std::uint64_t>(), 20u) << metrics.dump(1);
    EXPECT_GE(metrics["histograms"]["xbus.node.call_ns"]["count"].get<std::uint64_t>(), 20u);
    EXPECT_GT(metrics["histograms"]["xbus.router.frame_ns"]["count"].get<std::uint64_t>(), 40u);
    const auto trace = ask(server_path, "trace");
    const auto answers = std::ranges::count_if(trace["traceEvents"], [](const Json& e) {
        return e["ph"] == "X" && e["name"] == "answer" && e["cat"] == "xbus.node";
    });
    const auto routed = std::ranges::count_if(trace["traceEvents"], [](const Json& e) {
        return e["ph"] == "X" && e["name"] == "call" && e["cat"] == "xbus.router";
    });
    EXPECT_GE(answers, 20);
    EXPECT_GE(routed, 20);
    EXPECT_EQ(ask(server_path, "profile off")["profiling"], false);

    // The router's dump carries what it is; its log is telemetry's, one object a line.
    const auto dump = ask("bus/telemetry", "dump");
    EXPECT_EQ(dump["telemetry"], "0.1");
    const auto self = dump["state"]["xbus.router." + r.router->id()];
    EXPECT_EQ(self["topology"]["root"], "host/test") << dump["state"].dump(1);
    EXPECT_FALSE(self["stats"]["endpoints"].empty());
    bool connect_logged = false;
    for (const auto& line : std::views::split(tk::read_file(home.root() / "router.ndjson"), '\n')) {
        const std::string text(line.begin(), line.end());
        if (text.empty()) continue;
        const auto j = Json::parse(text);
        EXPECT_EQ(j["component"], "xbus-router");
        connect_logged = connect_logged || (j["event"] == "connect" && j["level"] == "info");
    }
    EXPECT_TRUE(connect_logged);

    // An answer larger than a frame (1 MiB) comes in pieces and is whole when joined.
    const std::string pad(300, 'p');
    for (int k = 0; k < 4096; ++k) telemetry::mark("filler", "test", {{"k", k}, {"pad", pad}});
    const auto pieces = client->call(server_path, "recent 4096");
    ASSERT_TRUE(pieces.has_value());
    EXPECT_GE(pieces->size(), 3u);
    std::string whole;
    for (const auto& p : *pieces) whole += p;
    EXPECT_EQ(Json::parse(whole)["recent"].size(), 4096u);
    // A request it does not know is answered with the ones it does.
    EXPECT_TRUE(ask(server_path, "make coffee")["requests"].is_array());
}

namespace {

// A stream's other side: each opening echoed on a thread of its own -- bytes
// back upper-cased, a region's reference answered with the sum of its bytes.
struct Echo {
    std::mutex m;
    std::vector<std::jthread> threads;
    std::atomic<bool> read_nothing { false };
    void operator()(xbus::Stream s) {
        std::lock_guard l(m);
        threads.emplace_back([this, s]() mutable {
            if (read_nothing) { while (!s.ended()) std::this_thread::sleep_for(20ms); return; }
            for (;;) {
                auto c = s.read(5s);
                if (!c) return;
                if (c->ref) {
                    auto r = s.region(c->ref->id);
                    std::uint64_t sum = 0;
                    if (r) for (std::uint64_t k = 0; k < c->ref->length; ++k) sum += static_cast<std::uint8_t>(r->bytes()[c->ref->offset + k]);
                    (void)s.write(std::format("sum:{}:{}", c->ref->version, sum));
                    continue;
                }
                std::ranges::transform(c->bytes, c->bytes.begin(), [](char ch) { return static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); });
                if (!s.write(c->bytes)) return;
            }
        });
    }
};

std::string read_exactly(xbus::Stream& s, std::size_t n) {
    std::string got;
    while (got.size() < n) {
        auto c = s.read(5s);
        if (!c) break;
        got += c->bytes;
    }
    return got;
}

}  // namespace

XTEST(Xbus, AStreamCarriesBytesWithCreditAndARegionInPlace, .area = "luban", .covers = {"XBUS-STREAM", "XBUS-REGION"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-stream");
    Running r(home.root());
    auto server = node(r.at, "server");
    auto client = node(r.at, "client");
    Echo echo;
    ASSERT_TRUE(server->offer_stream("service/demo/pipe", {}, std::ref(echo), {.window = 64u << 10}));
    // A megabyte through a 64 KiB window: credit, granted as it is read.
    auto st = client->open("service/demo/pipe", "hello", {.window = 64u << 10});
    ASSERT_TRUE(st.has_value()) << st.error().message;
    std::string sent(1u << 20, 'a');
    for (std::size_t k = 0; k < sent.size(); k += 97) sent[k] = 'b';
    std::jthread writer([&] { EXPECT_TRUE(st->write(sent, 10s)); });
    const auto back = read_exactly(*st, sent.size());
    writer.join();
    std::string upper = sent;
    std::ranges::transform(upper, upper.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });
    EXPECT_EQ(back.size(), sent.size());
    EXPECT_TRUE(back == upper) << "every byte, in order";
    // A region: 8 MiB both map; a reference is 24 bytes, the bytes read in place.
    auto region = st->create_region(8u << 20);
    ASSERT_TRUE(region.has_value()) << region.error();
    // Shared over a local socket (memfd); a Mac whose socket refuses the
    // handle goes inline by itself -- the reference reads the same either way.
    if constexpr (!tk::is_macos) EXPECT_TRUE(region->shared()) << "memfd over a local socket";
    std::uint64_t sum = 0;
    for (std::size_t k = 0; k < region->size(); ++k) {
        region->bytes()[k] = static_cast<std::byte>(k % 251);
        sum += k % 251;
    }
    ASSERT_TRUE(st->send_ref(*region, 0, region->size(), 7));
    EXPECT_EQ(read_exactly(*st, std::format("sum:7:{}", sum).size()), std::format("sum:7:{}", sum));
    st->close();
    // Credit is kept: a side that reads nothing stops a writer at its window.
    echo.read_nothing = true;
    auto stuck = client->open("service/demo/pipe", {}, {});
    ASSERT_TRUE(stuck.has_value());
    EXPECT_FALSE(stuck->write(std::string(256u << 10, 'x'), 300ms)) << "past the 64 KiB granted";
    stuck->close();
    // Nothing offered there: the opening fails, said.
    const auto none = client->open("service/demo/nothing", {}, {}, 2s);
    ASSERT_FALSE(none.has_value());
    std::lock_guard l(echo.m);
    for (auto& t : echo.threads) t.join();
}

XTEST(Xbus, ARegionAcrossALinkIsCopiedInline, .area = "luban", .covers = {"XBUS-REGION"}) {
    if constexpr (tk::is_windows) GTEST_SKIP() << "see above";
    auto home = tk::Home::isolated("xbus-region-link");
    fs::create_directories(home.root() / "host");
    fs::create_directories(home.root() / "guest");
    Running host(home.root() / "host", "host/test");
    Running guest(home.root() / "guest", "host/test/system/m");
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
    Echo echo;
    ASSERT_TRUE(inside->offer_stream("service/demo/pipe", {}, std::ref(echo)));
    auto outside = node(host.at, "outside");
    std::expected<xbus::Stream, xbus::CallError> st = std::unexpected(xbus::CallError{});
    for (int k = 0; k < 50 && !st; ++k) {
        st = outside->open("host/test/system/m/service/demo/pipe", {}, {}, 1s);
        if (!st) std::this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(st.has_value()) << st.error().message;
    const auto before = telemetry::metrics().counter("xbus.node.region_fallbacks").value();
    // A pipe carries no memory: the region is inline, each reference its bytes.
    auto region = st->create_region(3u << 20);
    ASSERT_TRUE(region.has_value()) << region.error();
    EXPECT_FALSE(region->shared());
    EXPECT_GT(telemetry::metrics().counter("xbus.node.region_fallbacks").value(), before) << "the fallback counted";
    std::uint64_t sum = 0;
    for (std::size_t k = 0; k < region->size(); ++k) {
        region->bytes()[k] = static_cast<std::byte>((k * 7) % 256);
        sum += (k * 7) % 256;
    }
    ASSERT_TRUE(st->send_ref(*region, 0, region->size(), 1));
    // In pieces: one answer a piece; their sums add up to the whole.
    std::uint64_t got = 0;
    for (int k = 0; k < 6 && got < sum; ++k) {
        auto c = st->read(5s);
        ASSERT_TRUE(c.has_value());
        const auto colon = c->bytes.rfind(':');
        got += std::stoull(c->bytes.substr(colon + 1));
    }
    EXPECT_EQ(got, sum);
    st->close();
    {
        std::lock_guard l(echo.m);
        for (auto& t : echo.threads) t.join();
    }
    // The routers stop before the pipe they move bytes through goes.
    for (auto* r : {&host, &guest}) {
        r->stop = true;
        r->thread.join();
    }
}
