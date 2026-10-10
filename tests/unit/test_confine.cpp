// The confine registry (SubOS design part 3 §6.2): implementations, the
// selector, and a platform matrix made of their claims.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.caps;
import xlings.subos.policy;
import xlings.subos.spec;
import xlings.subos.home_view;
import xlings.confine;
import xlings.confine.gates;

namespace sp = xlings::subos::spec;
namespace caps = xlings::subos::caps;
namespace cf = xlings::confine;
namespace gates = xlings::confine::gates;

namespace {

caps::Caps linux_host(bool bwrap, bool proot, int landlock) {
    caps::Caps c;
    c.platform = "linux";
    if (bwrap) c.bwrap = caps::Backend{.name = "bwrap", .bin = "/opt/bwrap", .source = "payload", .usable = true};
    if (proot) c.proot = caps::Backend{.name = "proot", .bin = "/opt/proot", .source = "payload", .usable = true};
    c.landlock_abi = landlock;
    c.seccomp = true;
    return c;
}

const gates::Status& row(const std::vector<gates::Status>& m, std::string_view g) {
    return *std::ranges::find(m, g, &gates::Status::gate);
}

}  // namespace

XTEST(Confine, EveryBackendHasOneImplementationAndEveryClaimNamesAnInterface,
      .area = "subos", .covers = {"GATE-CONFORM"}) {
    std::set<sp::Backend> seen;
    for (const auto& impl : cf::implementations()) {
        EXPECT_TRUE(seen.insert(impl.backend).second) << impl.name;
        EXPECT_EQ(cf::find(impl.backend), &impl);
        ASSERT_TRUE(impl.available && impl.view && impl.processes && impl.finish) << impl.name;
    }
    const std::set<std::string> known{"FsGate", "ProcessScope", "NetGate", "DeviceGate",
                                      "IdentityShim", "ExecTracer", "SessionHost", "RootfsRuntime"};
    caps::Caps mac, win;
    mac.platform = "macos";
    win.platform = "windows";
    for (const auto& host : {linux_host(true, true, 3), linux_host(false, true, 0), mac, win}) {
        for (const auto& impl : cf::implementations())
            if (impl.gates)
                for (const auto& claim : impl.gates(host)) EXPECT_TRUE(known.contains(claim.gate)) << claim.gate;
        const auto m = gates::probe(host);
        ASSERT_EQ(m.size(), known.size()) << host.platform;
        for (const auto& s : m) EXPECT_TRUE(known.contains(s.gate));
    }
}

XTEST(Confine, TheMatrixIsTheStrongestClaimAndNamesTheRouteWhenNothingClaims,
      .area = "subos", .covers = {"GATE-CONFORM"}) {
    // Landlock's kernel fence outranks proot's advisory view.
    const auto fenced = gates::probe(linux_host(false, true, 3));
    EXPECT_EQ(row(fenced, "FsGate").enforced, gates::Enforced::Kernel);
    EXPECT_NE(row(fenced, "FsGate").reason.find("Landlock"), std::string::npos);
    const auto view = gates::probe(linux_host(false, true, 0));
    EXPECT_EQ(row(view, "FsGate").enforced, gates::Enforced::Advisory);
    const auto none = gates::probe(linux_host(false, false, 0));
    EXPECT_FALSE(row(none, "FsGate").supported);
    EXPECT_FALSE(row(none, "RootfsRuntime").route.empty());
    EXPECT_TRUE(row(none, "SessionHost").supported) << "the session host is not a backend's";
}

XTEST(Confine, TheSelectorHonoursANamedBackendOrSaysWhyNotAndLandlockStartsAsThisBinary,
      .area = "subos", .covers = {"GATE-CONFORM", "INTENT-EQ"}) {
    sp::Request r;
    r.instance = "box";
    r.instance_dir = "/h/subos/box";
    r.user = "u";
    r.argv = {"make"};
    r.host_exists = [](std::string_view) { return true; };
    const xlings::subos::HomeView home{"/h"};
    r.preferred = sp::Backend::Landlock;
    auto refused = cf::compile(xlings::subos::policy::legacy(), home, linux_host(true, false, 0), r);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().missing.front().dimension, "backend");
    auto fenced = cf::compile(xlings::subos::policy::legacy(), home, linux_host(true, false, 2), r);
    ASSERT_TRUE(fenced);
    EXPECT_EQ(fenced->backend, sp::Backend::Landlock);
    auto launched = *fenced;
    launched.argv = {"/run/xlings/xlings", "__session-init", "--", "make"};
    EXPECT_EQ(cf::launch_argv(launched, std::nullopt, "/self/xlings"),
              (std::vector<std::string>{"/self/xlings", "__session-init", "--", "make"}));
}

XTEST(Confine, APolicyThatForbidsNestedUserNamespacesRefusesABwrapThatCannotWithTheFix,
      .area = "subos", .covers = {"DOC-ISOLATION", "INTENT-EQ", "PRIVATE-USERNS-RESTRICTED-HOST"}) {
    // A setuid bwrap (a recipe's, where sudo needs no password) enters, but
    // does not support --disable-userns: bwrap failing after the entry left
    // the session without its init (xim-pkgindex #945, Ubuntu 24.04 runner).
    namespace policy = xlings::subos::policy;
    sp::Request r;
    r.instance = "box";
    r.instance_dir = "/h/subos/box";
    r.user = "u";
    r.argv = {"make"};
    r.host_exists = [](std::string_view) { return true; };
    const xlings::subos::HomeView home{"/h"};
    auto locked = policy::preset(policy::Preset::Locked);
    locked.no_degrade = true;
    ASSERT_TRUE(locked.disable_userns);
    auto able = cf::compile(locked, home, linux_host(true, false, 0), r);
    ASSERT_TRUE(able);
    EXPECT_TRUE(able->disable_userns);
    auto setuid = linux_host(true, false, 0);
    setuid.bwrap->disable_userns_fails = "bwrap: --disable-userns not supported in setuid mode";
    auto refused = cf::compile(locked, home, setuid, r);
    ASSERT_FALSE(refused);
    const auto& why = refused.error().missing.front();
    EXPECT_EQ(why.dimension, "userns");
    EXPECT_NE(why.reason.find("setuid mode"), std::string::npos) << why.reason;
    EXPECT_NE(why.fix.find("self doctor --isolation --fix"), std::string::npos) << why.fix;
    // A policy that does not forbid them is not affected.
    EXPECT_TRUE(cf::compile(policy::legacy(), home, setuid, r));
}
