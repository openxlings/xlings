#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xim.resolver;
import xlings.core.xim.libxpkg.types.type;

namespace xim = xlings::xim;

namespace {
xim::PlanNode node(std::string name) {
    xim::PlanNode result;
    result.name = name;
    result.version = "1";
    result.namespaceName = "xim";
    result.canonicalName = "xim:" + name;
    return result;
}
std::vector<std::string> names(const xim::InstallPlan& plan) {
    std::vector<std::string> result;
    for (const auto& item : plan.nodes) result.push_back(item.name);
    return result;
}
}

XTEST(InstallToolOrder, RelocationToolRunsBeforeConsumersWithoutChangingTheirRuntimeClosure,
      .area = "xim", .covers = {"DOM-ELFPATCH-FAILCLOSED"}) {
    xim::InstallPlan plan;
    auto xz = node("xz");
    xz.deps = {"glibc"};
    xz.runtime_deps = {"glibc"};
    xz.depEdges = {{"glibc", xim::DepKind::Runtime, "xim:glibc@1"}};
    plan.nodes = {node("glibc"), xz, node("patchelf")};
    ASSERT_TRUE(xim::require_install_tool(plan, "xim:patchelf@1"));
    EXPECT_EQ(names(plan), (std::vector<std::string>{"patchelf", "glibc", "xz"}));
    EXPECT_EQ(plan.nodes.back().runtime_deps, (std::vector<std::string>{"glibc"}));
    EXPECT_EQ(plan.nodes.back().deps, (std::vector<std::string>{"glibc"}));
    EXPECT_EQ(plan.nodes.back().depEdges.back().kind, xim::DepKind::Build);
}

XTEST(InstallToolOrder, TheToolsOwnDependenciesPrecedeItWithoutCreatingABootstrapCycle,
      .area = "xim", .covers = {"DOM-ELFPATCH-FAILCLOSED"}) {
    xim::InstallPlan plan;
    auto tool = node("patchelf");
    tool.depEdges = {{"glibc", xim::DepKind::Runtime, "xim:glibc@1"}};
    plan.nodes = {node("xz"), tool, node("glibc")};
    ASSERT_TRUE(xim::require_install_tool(plan, "xim:patchelf@1"));
    EXPECT_EQ(names(plan), (std::vector<std::string>{"glibc", "patchelf", "xz"}));
    EXPECT_TRUE(plan.nodes.front().depEdges.empty());
}

XTEST(InstallToolOrder, MissingToolsDependenciesAndCyclesLeaveThePlanUnchanged,
      .area = "xim", .covers = {"DOM-ELFPATCH-FAILCLOSED"}) {
    xim::InstallPlan plan;
    plan.nodes = {node("xz")};
    EXPECT_FALSE(xim::require_install_tool(plan, "xim:patchelf@1"));
    EXPECT_EQ(names(plan), (std::vector<std::string>{"xz"}));
    auto tool = node("patchelf");
    tool.depEdges = {{"glibc", xim::DepKind::Runtime, "xim:glibc@1"}};
    plan.nodes.push_back(tool);
    EXPECT_FALSE(xim::require_install_tool(plan, "xim:patchelf@1"));
    auto libc = node("glibc");
    libc.depEdges = {{"patchelf", xim::DepKind::Runtime, "xim:patchelf@1"}};
    plan.nodes.push_back(libc);
    EXPECT_FALSE(xim::require_install_tool(plan, "xim:patchelf@1"));
    EXPECT_EQ(names(plan), (std::vector<std::string>{"xz", "patchelf", "glibc"}));
    EXPECT_TRUE(plan.nodes.front().depEdges.empty());
}
