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
    auto captured = rm::capture();
    if constexpr (!tk::is_linux) {
        EXPECT_FALSE(captured);
    } else {
        ASSERT_TRUE(captured) << captured.error();
        ASSERT_TRUE(rm::normalize(*captured));
        const std::vector<rm::Binding> escape{{.source = "/", .destination = "../outside"}};
        const auto rejected = rm::update(*captured, escape, {});
        EXPECT_FALSE(rejected);
        for (const int fd : *captured)
            xlings::platform::close_fd(fd);
    }
}
