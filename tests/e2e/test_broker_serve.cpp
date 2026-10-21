// A machine's broker on the host (design 2026-10-21 §3.3): what the guest
// agent relays from root inside is judged by the one verdict every
// supervisor gives (broker::judge, the instance's policy), recorded with who
// asked and how it came, then run for the instance -- its own flags kept.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::vector<Json> events(const tk::Home& home, std::string_view instance) {
    std::vector<Json> out;
    std::istringstream in(tk::read_file(home.dir() / "logs" / "subos" / std::string(instance) / "events.ndjson"));
    for (std::string l; std::getline(in, l);)
        if (auto j = Json::parse(l, nullptr, false); j.is_object()) out.push_back(j);
    return out;
}

}  // namespace

XTEST(BrokerServe, TheHostJudgesRecordsAndRunsWhatRootInsideAsked,
      .area = "subos", .covers = {"SUBOS-MACHINE-BROKER"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban machine's host is Linux here";
    auto home = tk::Home::isolated("broker-serve");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto serve = [&](std::vector<std::string> request) {
        std::vector<std::string> args{"subos", "__broker-serve", "box", "--actor", "alice", "--"};
        args.insert(args.end(), request.begin(), request.end());
        return home.xlings(args);
    };

    // The store's owner's: refused, with the command that does it on the host.
    auto owner = serve({"config", "--mirror", "CN"});
    EXPECT_EQ(owner.exit_code, 13) << owner.transcript();
    EXPECT_NE(owner.transcript().find("on the host"), std::string::npos) << owner.transcript();
    // What runs inside is never the broker's.
    auto read = serve({"search", "x"});
    EXPECT_EQ(read.exit_code, 13) << read.transcript();
    EXPECT_NE(read.transcript().find("not something the broker runs"), std::string::npos) << read.transcript();
    // This system's own state: run, its flags after `--` its own.
    auto removed = serve({"remove", "nothing-installed", "-y"});
    EXPECT_EQ(removed.transcript().find("not something the broker runs"), std::string::npos) << removed.transcript();

    const auto all = events(home, "box");
    std::vector<Json> decisions;
    for (const auto& e : all) if (e.value("event", "") == "decision") decisions.push_back(e);
    ASSERT_EQ(decisions.size(), 3u) << tk::read_file(home.dir() / "logs/subos/box/events.ndjson");
    for (const auto& d : decisions) {
        EXPECT_EQ(d.value("actor", ""), "alice");
        EXPECT_EQ(d.value("via", ""), "machine");
    }
    EXPECT_EQ(decisions[0].value("action", ""), "deny");
    EXPECT_EQ(decisions[2].value("action", ""), "allow");
    EXPECT_EQ(decisions[2].value("argc", 0), 3) << "the request's -y reached the broker";
    const auto done = std::ranges::find_if(all, [](const Json& e) { return e.value("event", "") == "broker-done"; });
    ASSERT_NE(done, all.end());
    EXPECT_EQ(done->value("program", ""), "remove");
    EXPECT_EQ(done->value("exit", -1), removed.exit_code);
}
