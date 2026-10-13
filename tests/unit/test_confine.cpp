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

// persist home / none (Luban OS architecture §10.1): a root's own state is seen
// through bwrap's memory overlay, so a session leaves nothing behind; /home
// stays only with `home`; a backend without a mount namespace refuses.
XTEST(Confine, PersistPutsTheRootsStateBehindAMemoryOverlay, .area = "subos", .covers = {"LUBAN-PERSIST"}) {
    auto home = xlings::testkit::Home::isolated("confine-persist");
    const auto root = home.dir() / "subos" / "box" / "rootfs";
    for (const auto* d : {"etc", "var", "root", "home"}) std::filesystem::create_directories(root / d);
    sp::Request r;
    r.instance = "box";
    r.instance_dir = home.dir() / "subos" / "box";
    r.user = "u";
    r.argv = {"sh"};
    r.root = root;
    r.root_mounts = {{sp::MountKind::RoBind, "/h/.xlings/data/xpkgs/x", "/h/.xlings/data/xpkgs/x"}};
    r.host_exists = [](std::string_view) { return true; };
    const xlings::subos::HomeView view{home.dir()};
    const auto overlays = [](const sp::SandboxSpec& s) {
        std::set<std::string> out;
        for (const auto& m : s.mounts) if (m.kind == sp::MountKind::TmpOverlay) out.insert(m.dst);
        return out;
    };
    r.persist = sp::Persist::All;
    auto all = cf::compile(xlings::subos::policy::legacy(), view, linux_host(true, false, 0), r);
    ASSERT_TRUE(all);
    EXPECT_TRUE(overlays(*all).empty());
    r.persist = sp::Persist::Home;
    auto kept = cf::compile(xlings::subos::policy::legacy(), view, linux_host(true, false, 0), r);
    ASSERT_TRUE(kept);
    EXPECT_EQ(overlays(*kept), (std::set<std::string>{"/etc", "/root", "/var"}));
    r.persist = sp::Persist::None;
    auto none = cf::compile(xlings::subos::policy::legacy(), view, linux_host(true, false, 0), r);
    ASSERT_TRUE(none);
    EXPECT_EQ(overlays(*none), (std::set<std::string>{"/etc", "/home", "/root", "/var"}));
    const auto argv = cf::launch_argv(*none, std::nullopt, "/self/xlings");
    EXPECT_NE(std::ranges::find(argv, std::string("--tmp-overlay")), argv.end());
    // The store's binds land under the home (/h/... here, /home/<user>/.xlings
    // on a machine): every overlay and /tmp comes first, or they hide them.
    std::size_t last_own = 0, first_store = none->mounts.size();
    for (std::size_t k = 0; k < none->mounts.size(); ++k) {
        const auto& m = none->mounts[k];
        if (m.kind == sp::MountKind::TmpOverlay || (m.kind == sp::MountKind::Tmpfs && m.dst == "/tmp")) last_own = k;
        if (m.dst == "/h/.xlings/data/xpkgs/x") first_store = std::min(first_store, k);
    }
    EXPECT_LT(last_own, first_store) << "an overlay or /tmp mounted over the store's binds";
    r.preferred = sp::Backend::Proot;
    auto proot = cf::compile(xlings::subos::policy::legacy(), view, linux_host(false, true, 0), r);
    ASSERT_FALSE(proot);
    EXPECT_EQ(proot.error().missing.front().dimension, "persist");
}

// A sandboxed SubOS shares the home's common part (payloads, indexes, shims)
// and never another instance: their trees are behind an empty tmpfs, and of
// config/ only its own directory is there, read-only.
XTEST(Confine, AnInstanceSeesNoOtherInstanceOrItsConfig, .area = "subos", .covers = {"SUBOS-INSTANCES-APART"}) {
    sp::Request r;
    r.instance = "box";
    r.instance_dir = "/h/subos/box";
    r.user = "u";
    r.argv = {"sh"};
    r.host_exists = [](std::string_view) { return true; };
    const xlings::subos::HomeView home{"/h"};
    auto s = cf::compile(xlings::subos::policy::legacy(), home, linux_host(true, false, 0), r);
    ASSERT_TRUE(s);
    const auto has = [&](sp::MountKind k, std::string_view dst) {
        return std::ranges::any_of(s->mounts, [&](const sp::MountOp& m) { return m.kind == k && m.dst == dst; });
    };
    EXPECT_TRUE(has(sp::MountKind::RoBind, "/h")) << "the home, read-only";
    EXPECT_TRUE(has(sp::MountKind::Tmpfs, "/h/subos")) << "other instances out of sight";
    EXPECT_TRUE(has(sp::MountKind::Bind, "/h/subos/box")) << "its own tree, writable";
    EXPECT_TRUE(has(sp::MountKind::Tmpfs, "/h/config/subos")) << "other instances' policy, persona and record";
    EXPECT_TRUE(has(sp::MountKind::RoBind, "/h/config/subos/box")) << "its own config, read-only";
}
