// The policy model (C14): presets, the file (fail closed), tighten-only
// overrides, the one decide(), and what the compiler makes of a preset.
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

namespace pol = xlings::subos::policy;
namespace sp = xlings::subos::spec;
namespace caps = xlings::subos::caps;
using xlings::subos::HomeView;

namespace {

caps::Caps bwrap_host(bool pasta = false) {
    caps::Caps c;
    c.platform = "linux";
    c.bwrap = caps::Backend{ .name = "bwrap", .bin = "/b/bwrap", .source = "payload", .usable = true };
    if (pasta) c.pasta = "/usr/bin/pasta";
    return c;
}

sp::Request req() {
    return sp::Request{ .instance = "box", .instance_dir = "/h/subos/box", .user = "alice",
                        .argv = {"/bin/sh", "-c", "true"},
                        .host_env = {{"LANG", "de_DE.UTF-8"}, {"LC_TIME", "de_DE"}},
                        .host_exists = [](std::string_view) { return true; } };
}

}  // namespace

XTEST(SubosPolicy, PresetsSayWhatTheDesignSays,
      .area = "subos", .covers = {"POL-PRESETS"}) {
    auto dev = pol::preset(pol::Preset::Dev);
    EXPECT_EQ(dev.net, pol::Net::Host);
    EXPECT_EQ(dev.fetch, pol::Fetch::Auto);
    EXPECT_EQ(dev.identity, pol::Identity::Host);

    auto priv = pol::preset(pol::Preset::Private);
    EXPECT_EQ(priv.net, pol::Net::Nat);
    EXPECT_EQ(priv.fetch, pol::Fetch::Ask);
    EXPECT_EQ(priv.identity, pol::Identity::Neutral);
    EXPECT_EQ(priv.needs.at("net"), pol::Need::Must);

    auto locked = pol::preset(pol::Preset::Locked);
    EXPECT_EQ(locked.net, pol::Net::None);
    EXPECT_EQ(locked.fetch, pol::Fetch::Deny);
    EXPECT_EQ(locked.observe, pol::Observe::Full);
    EXPECT_TRUE(locked.disable_userns);
    EXPECT_TRUE(locked.mounts_ro_default);
    EXPECT_TRUE(locked.grants_allowed.empty());
}

XTEST(SubosPolicy, TheFileFailsClosedOnWhatItDoesNotKnow,
      .area = "subos", .covers = {"POL-UNKNOWN-REFUSED"}) {
    EXPECT_FALSE(pol::from_json(nlohmann::json::parse(R"({"isolation":{"net":"vpn"}})")));
    EXPECT_FALSE(pol::from_json(nlohmann::json::parse(R"({"isolation":{"seccomp":"off"}})")));
    EXPECT_FALSE(pol::from_json(nlohmann::json::parse(R"({"extends":"paranoid"})")));
    EXPECT_FALSE(pol::from_json(nlohmann::json::parse(R"({"permissions":{"exec":"any"}})")));
    EXPECT_FALSE(pol::from_json(nlohmann::json::parse(R"({"isolation":{"grants":["root"]}})")));
    auto err = pol::from_json(nlohmann::json::parse(R"({"sandbox":{"x":1}})"));
    ASSERT_FALSE(err);
    EXPECT_NE(err.error().find("unknown field 'sandbox'"), std::string::npos) << err.error();
    // Free text is free.
    EXPECT_TRUE(pol::from_json(nlohmann::json::parse(R"({"comment":"x","x-team":"y","extends":"dev"})")));
}

XTEST(SubosPolicy, TheFileRoundTrips,
      .area = "subos", .covers = {"POL-PRESETS"}) {
    auto doc = nlohmann::json::parse(R"({
      "extends": "private",
      "isolation": {"net": "none", "identity": {"tz": "Asia/Shanghai"}, "grants_allowed": ["gpu"]},
      "mounts": [{"src": "/work", "dst": "/work", "mode": "ro"}],
      "permissions": {"fetch": {"default": "ask",
                                "rules": [{"match": "xim:*", "action": "auto"},
                                          {"match": "*", "size_gt": "2GB", "action": "deny"}]},
                      "index_update": "deny"},
      "observe": {"level": "full"}})");
    auto p = pol::from_json(doc);
    ASSERT_TRUE(p) << p.error();
    EXPECT_EQ(p->preset, pol::Preset::Private);
    EXPECT_EQ(p->net, pol::Net::None);
    EXPECT_EQ(p->tz, "Asia/Shanghai");
    ASSERT_EQ(p->fetch_rules.size(), 2u);
    EXPECT_EQ(p->fetch_rules[1].size_gt, 2ull << 30);
    auto again = pol::from_json(pol::to_json(*p));
    ASSERT_TRUE(again) << again.error();
    EXPECT_TRUE(pol::diff(*p, *again).empty());
}

XTEST(SubosPolicy, ACallMayOnlyTighten,
      .area = "subos", .covers = {"POL-SANDBOX-ALIAS"}) {
    auto priv = pol::preset(pol::Preset::Private);
    EXPECT_FALSE(pol::apply(priv, {.net = pol::Net::Host}));
    EXPECT_TRUE(pol::apply(priv, {.net = pol::Net::None}));
    EXPECT_FALSE(pol::apply(priv, {.fetch = pol::Fetch::Auto}));
    EXPECT_TRUE(pol::apply(priv, {.allow = {"gpu"}}));          // in grants_allowed
    auto locked = pol::preset(pol::Preset::Locked);
    EXPECT_FALSE(pol::apply(locked, {.allow = {"gpu"}}));       // grants_allowed is empty
    EXPECT_FALSE(pol::apply(locked, {.allow = {"root"}}));      // not a grant at all
}

XTEST(SubosPolicy, OneDecideAnswersFetchGrantsAndChanges,
      .area = "subos", .covers = {"POL-DECIDE", "POL-RULES"}) {
    auto p = *pol::from_json(nlohmann::json::parse(R"({"extends":"private",
        "permissions":{"fetch":{"default":"ask","rules":[
          {"match":"xim:*","index":"official","action":"auto"},
          {"match":"*","size_gt":"1GB","action":"deny"}]}}})"));
    EXPECT_EQ(pol::decide(p, {.kind = "fetch", .target = "xim:gcc", .index = "official"}).action,
              pol::Action::Allow);
    EXPECT_EQ(pol::decide(p, {.kind = "fetch", .target = "xim:gcc", .index = "mirror"}).action,
              pol::Action::Ask);                                      // rule 1 needs the index
    EXPECT_EQ(pol::decide(p, {.kind = "fetch", .target = "big", .size = 2ull << 30}).action,
              pol::Action::Deny);
    auto d = pol::decide(p, {.kind = "fetch", .target = "x", .instance = "box"});
    EXPECT_EQ(d.action, pol::Action::Ask);
    EXPECT_EQ(d.owner_command, "xlings install x --subos box");
    auto inside = pol::decide(p, {.kind = "policy_change", .from_inside = true, .instance = "box"});
    EXPECT_EQ(inside.action, pol::Action::Deny);
    EXPECT_EQ(pol::decide(p, {.kind = "policy_change"}).action, pol::Action::Allow);
    EXPECT_TRUE(pol::glob_match("xim:*", "xim:gcc"));
    EXPECT_FALSE(pol::glob_match("xim:*", "community:gcc"));
}

XTEST(SubosPolicy, LockedCompilesToNoNetworkANeutralIdentityAndNoNestedNamespaces,
      .area = "subos", .covers = {"POL-PRESETS", "ISO-SPEC-GOLDEN"}) {
    auto s = sp::compile(pol::preset(pol::Preset::Locked), HomeView{"/h"}, bwrap_host(), req());
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->unshare_net);
    EXPECT_TRUE(s->disable_userns);
    EXPECT_TRUE(s->unshare_user);
    EXPECT_EQ(s->hostname, "box");
    EXPECT_EQ(s->env.at("USER"), "user");
    EXPECT_EQ(s->env.at("HOME"), "/home/user");
    EXPECT_EQ(s->env.at("TZ"), "UTC");
    EXPECT_EQ(s->env.at("LANG"), "C.UTF-8");
    EXPECT_FALSE(s->env.contains("LC_TIME"));
    auto a = xlings::subos::provider::bwrap_argv(*s);
    EXPECT_NE(std::ranges::find(a, "--unshare-net"), a.end());
    EXPECT_NE(std::ranges::find(a, "/h/subos/box/etc-neutral/passwd"), a.end());
    EXPECT_EQ(std::ranges::find(a, "/etc/localtime"), a.end()) << "the host's zone stays outside";
}

XTEST(SubosPolicy, AMustThisHostCannotMeetRefusesAndAShouldDegrades,
      .area = "subos", .covers = {"ISO-MUST-SHOULD", "ISO-NO-DEGRADE"}) {
    // private needs a private network; without pasta there is none to give.
    auto r = sp::compile(pol::preset(pol::Preset::Private), HomeView{"/h"}, bwrap_host(), req());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().missing.front().dimension, "net");
    EXPECT_NE(r.error().missing.front().fix.find("passt"), std::string::npos);
    // With pasta, it enters.
    EXPECT_TRUE(sp::compile(pol::preset(pol::Preset::Private), HomeView{"/h"}, bwrap_host(true), req()));

    // proot under dev: entered, degraded, and says what.
    caps::Caps proot;
    proot.platform = "linux";
    proot.proot = caps::Backend{ .name = "proot", .bin = "/p", .source = "payload", .usable = true };
    auto dev = sp::compile(pol::preset(pol::Preset::Dev), HomeView{"/h"}, proot, req());
    ASSERT_TRUE(dev.has_value());
    EXPECT_FALSE(dev->degraded.empty());
    // ...and with --no-degrade, refused.
    auto strict = pol::preset(pol::Preset::Dev);
    strict.no_degrade = true;
    EXPECT_FALSE(sp::compile(strict, HomeView{"/h"}, proot, req()));
}

XTEST(SubosPolicy, NatOpensNothingUnlessPublishedOrGranted,
      .area = "subos", .covers = {"ISO-NET-NAT", "ISO-GRANTS"}) {
    auto r = req();
    auto s = sp::compile(pol::preset(pol::Preset::Private), HomeView{"/h"}, bwrap_host(true), r);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->net_nat);
    auto a = xlings::subos::provider::pasta_args(*s);
    auto has = [&](std::vector<std::string> seq) {
        return std::ranges::search(a, seq).begin() != a.end();
    };
    EXPECT_TRUE(has({"-t", "none"}));
    EXPECT_TRUE(has({"-T", "none"}));
    EXPECT_TRUE(has({"--no-map-gw"}));
    // bwrap must not make a second network namespace: it runs in pasta's.
    auto b = xlings::subos::provider::bwrap_argv(*s);
    EXPECT_EQ(std::ranges::find(b, "--unshare-net"), b.end());

    r.publish = {"18080:80"};
    r.grants = {"host-loopback"};
    auto p = sp::compile(pol::preset(pol::Preset::Private), HomeView{"/h"}, bwrap_host(true), r);
    ASSERT_TRUE(p.has_value());
    auto pa = xlings::subos::provider::pasta_args(*p);
    EXPECT_NE(std::ranges::search(pa, std::vector<std::string>{"-t", "18080:80"}).begin(), pa.end());
    EXPECT_EQ(std::ranges::find(pa, "--no-map-gw"), pa.end());

    // --publish without nat is a degradation dev reports, not silence.
    auto d = sp::compile(pol::preset(pol::Preset::Dev), HomeView{"/h"}, bwrap_host(true), r);
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(std::ranges::any_of(d->degraded, [](auto& u) { return u.dimension == "publish"; }));
}
