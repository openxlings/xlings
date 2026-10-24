// A library's noise kept off a person's terminal (design 2026-10-24 J70):
// lines on fd 2 that are diverted go to their sink, the others stay.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

XTEST(PlatformStderr, ALineDivertedGoesToItsSinkAndTheOthersStay, .area = "platform", .covers = {"LUBAN-OS-QUIET-TERMINAL"},
      .requires_ = {"posix"}) {
    std::mutex m;
    std::vector<std::string> diverted;
    xlings::platform::divert_stderr_lines([](std::string_view l) { return l.find("imDefLkup.c") != std::string_view::npos; },
                                          [&](std::string_view l) { std::lock_guard g(m); diverted.emplace_back(l); });
    std::fputs("Warning: imDefLkup.c,419: The application disposed a key event with 1 serial.\n", stderr);
    std::fputs("a line of its own\n", stderr);
    std::fflush(stderr);
    for (int k = 0; k < 100; ++k) {
        { std::lock_guard g(m); if (!diverted.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::lock_guard g(m);
    ASSERT_EQ(diverted.size(), 1u);
    EXPECT_NE(diverted[0].find("disposed a key event"), std::string::npos);
}
