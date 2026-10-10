// An environment's edition layer and the plan that moves it to a newer
// edition (Luban OS design part 2 §3.2, §3.3); a policy an upgrade applies
// only when it allows nothing more.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.subos.edition;
import xlings.subos.policy;

namespace ed = xlings::subos::edition;
namespace policy = xlings::subos::policy;

namespace {

ed::Record core_1() {
    return ed::Record{.ref = "subos:luban-core@2026.10.10.1",
                      .chain = {"subos:luban-tiny@2026.10.10.1"},
                      .packages = {{"xim:gcc", "16.1.0"}, {"xim:openssl", "3.1.5"}, {"xim:claude", "2.1.281"},
                                   {"xim:vim", "8.1.1045"}},
                      .policy = "xim:agent-private@2026.10.10.1"};
}

bool has(const std::vector<ed::Step>& steps, std::string_view key) {
    return std::ranges::any_of(steps, [&](const ed::Step& s) { return s.key == key; });
}

}  // namespace

XTEST(SubosEdition, TheRecordRoundTripsAndAnInstanceWithoutOneHasNone, .area = "luban",
      .covers = {"LUBAN-EDITION-RECORD"}) {
    const auto r = core_1();
    const auto back = ed::read(nlohmann::json{{"edition", ed::to_json(r)}});
    ASSERT_TRUE(back);
    EXPECT_EQ(back->ref, r.ref);
    EXPECT_EQ(back->chain, r.chain);
    EXPECT_EQ(back->packages, r.packages);
    EXPECT_EQ(back->policy, r.policy);
    EXPECT_FALSE(ed::read(nlohmann::json{{"kind", "rootfs"}}));
    EXPECT_EQ(ed::split("xim:gcc@16.1.0"), (std::pair<std::string, std::string>{"xim:gcc", "16.1.0"}));
    EXPECT_EQ(ed::split("xim:claude"), (std::pair<std::string, std::string>{"xim:claude", ""}));
}

XTEST(SubosEdition, AnUpgradeMovesWhatTheEditionBroughtAndLeavesWhatTheUserMoved, .area = "luban",
      .covers = {"LUBAN-UPGRADE"}) {
    // The user moved gcc to 16.2.0 and installed cmake; the new edition has
    // openssl 3.5.1, gcc 16.1.1, the newest claude, ninja; vim is gone.
    const std::vector<std::string> configured{"xim:gcc@16.1.0", "xim:gcc@16.2.0", "xim:openssl@3.1.5",
                                              "xim:claude@2.1.281", "xim:vim@8.1.1045", "xim:cmake@3.31.0"};
    const std::vector<std::string> next{"xim:gcc@16.1.1", "xim:openssl@3.5.1", "xim:claude", "xim:ninja@1.12.1"};
    const auto plan = ed::plan(core_1(), next, configured);
    EXPECT_TRUE(has(plan.upgrade, "xim:openssl"));
    EXPECT_TRUE(has(plan.upgrade, "xim:claude")) << "an unpinned package follows its newest";
    EXPECT_TRUE(has(plan.kept, "xim:gcc")) << "the user moved it: theirs";
    EXPECT_FALSE(has(plan.upgrade, "xim:gcc"));
    EXPECT_TRUE(has(plan.add, "xim:ninja"));
    EXPECT_EQ(plan.dropped, std::vector<std::string>{"xim:vim"}) << "left installed, said";
    EXPECT_FALSE(has(plan.upgrade, "xim:cmake")) << "the user's own package is not the edition's";
    // Already at the edition's pins: nothing to do but the unpinned one.
    const auto same = ed::plan(core_1(), {"xim:gcc@16.1.0", "xim:openssl@3.1.5", "xim:vim@8.1.1045"},
                               {"xim:gcc@16.1.0", "xim:openssl@3.1.5", "xim:vim@8.1.1045"});
    EXPECT_TRUE(same.empty());
}

XTEST(SubosEdition, APolicyThatAllowsMoreIsLooserAndOneThatAllowsLessIsNot, .area = "luban",
      .covers = {"LUBAN-UPGRADE-NEVER-LOOSENS"}) {
    const auto locked = policy::preset(policy::Preset::Locked);
    const auto priv = policy::preset(policy::Preset::Private);
    EXPECT_TRUE(policy::loosened(priv, priv).empty());
    EXPECT_TRUE(policy::loosened(priv, locked).empty()) << "locked allows nothing private does not";
    const auto looser = policy::loosened(locked, priv);
    EXPECT_FALSE(looser.empty());
    EXPECT_TRUE(std::ranges::any_of(looser, [](const std::string& l) { return l.starts_with("net "); }));
    auto granted = priv;
    granted.grants.insert("gpu");
    EXPECT_EQ(policy::loosened(priv, granted), std::vector<std::string>{"grant + gpu"});
    auto nested = locked;
    nested.disable_userns = false;
    EXPECT_EQ(policy::loosened(locked, nested), std::vector<std::string>{"nested user namespaces allowed"});
}
