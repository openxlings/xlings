#include <gtest/gtest.h>

import std;
import xlings.core.xvm.db;
import xlings.core.xvm.types;

using namespace xlings::xvm;

// Regression: `xlings use <t> latest` must pick the highest *numeric* version,
// stripping the "<scope>:" prefix. A bootstrap "local:0.4.47" must NOT beat a
// higher real release "0.4.52" (the bug that left `xlings self update` on the
// old local version because lexicographic "local:..." > "0.4.52").
TEST(XvmLatest, PrefersHigherReleaseOverLocalScope) {
    std::map<std::string, VData> v;
    v["0.4.40"];
    v["0.4.52"];
    v["local:0.4.47"];
    EXPECT_EQ(pick_highest_version(v), "0.4.52");
}

TEST(XvmLatest, LocalScopeWinsOnlyWhenActuallyHighest) {
    std::map<std::string, VData> v;
    v["0.4.40"];
    v["local:0.4.99"];
    EXPECT_EQ(pick_highest_version(v), "local:0.4.99");
}

TEST(XvmLatest, PlainNumericOrdering) {
    std::map<std::string, VData> v;
    v["0.4.9"];
    v["0.4.52"];
    v["0.4.10"];
    EXPECT_EQ(pick_highest_version(v), "0.4.52");  // numeric, not lexicographic
}
