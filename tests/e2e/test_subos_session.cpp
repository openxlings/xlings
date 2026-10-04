// Sessions (C9): a supervisor outside the sandbox hosts it, records it, and
// lets later commands join it.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("session");
    std::string name = "box";
    Box() {
        home.seed_sandbox_backend();
        auto r = home.xlings({"subos", "new", name});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }
    tk::RunResult cmd(const std::string& c) const {
        return home.xlings({"subos", "use", name, "--sandbox", "--cmd", c});
    }
    std::vector<nlohmann::json> events() const {
        std::vector<nlohmann::json> out;
        std::istringstream in(tk::read_file(home.dir() / "logs" / "subos" / name / "events.ndjson"));
        std::string line;
        while (std::getline(in, line)) {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (!j.is_discarded()) out.push_back(std::move(j));
        }
        return out;
    }
    bool running() const {
        return fs::exists(home.dir() / "run" / "subos" / name / "session.json");
    }
};

bool wait_for(const std::function<bool()>& pred, std::chrono::seconds limit = std::chrono::seconds(20)) {
    auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

XTEST(SubosSession, TheSupervisorRecordsTheSessionOutsideTheInstance,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"SES-SUPERVISOR", "OBS-LIFECYCLE"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto r = box.cmd("exit 9");
    EXPECT_EQ(r.exit_code, 9) << r.transcript();
    auto ev = box.events();
    ASSERT_GE(ev.size(), 2u);
    EXPECT_EQ(ev.front()["event"], "session-start");
    EXPECT_EQ(ev.front()["kind"], "lifecycle");
    EXPECT_TRUE(ev.front()["spec"].contains("mounts")) << "the isolation snapshot";
    EXPECT_EQ(ev.back()["event"], "session-end");
    EXPECT_EQ(ev.back()["exit"], 9);
    EXPECT_FALSE(box.running()) << "an ended session leaves nothing behind";
}

XTEST(SubosSession, ASecondCommandJoinsTheRunningSession,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"SES-JOIN", "OBS-OPS"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    tk::RunResult first;
    std::thread t([&] { first = box.cmd("echo shared > /tmp/marker; sleep 2; echo first-done"); });
    ASSERT_TRUE(wait_for([&] { return box.running(); })) << "the first session never started";
    // Same /tmp, same processes: the marker the first wrote is here.
    auto second = box.cmd("cat /tmp/marker; exit 4");
    t.join();
    EXPECT_EQ(second.exit_code, 4) << second.transcript();
    EXPECT_NE(second.out.find("shared"), std::string::npos) << second.transcript();
    EXPECT_EQ(first.exit_code, 0) << first.transcript();
    EXPECT_NE(first.out.find("first-done"), std::string::npos);

    auto ev = box.events();
    auto exec = std::ranges::find_if(ev, [](auto& e) { return e.value("event", "") == "exec"; });
    ASSERT_NE(exec, ev.end());
    EXPECT_EQ((*exec)["kind"], "ops");
    EXPECT_TRUE((*exec)["env"].is_array()) << "names, not values";
    auto end = std::ranges::find_if(ev, [](auto& e) { return e.value("event", "") == "exec-end"; });
    ASSERT_NE(end, ev.end());
    EXPECT_EQ((*end)["exit"], 4);
}

XTEST(SubosSession, PsListsAndStopEndsASession,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"SES-START-STOP", "OBS-LOG"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    tk::RunResult first;
    std::thread t([&] { first = box.cmd("sleep 60"); });
    ASSERT_TRUE(wait_for([&] { return box.running(); }));

    auto ps = box.home.xlings({"subos", "ps", "--json"});
    EXPECT_EQ(ps.exit_code, 0) << ps.transcript();
    auto lines = ps.json_lines();
    ASSERT_EQ(lines.size(), 1u) << ps.transcript();
    EXPECT_EQ(lines[0]["instance"], "box");

    auto stop = box.home.xlings({"subos", "stop", "box"});
    EXPECT_EQ(stop.exit_code, 0) << stop.transcript();
    t.join();
    EXPECT_NE(first.exit_code, 0) << "the command was ended, not completed";
    EXPECT_FALSE(box.running());

    auto log = box.home.xlings({"subos", "log", "box", "--json", "--kind", "lifecycle"});
    EXPECT_EQ(log.exit_code, 0);
    auto ev = log.json_lines();
    ASSERT_FALSE(ev.empty());
    for (auto& e : ev) EXPECT_EQ(e["kind"], "lifecycle");
    EXPECT_TRUE(std::ranges::any_of(ev, [](auto& e) {
        return e.value("event", "") == "session-stop-requested"; }));
}
