#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;

// Inside a Luban system "this subos" is the system itself (design 2026-10-16
// L §9). A machine's shim read its home's activeSubos -- `default`, the
// owner's choice -- and refused the vim the system's generation had placed.

namespace {
std::string active_line_(const tk::RunResult& r) {
    std::istringstream in(r.out);
    for (std::string line; std::getline(in, line);)
        if (line.find("\xE2\x96\xB8") != std::string::npos) return line;   // ▸ marks the active one
    return {};
}
}  // namespace

XTEST(CurrentSystem, InsideARootTheSystemNamesItselfAsTheCurrentSubos, .area = "subos",
      .covers = {"LUBAN-CURRENT-SYSTEM"}) {
    auto home = tk::Home::isolated("current-system");
    ASSERT_EQ(home.xlings({"subos", "new", "sys1", "-y"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "sys2", "-y"}).exit_code, 0);

    const auto outside = home.xlings({"subos", "list"});
    ASSERT_EQ(outside.exit_code, 0) << outside.out << outside.err;
    EXPECT_EQ(active_line_(outside).find("sys2"), std::string::npos) << outside.out;

    const auto inside = home.xlings({"subos", "list"}, {{"XLINGS_ROOT_INSTANCE", "sys2"}});
    ASSERT_EQ(inside.exit_code, 0) << inside.out << inside.err;
    EXPECT_NE(active_line_(inside).find("sys2"), std::string::npos) << inside.out;
}

XTEST(CurrentSystem, AnExplicitActiveSubosStillWins, .area = "subos", .covers = {"LUBAN-CURRENT-SYSTEM"}) {
    auto home = tk::Home::isolated("current-system-explicit");
    ASSERT_EQ(home.xlings({"subos", "new", "sys1", "-y"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "sys2", "-y"}).exit_code, 0);
    const auto r = home.xlings({"subos", "list"},
                               {{"XLINGS_ROOT_INSTANCE", "sys2"}, {"XLINGS_ACTIVE_SUBOS", "sys1"}});
    ASSERT_EQ(r.exit_code, 0) << r.out << r.err;
    EXPECT_NE(active_line_(r).find("sys1"), std::string::npos) << r.out;
}
