// The broker's classification (C15): what the xlings inside may do itself,
// what it sends outside, and what only the owner does.
#include <gtest/gtest.h>

import std;
import xlings.subos.broker;
import xlings.subos.policy;

namespace b = xlings::subos::broker;
namespace pol = xlings::subos::policy;
using V = std::vector<std::string>;

TEST(SubosBroker, ReadsStayLocal) {
    for (V argv : {V{"list"}, V{"info", "gcc"}, V{"search", "x"}, V{"use", "gcc"},
                   V{"subos", "list"}, V{"subos", "status", "box"}, V{"self", "doctor"},
                   V{"--version"}})
        EXPECT_EQ(b::classify(argv, "box").route, b::Route::Local) << argv[0];
}

TEST(SubosBroker, ChangesToTheHomeGoToTheBroker) {
    auto install = b::classify(V{"install", "gcc", "-y", "cmake"}, "box");
    EXPECT_EQ(install.route, b::Route::Broker);
    ASSERT_EQ(install.ops.size(), 2u);
    EXPECT_EQ(install.ops[0].target, "gcc");
    EXPECT_TRUE(install.ops[0].from_inside);
    EXPECT_EQ(b::classify(V{"update"}, "box").ops.front().kind, "index_update");
    EXPECT_EQ(b::classify(V{"remove", "gcc"}, "box").route, b::Route::Broker);
    EXPECT_EQ(b::classify(V{"use", "gcc", "15.1.0"}, "box").route, b::Route::Broker);
}

TEST(SubosBroker, TheOwnersThingsAreRefusedInside) {
    for (V argv : {V{"self", "update"}, V{"self", "doctor", "--fix"}, V{"subos", "remove", "x"},
                   V{"subos", "config", "box", "--net", "host"}, V{"config", "--mirror", "CN"},
                   V{"install", "x", "--subos", "other"}})
        EXPECT_EQ(b::classify(argv, "box").route, b::Route::Owner) << argv[0] << " " << argv[1];
    EXPECT_EQ(b::classify(V{"install", "x", "--subos", "box"}, "box").route, b::Route::Broker);
}

TEST(SubosBroker, TheStrictestTargetDecides) {
    auto p = pol::preset(pol::Preset::Dev);
    p.fetch_rules.push_back({.match = "evil*", .action = pol::Fetch::Deny});
    auto ok = b::classify(V{"install", "gcc"}, "box");
    EXPECT_EQ(b::decide(p, ok).action, pol::Action::Allow);
    auto mixed = b::classify(V{"install", "gcc", "evil-tool"}, "box");
    EXPECT_EQ(b::decide(p, mixed).action, pol::Action::Deny);
}
