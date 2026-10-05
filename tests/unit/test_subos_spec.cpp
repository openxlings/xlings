// The spec compiler and its providers (C8): Policy + HomeView + Caps ->
// SandboxSpec -> argv. Pure data, so isolation is checked here without a
// sandbox. The Legacy goldens are the argv the sandbox code produced before
// the compiler existed, byte for byte: introducing the compiler changes no
// behaviour, and every later change to a default shows up as a diff here.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.caps;
import xlings.subos.spec;
import xlings.subos.provider;
import xlings.subos.gates;

namespace sp = xlings::subos::spec;
namespace pv = xlings::subos::provider;
namespace pol = xlings::subos::policy;
namespace caps = xlings::subos::caps;
namespace gates = xlings::subos::gates;
using xlings::subos::HomeView;
using V = std::vector<std::string>;

namespace {

const HomeView kHome{ .home = "/h" };

caps::Caps linux_caps(bool bwrap_ok = true, bool proot = true) {
    caps::Caps c;
    c.platform = "linux";
    c.bwrap = caps::Backend{ .name = "bwrap", .bin = "/h/data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap",
                             .source = "payload", .usable = bwrap_ok,
                             .probe_output = bwrap_ok ? "" : "setting up uid map: Permission denied" };
    if (proot) c.proot = caps::Backend{ .name = "proot", .bin = "/h/data/xpkgs/xim-x-proot/5.4.0/bin/proot",
                                        .source = "payload", .usable = true };
    c.userns = bwrap_ok;
    return c;
}

// A host with every userland path but no GPU.
sp::Request request(V argv = {}) {
    return sp::Request{
        .instance = "box",
        .instance_dir = "/h/subos/box",
        .user = "u",
        .argv = std::move(argv),
        .shell = "/bin/bash",
        .host_exists = [](std::string_view p) {
            return !p.starts_with("/dev/nvidia") && p != "/dev/dri" && p != "/dev/dxg"
                && p != "/etc/pki";
        },
    };
}

const V kLegacyBinds{
    "--ro-bind", "/usr", "/usr",
    "--ro-bind", "/bin", "/bin",
    "--ro-bind", "/usr/lib", "/lib",
    "--ro-bind", "/usr/lib64", "/lib64",
    "--ro-bind", "/etc/resolv.conf", "/etc/resolv.conf",
    "--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache",
    "--ro-bind", "/etc/ssl", "/etc/ssl",
    "--ro-bind", "/etc/alternatives", "/etc/alternatives",
    "--ro-bind", "/etc/localtime", "/etc/localtime",
};

V concat(std::initializer_list<V> parts) {
    V out;
    for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

}  // namespace

XTEST(SubosSpec, LegacyBwrapArgvIsTheOldOnePlusTheS0Fixes,
      .area = "subos", .covers = {"ISO-SPEC-GOLDEN", "COMPAT-UNDECLARED"}) {
    auto spec = sp::compile(pol::legacy(), kHome, linux_caps(), request({"/bin/bash", "-c", "true"}));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->backend, sp::Backend::Bwrap);
    auto expected = concat({
        {"/h/data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap",
         "--unshare-pid", "--unshare-ipc", "--unshare-uts", "--die-with-parent", "--new-session",
         "--dev", "/dev", "--proc", "/proc"},
        kLegacyBinds,
        {"--bind", "/h/subos/box/home", "/home",
         "--bind", "/h/subos/box/tmp", "/tmp",
         // S0 (#640 F1, F6): the home read-only, only this instance writable,
         // other instances, the audit, sockets and host facts covered.
         "--ro-bind", "/h", "/h",
         "--tmpfs", "/h/subos",
         "--bind", "/h/subos/box", "/h/subos/box",
         "--tmpfs", "/h/logs",
         "--tmpfs", "/h/run",
         "--tmpfs", "/h/state",
         "--bind", "/h/subos/box/etc/passwd", "/etc/passwd",
         "--bind", "/h/subos/box/etc/group", "/etc/group",
         "--bind", "/h/subos/box/etc/hosts", "/etc/hosts",
         "--bind", "/h/subos/box/etc/nsswitch.conf", "/etc/nsswitch.conf",
         "--chdir", "/home/u", "--", "/bin/bash", "-c", "true"},
    });
    EXPECT_EQ(pv::bwrap_argv(*spec), expected);
    EXPECT_TRUE(spec->clear_env);       // S0: an allow-list, not inheritance
    EXPECT_EQ(spec->env.at("HOME"), "/home/u");
    EXPECT_EQ(spec->env.at("PATH"), "/h/subos/box/bin:/h/bin:/usr/local/bin:/usr/bin:/bin:/run/xlings");
    EXPECT_EQ(spec->env.at("XLINGS_SUBOS_MODE"), "sandbox");
}

XTEST(SubosSpec, LegacyInteractiveShellGetsDashIOnlyOnATerminal,
      .area = "subos", .covers = {"ISO-SPEC-GOLDEN"}) {
    auto r = request();
    r.interactive = true;
    auto spec = sp::compile(pol::legacy(), kHome, linux_caps(), r);
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->argv, (V{"/bin/bash", "-i"}));
    // A terminal keeps its job control: no new session, the seccomp filter instead.
    EXPECT_FALSE(spec->new_session);
    EXPECT_TRUE(spec->block_tiocsti);
    auto a = pv::bwrap_argv(*spec, 7);
    EXPECT_NE(std::ranges::search(a, V{"--seccomp", "7"}).begin(), a.end());
    r.interactive = false;
    EXPECT_EQ(sp::compile(pol::legacy(), kHome, linux_caps(), r)->argv, (V{"/bin/bash"}));
}

XTEST(SubosSpec, LegacyTmpfsAndImageStorage,
      .area = "subos", .covers = {"ISO-SPEC-GOLDEN"}) {
    auto r = request({"/bin/bash", "-c", "true"});
    r.storage = sp::Storage::Tmpfs;
    auto t = pv::bwrap_argv(*sp::compile(pol::legacy(), kHome, linux_caps(), r));
    const V tail{"--tmpfs", "/home/u", "--tmpfs", "/tmp",
                 "--chdir", "/home/u", "--", "/bin/bash", "-c", "true"};
    ASSERT_GE(t.size(), tail.size());
    EXPECT_EQ(V(t.end() - static_cast<long>(tail.size()), t.end()), tail);
    EXPECT_EQ(std::ranges::find(t, "/h/subos/box/home"), t.end())
        << "tmpfs storage must not bind the shared home";

    r.storage = sp::Storage::Image;
    r.image_mountpoint = "/h/subos/box/.mountpoint";
    auto i = pv::bwrap_argv(*sp::compile(pol::legacy(), kHome, linux_caps(), r));
    EXPECT_NE(std::ranges::search(i, V{"--bind", "/h/subos/box/.mountpoint", "/home"}).begin(), i.end());
    EXPECT_NE(std::ranges::search(i, V{"--tmpfs", "/tmp"}).begin(), i.end());
}

XTEST(SubosSpec, LegacyProotArgvIsUnchanged,
      .area = "subos", .covers = {"ISO-SPEC-GOLDEN"}) {
    auto spec = sp::compile(pol::legacy(), kHome, linux_caps(/*bwrap_ok=*/false),
                            request({"/bin/bash", "-c", "true"}));
    ASSERT_TRUE(spec.has_value());
    EXPECT_EQ(spec->backend, sp::Backend::Proot);
    V expected{"/h/data/xpkgs/xim-x-proot/5.4.0/bin/proot", "-r", "/h/subos/box/sandbox-root",
               "--bind=/proc:/proc", "--bind=/sys:/sys", "--bind=/dev:/dev",
               "--bind=/usr:/usr", "--bind=/bin:/bin", "--bind=/usr/lib:/lib",
               "--bind=/usr/lib64:/lib64", "--bind=/etc/resolv.conf:/etc/resolv.conf",
               "--bind=/etc/ld.so.cache:/etc/ld.so.cache", "--bind=/etc/ssl:/etc/ssl",
               "--bind=/etc/alternatives:/etc/alternatives", "--bind=/etc/localtime:/etc/localtime",
               "--bind=/h/subos/box/home:/home", "--bind=/h/subos/box/tmp:/tmp", "--bind=/h:/h",
               "--bind=/h/subos/box/etc/passwd:/etc/passwd", "--bind=/h/subos/box/etc/group:/etc/group",
               "--bind=/h/subos/box/etc/hosts:/etc/hosts",
               "--bind=/h/subos/box/etc/nsswitch.conf:/etc/nsswitch.conf",
               "--cwd=/home/u", "/bin/bash", "-c", "true"};
    EXPECT_EQ(pv::proot_argv(*spec), expected);
    EXPECT_EQ(spec->env.at("PROOT_NO_SECCOMP"), "1");
}

XTEST(SubosSpec, BackendRefusalsSayWhatIsMissingAndHowToFixIt,
      .area = "subos", .covers = {"ISO-MUST-SHOULD"}) {
    caps::Caps none;
    none.platform = "linux";
    auto r = sp::compile(pol::legacy(), kHome, none, request());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().missing.front().dimension, "backend");
    EXPECT_EQ(r.error().missing.front().need, pol::Need::Must);

    auto req = request();
    req.preferred = sp::Backend::Bwrap;
    auto b = sp::compile(pol::legacy(), kHome, linux_caps(/*bwrap_ok=*/false), req);
    ASSERT_FALSE(b.has_value());
    EXPECT_NE(b.error().missing.front().reason.find("uid map"), std::string::npos)
        << "the probe's own words, not a guess";

    auto tmpfs = request();
    tmpfs.storage = sp::Storage::Tmpfs;
    auto t = sp::compile(pol::legacy(), kHome, linux_caps(/*bwrap_ok=*/false), tmpfs);
    ASSERT_FALSE(t.has_value());
    EXPECT_EQ(t.error().missing.front().dimension, "storage");
}

XTEST(SubosSpec, GpuGrantAddsOnlyTheDevicesThatExist,
      .area = "subos", .covers = {"ISO-GRANTS"}) {
    auto r = request();
    r.grants.insert("gpu");
    r.host_exists = [](std::string_view p) { return p == "/dev/nvidiactl" || p == "/dev/dri" || p == "/usr"; };
    auto a = pv::bwrap_argv(*sp::compile(pol::legacy(), kHome, linux_caps(), r));
    EXPECT_NE(std::ranges::search(a, V{"--dev-bind", "/dev/nvidiactl", "/dev/nvidiactl"}).begin(), a.end());
    EXPECT_NE(std::ranges::search(a, V{"--dev-bind", "/dev/dri", "/dev/dri"}).begin(), a.end());
    EXPECT_EQ(std::ranges::search(a, V{"--dev-bind", "/dev/nvidia0", "/dev/nvidia0"}).begin(), a.end());
    EXPECT_NE(std::ranges::search(a, V{"--ro-bind", "/sys", "/sys"}).begin(), a.end());
}

XTEST(SubosSpec, HomeRedirectOnMacosAndWindows,
      .area = "subos", .covers = {"PLAT-HINT"}) {
    caps::Caps mac;
    mac.platform = "macos";
    auto m = sp::compile(pol::legacy(), kHome, mac, request());
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->backend, sp::Backend::HomeRedirect);
    EXPECT_EQ(m->env.at("HOME"), "/h/subos/box/home/u");
    EXPECT_EQ(m->env.at("TMPDIR"), "/h/subos/box/tmp");

    caps::Caps win;
    win.platform = "windows";
    auto w = sp::compile(pol::legacy(), kHome, win, request());
    ASSERT_TRUE(w.has_value());
    EXPECT_TRUE(w->env.contains("USERPROFILE"));
    EXPECT_FALSE(w->env.contains("HOME"));
}

XTEST(SubosSpec, DescribeCarriesEnvironmentNamesNeverValues,
      .area = "subos", .covers = {"OBS-REDACT"}) {
    auto r = request();
    r.host_env = {{"GITHUB_TOKEN", "ghp_secret"}, {"LANG", "C.UTF-8"}};
    auto p = pol::legacy();
    p.env_inherit = false;
    auto spec = sp::compile(p, kHome, linux_caps(), r);
    ASSERT_TRUE(spec.has_value());
    auto text = spec->describe().dump();
    EXPECT_EQ(text.find("ghp_secret"), std::string::npos);
    EXPECT_NE(text.find("LANG"), std::string::npos);
    EXPECT_FALSE(spec->env.contains("GITHUB_TOKEN"));
}

XTEST(SubosSpec, ThePlatformMatrixIsMeasured,
      .area = "subos", .covers = {"PLAT-STATUS-MATRIX"}) {
    auto find = [](const std::vector<gates::Status>& v, std::string_view g) {
        return *std::ranges::find(v, g, &gates::Status::gate);
    };
    auto ok = gates::probe(linux_caps());
    EXPECT_EQ(find(ok, "FsGate").enforced, gates::Enforced::Kernel);
    auto proot_only = gates::probe(linux_caps(/*bwrap_ok=*/false));
    EXPECT_EQ(find(proot_only, "FsGate").enforced, gates::Enforced::Advisory);
    caps::Caps mac;
    mac.platform = "macos";
    auto m = gates::probe(mac);
    EXPECT_EQ(m.size(), 8u);
    EXPECT_EQ(find(m, "FsGate").enforced, gates::Enforced::Advisory);
    EXPECT_FALSE(find(m, "NetGate").supported);
    EXPECT_FALSE(find(m, "NetGate").route.empty());
}

XTEST(SubosSpec, TheEnvironmentIsAnAllowList,
      .area = "subos", .covers = {"F3"}) {
    auto r = request();
    r.host_env = {{"GITHUB_TOKEN", "x"}, {"SSH_AUTH_SOCK", "/run/a"}, {"XAUTHORITY", "/x"},
                  {"DBUS_SESSION_BUS_ADDRESS", "unix:"}, {"LANG", "C.UTF-8"}, {"LC_ALL", "C"},
                  {"EDITOR", "vi"}, {"https_proxy", "http://p"}, {"XLINGS_AGENT_MODE", "1"}};
    auto spec = sp::compile(pol::legacy(), kHome, linux_caps(), r);
    ASSERT_TRUE(spec.has_value());
    for (auto k : {"GITHUB_TOKEN", "SSH_AUTH_SOCK", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS"})
        EXPECT_FALSE(spec->env.contains(k)) << k;
    for (auto k : {"LANG", "LC_ALL", "EDITOR", "https_proxy", "XLINGS_AGENT_MODE"})
        EXPECT_TRUE(spec->env.contains(k)) << k;
    EXPECT_EQ(spec->env.at("USER"), "u");
}

XTEST(SubosSpec, ProotSaysWhatItCannotIsolate,
      .area = "subos", .covers = {"ISO-MUST-SHOULD"}) {
    auto spec = sp::compile(pol::legacy(), kHome, linux_caps(/*bwrap_ok=*/false), request());
    ASSERT_TRUE(spec.has_value());
    EXPECT_FALSE(spec->unshare_pid);
    EXPECT_FALSE(spec->degraded.empty());
}

// ── Landlock (C18): a write fence without namespaces ─────────────────

XTEST(SubosSpec, LandlockIsAskedForAndNeedsTheKernel,
      .area = "subos", .covers = {"ISO-LANDLOCK"}) {
    auto c = linux_caps(false, false);
    auto r = request({"make"});
    r.preferred = sp::Backend::Landlock;
    auto none = sp::compile(pol::preset(pol::Preset::Dev), kHome, c, r);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().missing[0].dimension, "backend");
    EXPECT_NE(none.error().missing[0].reason.find("Landlock"), std::string::npos);

    // Not chosen on its own: an unusable bwrap and no proot is still a refusal.
    c.landlock_abi = 3;
    EXPECT_FALSE(sp::compile(pol::preset(pol::Preset::Dev), kHome, c, request({"make"})));
}

XTEST(SubosSpec, LandlockWritesOnlyTheInstanceAndWhatIsMountedReadWrite,
      .area = "subos", .covers = {"ISO-LANDLOCK"}) {
    auto c = linux_caps(false, false);
    c.landlock_abi = 3;
    auto r = request({"make"});
    r.preferred = sp::Backend::Landlock;
    auto policy = pol::preset(pol::Preset::Dev);
    policy.mounts.push_back({.src = "/work", .dst = "", .rw = true});
    policy.mounts.push_back({.src = "/ref", .dst = "", .rw = false});
    auto s = sp::compile(policy, kHome, c, r);
    ASSERT_TRUE(s) << s.error().missing[0].reason;
    EXPECT_EQ(s->backend, sp::Backend::Landlock);
    EXPECT_TRUE(s->mounts.empty()) << "no view: nothing is mounted";
    std::vector<std::string> rw;
    for (auto& p : s->landlock_rw) rw.push_back(p.generic_string());
    EXPECT_EQ(rw, (V{"/h/subos/box", "/dev", "/proc", "/work"})) << "never the host's /tmp, never /ref";
    EXPECT_EQ(s->env.at("HOME"), "/h/subos/box/home/u");
    EXPECT_EQ(s->env.at("TMPDIR"), "/h/subos/box/tmp");
    EXPECT_EQ(s->env.at("XLINGS_BROKER_SOCKET"), kHome.broker_socket("box").generic_string());
    EXPECT_EQ(s->argv, V{"make"});
    // What it does not give is said, not implied.
    std::set<std::string> degraded;
    for (auto& d : s->degraded) degraded.insert(d.dimension);
    EXPECT_TRUE(degraded.contains("fs") && degraded.contains("pid")) << s->describe().dump();
    // session-init reads the fence from its environment.
    auto env = pv::process_env(*s, {});
    EXPECT_EQ(env.at("XLINGS_SESSION_LANDLOCK_RW"), "/h/subos/box\n/dev\n/proc\n/work\n");

    // A mapping elsewhere has no meaning without a view.
    policy.mounts = {{.src = "/work", .dst = "/w", .rw = true}};
    EXPECT_FALSE(sp::compile(policy, kHome, c, r));
    // What private requires, Landlock cannot give.
    EXPECT_FALSE(sp::compile(pol::preset(pol::Preset::Private), kHome, c, r));
}
