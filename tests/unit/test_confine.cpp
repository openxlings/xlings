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

// A root's userland has its own glibc's locales, not the host's: a host's
// LC_ALL named one it lacks, and bash said so on every start.
XTEST(Confine, ARootSpeaksCUtf8WhateverTheHostsLocale, .area = "subos", .covers = {"ROOT-LOCALE"}) {
    auto home = xlings::testkit::Home::isolated("confine-locale");
    const auto root = home.dir() / "subos" / "box" / "rootfs";
    std::filesystem::create_directories(root / "etc");
    sp::Request r;
    r.instance = "box";
    r.instance_dir = home.dir() / "subos" / "box";
    r.user = "u";
    r.argv = {"sh"};
    r.root = root;
    r.root_mounts = {{sp::MountKind::RoBind, "/h/.xlings/data/xpkgs/x", "/h/.xlings/data/xpkgs/x"}};
    r.host_exists = [](std::string_view) { return true; };
    r.host_env = {{"LANG", "en_US.UTF-8"}, {"LC_ALL", "en_US.UTF-8"}, {"LC_CTYPE", "zh_CN.UTF-8"},
                  {"LANGUAGE", "en_US"}, {"TERM", "xterm-256color"}};
    const xlings::subos::HomeView view{home.dir()};
    auto spec = cf::compile(xlings::subos::policy::legacy(), view, linux_host(true, false, 0), r);
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->env.at("LANG"), "C.UTF-8");
    for (const auto& [k, v] : spec->env)
        EXPECT_FALSE(k.starts_with("LC_") || k == "LANGUAGE") << k << "=" << v;
}

// What xlings itself sets for a system's session (its prompt's facts) passes
// whatever the policy, with no degradation said; a caller's own --env is
// still held to the policy's env_pass (an agent workspace said
// "! env not in effect: --env LUBAN_KIND" on every command).
XTEST(Confine, ASystemsOwnSessionFactsPassWhateverThePolicy, .area = "subos", .covers = {"SUBOS-SYSTEM-ENV"}) {
    auto home = xlings::testkit::Home::isolated("confine-system-env");
    const auto root = home.dir() / "subos" / "box" / "rootfs";
    std::filesystem::create_directories(root / "etc");
    sp::Request r;
    r.instance = "box";
    r.instance_dir = home.dir() / "subos" / "box";
    r.user = "u";
    r.argv = {"sh"};
    r.root = root;
    r.root_mounts = {{sp::MountKind::RoBind, "/h/.xlings/data/xpkgs/x", "/h/.xlings/data/xpkgs/x"}};
    r.host_exists = [](std::string_view) { return true; };
    r.system_env = {{"LUBAN_KIND", "sandbox"}, {"LUBAN_PERSIST", "all"}, {"PS1", "[luban] $ "}};
    r.explicit_env = {{"MY_SECRET", "x"}};
    const xlings::subos::HomeView view{home.dir()};
    auto strict = xlings::subos::policy::preset(xlings::subos::policy::Preset::Dev);
    strict.env_explicit_any = false;   // a caller's --env only by name
    auto spec = cf::compile(strict, view, linux_host(true, false, 0), r);
    ASSERT_TRUE(spec) << (spec ? std::string{} : (spec.error().missing.empty() ? std::string("refused") : spec.error().missing.front().reason));
    EXPECT_EQ(spec->env.at("LUBAN_KIND"), "sandbox");
    EXPECT_EQ(spec->env.at("PS1"), "[luban] $ ");
    EXPECT_FALSE(spec->env.contains("MY_SECRET")) << "a caller's --env is the policy's to pass";
    for (const auto& d : spec->degraded) EXPECT_EQ(d.reason.find("LUBAN_"), std::string::npos) << d.reason;
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

// The host's /bin, /sbin and /lib* as the host has them (fix plan 2026-10-16
// D2): a merged-/usr host's links resolve into /usr; Alpine's /lib (ld-musl)
// and openSUSE Leap 15's /lib64 (ld-linux) are directories of their own, and
// binding /usr/lib over them left /bin/sh without its loader.
XTEST(Confine, TheHostsProgramAndLibraryDirectoriesAreWhereTheHostsResolve,
      .area = "subos", .covers = {"SANDBOX-HOST-USERLAND"}) {
    const xlings::subos::HomeView home{"/h"};
    auto mounts_on = [&](std::set<std::string_view> present, std::map<std::string_view, std::string> links) {
        sp::Request r;
        r.instance = "box";
        r.instance_dir = "/h/subos/box";
        r.user = "u";
        r.argv = {"sh"};
        r.host_exists = [present](std::string_view p) {
            return (!p.starts_with("/lib") && !p.starts_with("/bin") && !p.starts_with("/sbin")) || present.contains(p);
        };
        r.host_resolve = [links](std::string_view p) {
            auto it = links.find(p);
            return it == links.end() ? std::string(p) : it->second;
        };
        auto s = cf::compile(xlings::subos::policy::legacy(), home, linux_host(true, false, 0), r);
        EXPECT_TRUE(s);
        std::map<std::string, std::string> top;
        if (s)
            for (const auto& m : s->mounts)
                if (m.kind == sp::MountKind::RoBind && (m.dst.starts_with("/lib") || m.dst == "/bin" || m.dst == "/sbin"))
                    top[m.dst] = m.src;
        return top;
    };
    const auto merged = mounts_on({"/bin", "/sbin", "/lib", "/lib64"},
                                  {{"/bin", "/usr/bin"}, {"/sbin", "/usr/sbin"}, {"/lib", "/usr/lib"},
                                   {"/lib64", "/usr/lib64"}});
    EXPECT_EQ(merged, (std::map<std::string, std::string>{
                          {"/bin", "/usr/bin"}, {"/sbin", "/usr/sbin"}, {"/lib", "/usr/lib"}, {"/lib64", "/usr/lib64"}}));
    const auto alpine = mounts_on({"/bin", "/sbin", "/lib"}, {});
    EXPECT_EQ(alpine, (std::map<std::string, std::string>{{"/bin", "/bin"}, {"/sbin", "/sbin"}, {"/lib", "/lib"}}));
    const auto leap = mounts_on({"/bin", "/sbin", "/lib", "/lib64"}, {});
    EXPECT_EQ(leap.at("/lib64"), "/lib64") << "Leap 15's loader is in its own /lib64";
}

// A device mounted in (--mount /dev/kvm) is usable inside: a plain bind is
// nodev in the sandbox's mount namespace, and opening the node failed.
XTEST(Confine, AMountedDeviceNodeIsBoundWithItsDevicesUsable, .area = "subos", .covers = {"ISO-MOUNT"}) {
    namespace policy = xlings::subos::policy;
    sp::Request r;
    r.instance = "box";
    r.instance_dir = "/h/subos/box";
    r.user = "u";
    r.argv = {"make"};
    r.host_exists = [](std::string_view) { return true; };
    r.host_device = [](std::string_view p) { return p == "/dev/kvm"; };
    const xlings::subos::HomeView home{"/h"};
    auto p = policy::legacy();
    p.mounts = {{.src = "/dev/kvm"}, {.src = "/data"}, {.src = "/ro", .rw = false}};
    auto s = cf::compile(p, home, linux_host(true, false, 0), r);
    ASSERT_TRUE(s);
    const auto kind_of = [&](std::string_view dst) -> std::optional<sp::MountKind> {
        for (const auto& m : s->mounts) if (m.dst == dst) return m.kind;
        return std::nullopt;
    };
    EXPECT_EQ(kind_of("/dev/kvm"), sp::MountKind::DevBind);
    EXPECT_EQ(kind_of("/data"), sp::MountKind::Bind) << "a directory stays a plain bind";
    EXPECT_EQ(kind_of("/ro"), sp::MountKind::RoBind);
    const auto argv = cf::launch_argv(*s, std::nullopt, "/self/xlings");
    const auto at = std::ranges::find(argv, "--dev-bind");
    ASSERT_NE(at, argv.end());
    EXPECT_EQ(*(at + 1), "/dev/kvm");
}
