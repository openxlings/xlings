#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.platform.root_mount;

namespace tk = xlings::testkit;
namespace rm = xlings::platform::root_mount;

XTEST(RootMount, InvalidDescriptorsCannotSelectAHostNamespace, .area = "subos",
      .covers = {"ROOT-NO-HOST"}) {
    const std::array<int, 3> invalid{-1, -1, -1};
    auto untrusted = invalid;
    EXPECT_FALSE(rm::normalize(untrusted));
    EXPECT_EQ(untrusted, invalid);
    const auto result = rm::update(invalid, {}, {});
    ASSERT_FALSE(result);
    EXPECT_FALSE(result.error().empty());
    if constexpr (!tk::is_linux)
        EXPECT_FALSE(rm::capture());
}

// A captured namespace is normalised to the owner of its mount namespace, and
// a binding that leaves the root is refused. Normalising proves the captured
// user namespace's ancestry from that owner (NS_GET_USERNS), which a process in
// a user namespace nested under another process's cannot name (EPERM): the
// capability states it, and a lane that declares it fails without it.
XTEST(RootMount, ACapturedNamespaceRefusesABindingOutsideTheRoot, .area = "subos",
      .covers = {"ROOT-NO-HOST"}, .requires_ = {"linux", "mntns-owner"}) {
    auto captured = rm::capture();
    ASSERT_TRUE(captured) << captured.error();
    const auto normalized = rm::normalize(*captured);
    ASSERT_TRUE(normalized) << normalized.error();
    const std::vector<rm::Binding> escape{{.source = "/", .destination = "../outside"}};
    const auto rejected = rm::update(*captured, escape, {});
    EXPECT_FALSE(rejected);
    for (const int fd : *captured)
        xlings::platform::close_fd(fd);
}
