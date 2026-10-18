// opencaps (luban/specs/opencaps): needs and provisions by name and level;
// a name this side does not know is not met, and says so.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import opencaps;
import opencaps.conformance;

XTEST(Opencaps, ANeedIsMetByTheSameNameAtItsLevelOrHigher, .area = "luban", .covers = {"LUBAN-CAPS"}) {
    auto need = opencaps::parse_need("root-layout>=2");
    ASSERT_TRUE(need);
    EXPECT_EQ(need->name, "root-layout");
    EXPECT_EQ(need->level, 2);
    EXPECT_EQ(opencaps::format(*need), "root-layout>=2");
    EXPECT_EQ(opencaps::parse_need(" agent-display ")->level, 1);
    EXPECT_EQ(opencaps::format(*opencaps::parse_need("agent-display")), "agent-display");
    for (const auto* bad : {"", "Root", "root-layout>=0", "root-layout>=two", "9lives", "root layout"})
        EXPECT_FALSE(opencaps::parse_need(bad)) << bad;

    opencaps::Provides window;
    ASSERT_TRUE(opencaps::parse_provision("root-layout=1", window));
    ASSERT_TRUE(opencaps::parse_provision("root-layout=2", window));
    ASSERT_TRUE(opencaps::parse_provision("host-gpu", window));
    EXPECT_EQ(window.at("root-layout"), 2) << "the highest level provided";
    EXPECT_FALSE(opencaps::parse_provision("root-layout>=2", window));

    const std::vector<opencaps::Need> needs{*need, {"host-gpu", 1}, {"agent-display", 1}, {"warp-drive", 1}};
    const auto all = opencaps::check(needs, window);
    ASSERT_EQ(all.size(), 4u);
    EXPECT_TRUE(all[0].met());
    EXPECT_TRUE(all[1].met());
    EXPECT_FALSE(all[2].met());
    EXPECT_NE(all[2].why().find("not provided"), std::string::npos) << all[2].why();
    EXPECT_FALSE(all[3].met());
    EXPECT_FALSE(all[3].known);
    EXPECT_NE(all[3].why().find("newer"), std::string::npos) << "an unknown name: the other side is newer";
    opencaps::Provides old{{"root-layout", 1}};
    const auto behind = opencaps::unmet(std::vector<opencaps::Need>{*need}, old);
    ASSERT_EQ(behind.size(), 1u);
    EXPECT_EQ(behind[0].have, 1);
    EXPECT_NE(behind[0].why().find("root-layout 1 is provided"), std::string::npos) << behind[0].why();
}

XTEST(Opencaps, WhatAPartProvidesIsRegistered, .area = "luban", .covers = {"LUBAN-CAPS"}) {
    EXPECT_EQ(opencaps::known_level("root-layout"), 2);
    EXPECT_EQ(opencaps::known_level("nothing-such"), 0);
    // Every registered name is a valid name, and its levels run from 1 without a gap.
    std::map<std::string, std::set<int>> levels;
    for (const auto& l : opencaps::registry()) {
        EXPECT_TRUE(opencaps::parse_need(std::format("{}>={}", l.name, l.level))) << l.name;
        levels[std::string(l.name)].insert(l.level);
    }
    for (const auto& [name, ls] : levels) EXPECT_EQ(*ls.rbegin(), static_cast<int>(ls.size())) << name;
    EXPECT_TRUE(opencaps::conformance::check({{"root-layout", 2}, {"host-gpu", 1}}).ok());
    const auto bad = opencaps::conformance::check({{"root-layout", 3}, {"made-up", 1}});
    EXPECT_EQ(bad.failed.size(), 2u);
}
