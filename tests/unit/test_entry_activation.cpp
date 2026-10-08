#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg.executor;
import xlings.core.xim.installer;
import xlings.core.xim.libxpkg.types.type;

TEST(EntryActivation, AnotherPackageCannotClaimTheSharedClientEvenWithoutAnExistingOwner) {
    xlings::xim::PlanNode node;
    node.name = "fixture";
    node.namespaceName = "xim";
    node.canonicalName = "xim:fixture";
    node.version = "1.0.0";
    mcpplibs::xpkg::XvmOp operation;
    operation.op = "add";
    operation.name = "xlings";
    operation.type = "group";
    operation.version = "1.0.0";
    auto forged =
        xlings::xim::normalize_xpkg_registration_plan(node, {operation}, "", "/fixture", true);
    ASSERT_FALSE(forged);
    EXPECT_NE(forged.error().message.find("only the xlings package"), std::string::npos);

    node.name = "xlings";
    node.namespaceName = "thirdparty";
    node.canonicalName = "thirdparty:xlings";
    auto other_provider = xlings::xim::normalize_xpkg_registration_plan(
        node, {operation}, "thirdparty", "/fixture", true);
    EXPECT_FALSE(other_provider);

    node.namespaceName = "xim";
    node.canonicalName = "xim:xlings";
    auto designated =
        xlings::xim::normalize_xpkg_registration_plan(node, {operation}, "", "/fixture", true);
    ASSERT_TRUE(designated) << designated.error().message;
    EXPECT_EQ(designated->batch.provider, "xim:xlings");
}
