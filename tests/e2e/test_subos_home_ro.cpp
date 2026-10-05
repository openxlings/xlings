// The xlings home as a sandbox sees it (C10) and the broker that changes it
// on the sandbox's behalf (C15).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("home-ro");
    Box() {
        home.seed_sandbox_backend();
        for (auto n : {"box", "other"}) {
            auto r = home.xlings({"subos", "new", n});
            if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
        }
    }
    tk::RunResult in(const std::string& script) const {
        return home.xlings({"subos", "exec", "box", "--sandbox", "--", "/bin/sh", "-c", script});
    }
};

}  // namespace

XTEST(SubosHomeReadOnly, TheHomeIsReadOnlyExceptTheInstancesOwnTree,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F1", "POL-OUTSIDE-INSTANCE"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    ASSERT_EQ(box.home.xlings({"subos", "config", "box", "--sandbox=dev"}).exit_code, 0);
    const auto h = box.home.dir().string();
    auto r = box.in(
        "for f in bin/evil .xlings.json data/evil config/subos/box/policy.json; do "
        "  if touch \"$XLINGS_HOME/$f\" 2>/dev/null; then echo \"WROTE $f\"; else echo \"RO $f\"; fi; "
        "done; "
        "cat $XLINGS_HOME/config/subos/box/policy.json >/dev/null && echo POLICY-READABLE; "
        "touch $XLINGS_HOME/subos/box/mine && echo OWN-WRITABLE");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(r.out.find("WROTE"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("RO bin/evil"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("RO config/subos/box/policy.json"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("POLICY-READABLE"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("OWN-WRITABLE"), std::string::npos) << r.out;
    EXPECT_FALSE(fs::exists(box.home.dir() / "bin" / "evil"));
}

XTEST(SubosHomeReadOnly, OtherInstancesAndTheAuditAreOutOfSight,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F6", "F15"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    tk::write_file(box.home.dir() / "subos" / "other" / "home" / "secret", "x");
    // Some audit exists before this session starts.
    ASSERT_EQ(box.in("true").exit_code, 0);
    auto r = box.in("echo subos=$(ls $XLINGS_HOME/subos | tr '\\n' ,); "
                    "echo logs=$(ls -A $XLINGS_HOME/logs | wc -l); "
                    "echo run=$(ls -A $XLINGS_HOME/run | wc -l)");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("subos=box,"), std::string::npos) << r.out;     // not `other`
    EXPECT_NE(r.out.find("logs=0"), std::string::npos) << r.out;         // the audit is not there
    EXPECT_NE(r.out.find("run=0"), std::string::npos) << r.out;          // nor the sockets
    // ...and outside, it is.
    EXPECT_TRUE(fs::exists(box.home.dir() / "logs" / "subos" / "box" / "events.ndjson"));
}

XTEST(SubosHomeReadOnly, ReadingWorksInsideAndWritesGoThroughTheBroker,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"OBS-PERM"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto r = box.in("xlings --version && xlings subos list >/dev/null && echo READS-OK; "
                    "xlings remove not-installed-anywhere -y; echo remove=$?");
    EXPECT_NE(r.out.find("READS-OK"), std::string::npos) << r.transcript();
    // The broker ran it outside and brought back its exit code -- not a hang
    // on a lock file it could not write, not a refusal.
    EXPECT_EQ(r.out.find("remove=13"), std::string::npos) << r.transcript();
    EXPECT_EQ(r.out.find("remove=0"), std::string::npos) << r.transcript();
    auto log = box.home.xlings({"subos", "log", "box", "--json", "--kind", "perm"});
    auto ev = log.json_lines();
    EXPECT_TRUE(std::ranges::any_of(ev, [](auto& e) {
        return e.value("event", "") == "decision" && e.value("program", "") == "remove"; }))
        << log.transcript();
}

XTEST(SubosHomeReadOnly, WhatOnlyTheOwnerDoesIsRefusedInside,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"EXIT-13"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto r = box.in("xlings self update; echo self=$?; "
                    "xlings subos remove other -y; echo subos=$?; "
                    "xlings install x --subos other; echo other=$?");
    EXPECT_NE(r.out.find("self=13"), std::string::npos) << r.transcript();
    EXPECT_NE(r.out.find("subos=13"), std::string::npos) << r.transcript();
    EXPECT_NE(r.out.find("other=13"), std::string::npos) << r.transcript();
    EXPECT_TRUE(fs::exists(box.home.dir() / "subos" / "other"));
}

XTEST(SubosHomeReadOnly, FetchDenyAndAskAreDecidedOutside,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"PERM-FETCH-DENY", "PERM-FETCH-ASK", "EXIT-75"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    ASSERT_EQ(box.home.xlings({"subos", "config", "box", "--fetch", "deny"}).exit_code, 0);
    auto denied = box.in("xlings install -y anything; echo rc=$?");
    EXPECT_NE(denied.out.find("rc=13"), std::string::npos) << denied.transcript();
    EXPECT_NE(denied.transcript().find("xlings install anything --subos box"), std::string::npos)
        << denied.transcript();

    ASSERT_EQ(box.home.xlings({"subos", "config", "box", "--fetch", "ask"}).exit_code, 0);
    auto asked = box.in("xlings install -y anything; echo rc=$?");
    EXPECT_NE(asked.out.find("rc=75"), std::string::npos) << asked.transcript();
    auto reqs = box.home.xlings({"subos", "requests", "box", "--json"});
    auto lines = reqs.json_lines();
    ASSERT_EQ(lines.size(), 1u) << reqs.transcript();
    const auto id = lines[0].value("id", "");
    EXPECT_EQ(box.home.xlings({"subos", "deny", "box", id}).exit_code, 0);
    EXPECT_TRUE(box.home.xlings({"subos", "requests", "box", "--json"}).json_lines().empty());
    auto log = tk::read_file(box.home.dir() / "logs" / "subos" / "box" / "events.ndjson");
    EXPECT_NE(log.find("request-denied"), std::string::npos);
}

XTEST(SubosHomeReadOnly, AutoFetchInstallsFromInsideAndInstallSubosFromOutside,
      .area = "subos", .cost = tk::Cost::Slow,
      .covers = {"PERM-FETCH-AUTO", "PERM-INSTALL-SUBOS"},
      .requires_ = {"linux", "xlings-bin", "sandbox", "network"}, .resources = {"sandbox"}) {
    Box box;
    ASSERT_EQ(box.home.xlings({"self", "init"}, {}, std::chrono::minutes(5)).exit_code, 0);
    auto outside = box.home.xlings({"install", "-y", "ninja", "--subos", "other"}, {},
                                   std::chrono::minutes(5));
    EXPECT_EQ(outside.exit_code, 0) << outside.transcript();
    auto listed = box.home.xlings({"list"}, {{"XLINGS_ACTIVE_SUBOS", "other"}});
    EXPECT_NE(listed.out.find("ninja"), std::string::npos) << listed.transcript();

    auto inside = box.home.xlings({"subos", "exec", "box", "--sandbox", "--", "xlings", "install",
                                   "-y", "ninja"}, {}, std::chrono::minutes(5));
    EXPECT_EQ(inside.exit_code, 0) << inside.transcript();
    auto box_list = box.home.xlings({"list"}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    EXPECT_NE(box_list.out.find("ninja"), std::string::npos) << box_list.transcript();
}

namespace {
// Talk to the broker the way a hostile process inside would: directly, with
// any argv, skipping the client's own classification.
const char* kRawBrokerRequest =
    "import socket,json,array,sys,os\n"
    "s=socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)\n"
    "s.connect(os.environ.get('XLINGS_BROKER_SOCKET') or '/run/xlings/broker.sock')\n"
    "s.sendmsg([json.dumps({'op':'run','argv':sys.argv[1:]}).encode()],\n"
    "          [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i',[0,1,2]))])\n"
    "print('REPLY', s.recv(65536).decode())\n";
}

XTEST(SubosHomeReadOnly, TheBrokerRunsOnlyWhatChangesThisInstance,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"OBS-PERM", "EXIT-13"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    // `interface` is what the client runs locally; run OUTSIDE, as the
    // owner, it would create an instance. The broker must refuse it.
    auto r = box.home.xlings({"subos", "exec", "box", "--sandbox", "--", "python3", "-c", kRawBrokerRequest,
                              "interface", "create_subos", "--args", R"({"name":"pwned"})"});
    if (r.transcript().find("No such file") != std::string::npos) GTEST_SKIP() << "no python3";
    EXPECT_NE(r.out.find("REPLY"), std::string::npos) << r.transcript();
    EXPECT_NE(r.out.find("\"exit\":13"), std::string::npos) << r.transcript();
    EXPECT_FALSE(fs::exists(box.home.dir() / "subos" / "pwned")) << "the broker ran a command it must not";
}
