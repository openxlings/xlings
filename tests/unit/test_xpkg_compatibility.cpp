#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import xlings.core.xim.compatibility;

using namespace xlings::xim;

TEST(XpkgCompatibility, ArchDeclarationsAreFailClosedForEverySpec) {
    mcpplibs::xpkg::Package package;
    package.spec = "1";
    package.archs = {"x86_64"};
    auto result = check_target_compatibility(package, "linux", "aarch64");
    EXPECT_FALSE(result.supported);
    EXPECT_EQ(result.target, "linux-aarch64");
    EXPECT_EQ(result.supportedTargets,
              (std::vector<std::string>{"linux-x86_64"}));
    EXPECT_EQ(compatibility_error("xim:d2x@1", result),
              "E_UNSUPPORTED_TARGET: xim:d2x@1 has no linux-aarch64 artifact; "
              "supported targets: linux-x86_64");
}

TEST(XpkgCompatibility, ArmAliasesMatchAndEmptyMeansPortable) {
    mcpplibs::xpkg::Package arm;
    arm.archs = {"arm64"};
    EXPECT_TRUE(check_target_compatibility(arm, "macosx", "aarch64").supported);

    mcpplibs::xpkg::Package portable;
    EXPECT_TRUE(check_target_compatibility(portable, "windows", "x86_64").supported);
}
