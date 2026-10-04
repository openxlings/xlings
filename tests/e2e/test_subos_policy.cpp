// Policy, end to end (C14 + C20): `subos config` declares, the declaration
// holds however the instance is entered, presets isolate what they say they
// isolate, and a policy this version cannot enforce is refused.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("policy");
    Box() {
        home.seed_sandbox_backend();
        auto r = home.xlings({"subos", "new", "box"});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }
    tk::RunResult run(std::vector<std::string> args) const { return home.xlings(std::move(args)); }
    fs::path policy_file() const { return home.dir() / "config" / "subos" / "box" / "policy.json"; }
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

XTEST(SubosPolicyE2E, ConfigDeclaresOutsideTheInstanceAndStatusReportsIt,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-CONFIG-STATUS", "PLAT-STATUS-MATRIX"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    auto c = box.run({"subos", "config", "box", "--sandbox=locked", "--json"});
    ASSERT_EQ(c.exit_code, 0) << c.transcript();
    ASSERT_TRUE(fs::exists(box.policy_file()));
    EXPECT_FALSE(fs::exists(box.home.dir() / "subos" / "box" / "policy.json"))
        << "the policy is not part of what it governs";
    auto ev = tk::read_file(box.home.dir() / "logs" / "subos" / "box" / "events.ndjson");
    EXPECT_NE(ev.find("policy-change"), std::string::npos);

    auto s = box.run({"subos", "status", "box", "--json"});
    ASSERT_EQ(s.exit_code, 0) << s.transcript();
    auto j = nlohmann::json::parse(s.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << s.out;
    EXPECT_EQ(j["policy_source"], "file");
    EXPECT_EQ(j["requested"]["isolation"]["net"], "none");
    EXPECT_EQ(j["effective"]["gates"].size(), 8u);
}

XTEST(SubosPolicyE2E, ADeclaredInstanceIsIsolatedHoweverItIsEntered,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"ISO-NET-NONE", "F5", "F7", "ISO-DISABLE-USERNS"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    ASSERT_EQ(box.run({"subos", "config", "box", "--sandbox=locked"}).exit_code, 0);
    // No --sandbox anywhere: the instance's declaration is what counts.
    auto r = box.run({"subos", "use", "box", "--cmd",
        "echo user=$(id -un) host=$(hostname) tz=$TZ; "
        "echo ifaces=$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' ' | tr '\\n' ,); "
        "echo arp=$(tail -n +2 /proc/net/arp 2>/dev/null | wc -l); "
        "unshare -U true 2>/dev/null && echo nested=yes || echo nested=no"});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("user=user host=box tz=UTC"), std::string::npos) << r.out;   // F7
    EXPECT_NE(r.out.find("ifaces=lo,"), std::string::npos) << r.out;                  // F5, net=none
    EXPECT_NE(r.out.find("arp=0"), std::string::npos) << r.out;                        // no neighbours
    EXPECT_NE(r.out.find("nested=no"), std::string::npos) << r.out;
}

XTEST(SubosPolicyE2E, APresetIsPickedPerCallAndCanOnlyTighten,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-SANDBOX-ALIAS"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto host = box.run({"subos", "exec", "box", "--sandbox", "--", "/bin/sh", "-c",
                         "tail -n +3 /proc/net/dev | wc -l"});
    auto locked = box.run({"subos", "exec", "box", "--sandbox=locked", "--", "/bin/sh", "-c",
                           "tail -n +3 /proc/net/dev | wc -l"});
    ASSERT_EQ(locked.exit_code, 0) << locked.transcript();
    EXPECT_EQ(trim(locked.out), "1");
    EXPECT_NE(trim(host.out), "") << host.transcript();

    ASSERT_EQ(box.run({"subos", "config", "box", "--sandbox=locked"}).exit_code, 0);
    auto looser = box.run({"subos", "exec", "box", "--net", "host", "--", "true"});
    EXPECT_EQ(looser.exit_code, 125) << looser.transcript();
    EXPECT_NE(looser.transcript().find("loosen"), std::string::npos);
    auto dev = box.run({"subos", "exec", "box", "--sandbox=dev", "--", "true"});
    EXPECT_EQ(dev.exit_code, 125) << dev.transcript();
}

XTEST(SubosPolicyE2E, APolicyThisVersionCannotEnforceIsRefused,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-UNKNOWN-REFUSED"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    tk::write_file(box.policy_file(), R"({"extends":"dev","isolation":{"net":"vpn"}})");
    auto r = box.run({"subos", "exec", "box", "--", "true"});
    EXPECT_EQ(r.exit_code, 125) << r.transcript();
    EXPECT_NE(r.transcript().find("vpn"), std::string::npos) << r.transcript();
}

XTEST(SubosPolicyE2E, OnlyTheOwnerOutsideChangesThePolicy,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"POL-DECIDE", "EXIT-13"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    auto r = box.home.xlings({"subos", "config", "box", "--net", "host"},
                             {{"XLINGS_SUBOS_MODE", "sandbox"}});
    EXPECT_EQ(r.exit_code, 13) << r.transcript();
    EXPECT_NE(r.transcript().find("E_PERMISSION"), std::string::npos);
    EXPECT_FALSE(fs::exists(box.policy_file()));
}

namespace {
// A unix socket in the abstract namespace -- what an X server listens on
// (@/tmp/.X11-unix/X0) and what a mount namespace does not hide (#640 F2).
const char* kAbstractProbe =
    "import socket,sys\n"
    "s=socket.socket(socket.AF_UNIX)\n"
    "try:\n s.connect('\\0' + sys.argv[1]); print('REACHED')\n"
    "except OSError as e: print('BLOCKED', e.errno)\n";
}

XTEST(SubosPolicyE2E, TheHostsAbstractSocketsAreOutOfReachWithAPrivateNetwork,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F2"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    // The host listens on an abstract socket, as an X server does.
    const auto name = std::format("xlings-f2-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    tk::RunOptions server;
    server.argv = {"/usr/bin/python3", "-c",
                   "import socket,sys,time\ns=socket.socket(socket.AF_UNIX)\n"
                   "s.bind('\\0'+sys.argv[1]); s.listen(); time.sleep(20)", name};
    server.env = {{"PATH", "/usr/bin:/bin"}};
    server.timeout = std::chrono::seconds(25);
    std::thread t([&] { (void)tk::run(server); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto dev = box.run({"subos", "exec", "box", "--sandbox", "--", "python3", "-c", kAbstractProbe, name});
    auto locked = box.run({"subos", "exec", "box", "--sandbox=locked", "--", "python3", "-c",
                           kAbstractProbe, name});
    t.detach();
    if (dev.out.find("No such file") != std::string::npos) GTEST_SKIP() << "no python3";
    // dev shares the host's network namespace and says so in the design: only
    // a private network hides the abstract namespace.
    EXPECT_NE(dev.out.find("REACHED"), std::string::npos) << dev.transcript();
    EXPECT_NE(locked.out.find("BLOCKED"), std::string::npos) << locked.transcript();
}

XTEST(SubosPolicyE2E, NatGivesAPrivateNetworkThroughPasta,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"ISO-NET-NAT", "F5"},
      .requires_ = {"linux", "xlings-bin", "sandbox", "pasta"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    auto r = box.run({"subos", "exec", "box", "--sandbox=private", "--", "/bin/sh", "-c",
        "echo ifaces=$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' ' | sort | tr '\\n' ,); "
        "echo host=$(hostname)"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("host=box"), std::string::npos) << r.out;
    // lo and pasta's tap: a network of its own, not the host's interfaces.
    auto line = r.out.substr(r.out.find("ifaces="));
    EXPECT_NE(line.find("lo,"), std::string::npos) << r.out;
    EXPECT_EQ(std::ranges::count(line.substr(0, line.find('\n')), ','), 2) << r.out;
}
