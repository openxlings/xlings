// INTENT-EQ (SubOS design part 3 §6): the sandbox a policy compiles to is a
// golden, recorded from the compiler before the Intent IR and the confine
// registry replaced its insides. Every case below must produce the same
// description, the same backend argv and the same environment, byte for byte.
//
// XLINGS_INTENT_GOLDEN_WRITE=1 rewrites the golden; a change to it is a
// change to what a sandbox IS and is reviewed as one.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.caps;
import xlings.subos.spec;
import xlings.subos.provider;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace pol = xlings::subos::policy;
namespace sp = xlings::subos::spec;
namespace caps = xlings::subos::caps;
namespace pv = xlings::subos::provider;
using xlings::subos::HomeView;

namespace {

fs::path golden_path() {
    return fs::path(__FILE__).parent_path().parent_path() / "fixtures/intent-eq/golden.tsv";
}

caps::Caps host(int which) {
    caps::Caps c;
    c.platform = "linux";
    c.kernel = "6.8.0";
    switch (which) {
    case 0:   // everything
        c.bwrap = caps::Backend{.name = "bwrap", .bin = "/opt/bwrap", .source = "root-owned", .usable = true};
        c.proot = caps::Backend{.name = "proot", .bin = "/opt/proot", .source = "payload", .usable = true};
        c.userns = true;
        c.pasta = fs::path("/opt/pasta");
        c.seccomp = true;
        c.landlock_abi = 3;
        break;
    case 1:   // bwrap found but cannot make a namespace; proot is there
        c.bwrap = caps::Backend{.name = "bwrap", .bin = "/usr/bin/bwrap", .source = "system", .usable = false,
                                .probe_output = "bwrap: setting up uid map: Permission denied\n"};
        c.proot = caps::Backend{.name = "proot", .bin = "/opt/proot", .source = "payload", .usable = true};
        c.pasta_missing = "pasta (passt) is not installed";
        break;
    case 2:   // nothing
        c.pasta_missing = "/dev/net/tun is missing on this host";
        break;
    case 3: c.platform = "macos"; break;
    default: c.platform = "windows"; break;
    }
    return c;
}

pol::Policy policy_of(int preset, int net, int extra) {
    auto p = pol::preset(static_cast<pol::Preset>(preset));
    if (net == 1) { p.net = pol::Net::Nat; }
    if (net == 2) { p.net = pol::Net::Proxy; p.proxy = "socks5h://proxy.example:1080"; }
    if (net == 3) { p.net = pol::Net::None; }
    if (extra % 3 == 1) {
        p.mounts.push_back({"/work/src", "", true, true});
        p.mounts.push_back({"/work/ref", "/ref", false, true});
        p.env_pass.push_back("EDITOR");
    }
    if (extra % 6 == 5) {
        p.mounts.push_back({"/etc", "/etc", false, true});           // refused: the sandbox's own
        p.mounts.push_back({"/work/missing", "", true, true});        // refused: absent
    }
    if (extra % 3 == 2) {
        p.disable_userns = true;
        p.needs["net"] = pol::Need::Must;
        p.env_explicit_any = false;
    }
    if (extra % 5 == 4) p.no_degrade = true;
    return p;
}

sp::Request request_of(int storage, bool interactive, int grants, bool root, std::optional<sp::Backend> preferred) {
    sp::Request r;
    r.instance = "box";
    r.instance_dir = "/h/.xlings/subos/box";
    r.user = "alice";
    r.interactive = interactive;
    r.storage = static_cast<sp::Storage>(storage);
    r.image_mountpoint = "/h/.xlings/subos/box/.img";
    r.preferred = preferred;
    if (grants == 1) r.grants = {"gpu", "display", "ssh-agent"};
    if (grants == 2) r.grants = {"audio", "dbus", "camera", "host-loopback"};
    r.host_env = {{"TERM", "xterm-256color"}, {"LANG", "en_US.UTF-8"}, {"LC_ALL", "en_US.UTF-8"},
                  {"DISPLAY", ":0"}, {"XAUTHORITY", "/h/.Xauthority"}, {"WAYLAND_DISPLAY", "wayland-0"},
                  {"XDG_RUNTIME_DIR", "/run/user/1000"}, {"SSH_AUTH_SOCK", "/tmp/ssh-agent.sock"},
                  {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus"}, {"http_proxy", "http://p:3128"},
                  {"EDITOR", "vim"}, {"SECRET_TOKEN", "do-not-pass"}};
    r.explicit_env = {{"FOO", "bar"}, {"EDITOR", "nano"}};
    if (interactive) r.argv = {}; else r.argv = {"make", "-j4"};
    if (grants == 2) r.publish = {"8080:80"};
    if (root) {
        r.root = "/h/.xlings/subos/box/rootfs";
        r.root_mounts = {{sp::MountKind::RoBind, "/h/.xlings/data/xpkgs/xim-x-glibc/2.44", "/h/.xlings/data/xpkgs/xim-x-glibc/2.44"}};
    }
    r.host_exists = [](std::string_view p) { return p.find("missing") == std::string_view::npos; };
    return r;
}

nlohmann::json outcome(const pol::Policy& p, const caps::Caps& c, const sp::Request& r) {
    const HomeView home{"/h/.xlings"};
    auto compiled = sp::compile(p, home, c, r);
    if (!compiled) {
        nlohmann::json j{{"refused", nlohmann::json::array()}};
        for (const auto& u : compiled.error().missing)
            j["refused"].push_back({{"dimension", u.dimension}, {"reason", u.reason}, {"fix", u.fix}});
        return j;
    }
    const auto& s = *compiled;
    nlohmann::json j{{"spec", s.describe()}};
    j["backend_bin"] = s.backend_bin.generic_string();
    if (s.backend == sp::Backend::Bwrap || s.backend == sp::Backend::Fake) j["argv"] = pv::bwrap_argv(s, 7);
    if (s.backend == sp::Backend::Proot) j["argv"] = pv::proot_argv(s);
    if (s.net_nat) j["pasta"] = pv::pasta_args(s);
    j["env"] = pv::process_env(s, {{"INHERITED", "1"}, {"PATH", "/usr/bin"}});
    j["landlock_rw"] = nlohmann::json::array();
    for (const auto& w : s.landlock_rw) j["landlock_rw"].push_back(w.generic_string());
    return j;
}

struct Case {
    std::string name;
    nlohmann::json result;
};

std::string summary(const nlohmann::json& result) {
    if (result.contains("refused")) {
        std::string dims;
        for (const auto& u : result["refused"]) dims += (dims.empty() ? "" : ",") + u["dimension"].get<std::string>();
        return "refused:" + dims;
    }
    return "ok:" + result["spec"]["backend"].get<std::string>();
}

std::vector<Case> cases() {
    std::vector<Case> out;
    const std::array<std::optional<sp::Backend>, 5> preferred{
        std::nullopt, sp::Backend::Bwrap, sp::Backend::Proot, sp::Backend::Landlock, sp::Backend::Fake};
    auto add = [&](std::string sweep, int preset, std::size_t pi, int h, int storage, int net, int grants,
                   int extra, bool interactive, bool root) {
        auto p = policy_of(preset, net, extra);
        auto r = request_of(storage, interactive, grants, root, preferred[pi]);
        out.push_back({std::format("{} preset{} pref{} host{} st{} net{} gr{} x{} i{} root{}", sweep, preset, pi,
                                   h, storage, net, grants, extra, interactive, root),
                       outcome(p, host(h), r)});
    };
    // A: every host and backend preference, the other axes rotating --
    // the refusals live here.
    int n = 0;
    for (int round = 0; round != 2; ++round)
        for (int preset = 0; preset != 4; ++preset)
            for (std::size_t pi = 0; pi != preferred.size(); ++pi)
                for (int h = 0; h != 5; ++h) {
                    const int k = n++ + round * 7;
                    add(std::format("A{}", round), preset, pi, h, k % 3, (k / 3) % 4, (k / 2) % 3, k / 5,
                        (k % 2) == 0, (k % 7) == round);
                }
    // B: a host with every backend, every combination of what changes the
    // shape of a sandbox.
    for (int preset = 0; preset != 4; ++preset)
        for (std::size_t pi = 0; pi != 4; ++pi)
            for (int storage = 0; storage != 3; ++storage)
                for (int net = 0; net != 4; ++net)
                    for (int interactive = 0; interactive != 2; ++interactive) {
                        const int k = preset * 31 + static_cast<int>(pi) * 7 + storage * 5 + net * 3 + interactive;
                        add("B", preset, pi, 0, storage, net, k % 3, k % 13, interactive == 1,
                            pi <= 1 && (k % 4) == 0);
                    }
    // D: the namespace-less backends (proot, Landlock) where they can run:
    // shared storage, on the full host and on the one whose bwrap fails.
    for (std::size_t pi : {std::size_t{2}, std::size_t{3}})
        for (int h = 0; h != 2; ++h)
            for (int preset = 0; preset != 4; ++preset)
                for (int net = 0; net != 4; ++net)
                    for (int grants = 0; grants != 3; ++grants) {
                        const int k = preset * 11 + net * 5 + grants * 3 + h;
                        add("D", preset, pi, h, 0, net, grants, k % 6 == 5 ? 0 : k % 5, (k % 2) == 1, false);
                    }
    // C: the home redirect of macOS and Windows.
    for (int h = 3; h != 5; ++h)
        for (int preset = 0; preset != 4; ++preset)
            for (int net = 0; net != 4; ++net)
                for (int grants = 0; grants != 3; ++grants)
                    add("C", preset, 0, h, 0, net, grants, grants * 3, grants == 1, false);
    return out;
}

}  // namespace

XTEST(IntentEq, TheCompiledSandboxIsByteIdenticalToTheRecordedGolden,
      .area = "subos", .covers = {"INTENT-EQ"}, .requires_ = {"posix"}) {
    // One line per case: its name, a summary, and the sha256 of everything
    // it compiled to (description, argv, environment). A difference prints
    // the whole of what it compiles to now.
    const auto all = cases();
    ASSERT_GE(all.size(), 500u);
    std::vector<std::string> lines;
    for (const auto& c : all)
        lines.push_back(std::format("{}\t{}\t{}", c.name, summary(c.result), xlings::sha256::hex(c.result.dump())));
    if (const char* w = std::getenv("XLINGS_INTENT_GOLDEN_WRITE"); w && std::string_view(w) == "1") {
        fs::create_directories(golden_path().parent_path());
        std::ofstream out(golden_path(), std::ios::binary);
        for (const auto& l : lines) out << l << '\n';
        GTEST_SKIP() << "golden written: " << golden_path().string();
    }
    std::ifstream in(golden_path(), std::ios::binary);
    ASSERT_TRUE(in) << "no golden at " << golden_path().string() << " (XLINGS_INTENT_GOLDEN_WRITE=1)";
    std::vector<std::string> golden;
    for (std::string l; std::getline(in, l);) golden.push_back(l);
    ASSERT_EQ(golden.size(), lines.size());
    int differing = 0;
    for (std::size_t i = 0; i != lines.size(); ++i) {
        if (lines[i] == golden[i]) continue;
        if (++differing <= 3) ADD_FAILURE() << "case differs\n  golden: " << golden[i] << "\n  now:    "
                                            << lines[i] << "\n  " << all[i].result.dump();
    }
    EXPECT_EQ(differing, 0);
}
