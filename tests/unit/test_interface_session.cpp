// The interface session's helper threads (2026-10-26, X1): a call ends when its
// capability does. The heartbeat slept in 500 ms slices and was joined at the
// end of every call, so `interface payload_status` -- about 40 ms of work --
// took 517 ms on every invocation mcpp made.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;

XTEST(InterfaceSession, ACallEndsWhenItsCapabilityDoes, .area = "interface",
      .covers = {"IFACE-CALL-ENDS"}, .requires_ = {"xlings-bin", "posix"}) {
    auto home = tk::Home::isolated("iface-call-ends");
    ASSERT_EQ(home.xlings({"interface", "--version"}).exit_code, 0);   // warm the home
    std::chrono::milliseconds best { std::chrono::hours(1) };
    for (int i = 0; i < 3; ++i) {
        auto r = home.xlings({"interface", "payload_status", "--args", R"({"targets":[]})"});
        ASSERT_NE(r.out.find(R"("kind":"result")"), std::string::npos) << r.transcript();
        best = std::min(best, r.elapsed);
    }
    // Below the old fixed half-second with room for an unoptimized build on a
    // shared runner; the floor it guards is the 500 ms sleep, not a budget.
    EXPECT_LT(best.count(), 450) << "a call held open after its capability returned";
}
