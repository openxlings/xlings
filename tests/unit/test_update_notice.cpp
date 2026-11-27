// The newer-xlings notice (#425, design 2026-11-27 §9.2): what the index sync
// offers is kept for the end of the command, the last offer winning.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.update_notice;

XTEST(UpdateNotice, TheLastOfferIsWhatTheCommandSays, .area = "xself", .covers = {"UPDATE-NOTICE"}) {
    namespace un = xlings::update_notice;
    EXPECT_FALSE(un::pending().has_value());
    un::offer("2026.12.1.1");
    un::offer("2026.12.2.1");
    ASSERT_TRUE(un::pending().has_value());
    EXPECT_EQ(*un::pending(), "2026.12.2.1");
}
