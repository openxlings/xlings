#include <gtest/gtest.h>

import std;
import xlings.core.version_order;

using namespace xlings::version_order;

TEST(VersionOrder, FourComponentRevisionIsNumeric) {
    EXPECT_TRUE(compare("2026.8.3.10", "2026.8.3.9") > 0);
    EXPECT_TRUE(compare("2026.8.3.2", "2026.8.3.10") < 0);
}

TEST(VersionOrder, SemanticVersionsRemainNumeric) {
    std::vector<std::string> versions{"0.0.9", "0.0.100", "0.0.11"};
    sort_desc(versions);
    EXPECT_EQ(versions,
              (std::vector<std::string>{"0.0.100", "0.0.11", "0.0.9"}));
}

TEST(VersionOrder, MissingNumericComponentsCompareAsZero) {
    EXPECT_TRUE(compare("1.2", "1.2.0") == 0);
    EXPECT_TRUE(compare("1.2", "1.2.1") < 0);
}

TEST(VersionOrder, NonVersionsSortDeterministicallyAfterVersions) {
    std::vector<std::string> versions{
        "res_versioned", "latest", "2.0.0", "1.0.0"};
    sort_desc(versions);
    EXPECT_EQ(versions[0], "2.0.0");
    EXPECT_EQ(versions[1], "1.0.0");
    EXPECT_EQ(versions[2], "res_versioned");
    EXPECT_EQ(versions[3], "latest");
}

TEST(VersionOrder, ResourceSentinelsAreInternal) {
    EXPECT_TRUE(is_internal_key("res_versioned"));
    EXPECT_FALSE(is_internal_key("2026.8.3.1"));
    EXPECT_FALSE(is_internal_key("latest"));
}
