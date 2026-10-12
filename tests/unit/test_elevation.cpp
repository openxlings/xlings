// Administrator rights are asked for only where somebody can answer.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.subos.elevation;
import xlings.core.xself.repair;

namespace elevation = xlings::subos::elevation;

XTEST(Elevation, AgentModeAsksNobody, .area = "subos", .covers = {"ELEVATION-NOBODY-TO-ASK"}) {
    xlings::platform::set_env_variable("XLINGS_AGENT_MODE", "1");
    EXPECT_FALSE(elevation::can_ask());
    xlings::platform::unset_env_variable("XLINGS_AGENT_MODE");
    const auto line = elevation::nobody_to_ask("mounting the home image of 'box'", "xlings subos use box");
    EXPECT_NE(line.find("xlings subos use box"), std::string::npos) << line;
}

XTEST(Elevation, RepairChildrenReadNothingFromTheTerminal, .area = "self", .covers = {"ELEVATION-NOBODY-TO-ASK"}) {
    const auto suffix = xlings::xself::quiet_suffix();
    EXPECT_NE(suffix.find(std::format("<{}", xlings::platform::null_device)), std::string::npos) << suffix;
    if constexpr (xlings::platform::is_posix) {
        // The child sees agent mode; the parent's environment is restored.
        const int rc = xlings::xself::run_unattended("test \"$XLINGS_AGENT_MODE\" = 1");
        EXPECT_EQ(rc, 0);
        EXPECT_EQ(std::getenv("XLINGS_AGENT_MODE"), nullptr);
    }
}
