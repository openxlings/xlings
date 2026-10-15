#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

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

// A regular user in a system whose home is root's has a home of their own
// (design 2026-10-16 L §8): `xlings install` there needs no root.
namespace {
std::string home_line_(const tk::RunResult& r) {
    std::istringstream in(r.out);
    for (std::string line; std::getline(in, line);)
        if (line.find("XLINGS_HOME") != std::string::npos) return line;
    return {};
}
struct ReadOnly {
    std::filesystem::path dir;
    explicit ReadOnly(std::filesystem::path d) : dir(std::move(d)) {
        std::filesystem::permissions(dir, std::filesystem::perms::owner_write | std::filesystem::perms::group_write
                                              | std::filesystem::perms::others_write,
                                     std::filesystem::perm_options::remove);
    }
    ~ReadOnly() {
        std::filesystem::permissions(dir, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    }
};
}  // namespace

XTEST(CurrentSystem, ARegularUserInASystemHasAHomeOfTheirOwn, .area = "subos", .covers = {"LUBAN-USER-LAYER"}) {
    if constexpr (!xlings::platform::is_linux) GTEST_SKIP() << "Luban systems are Linux";
    if (xlings::platform::is_root()) GTEST_SKIP() << "root writes every home";
    auto home = tk::Home::isolated("user-layer");
    const auto system = home.root() / "system";
    const std::map<std::string, std::string> asSystem{{"XLINGS_HOME", system.string()}};
    ASSERT_EQ(home.xlings({"subos", "new", "sys", "-y"}, asSystem).exit_code, 0);
    ReadOnly guard(system);
    auto inSystem = asSystem;
    inSystem["XLINGS_ROOT_INSTANCE"] = "sys";
    const auto inside = home.xlings({"config"}, inSystem);
    ASSERT_EQ(inside.exit_code, 0) << inside.transcript();
    EXPECT_NE(home_line_(inside).find(home.dir().string()), std::string::npos) << inside.out;
    // In a sandbox the broker installs into the system: the home stays.
    auto sandboxed = inSystem;
    sandboxed["XLINGS_SUBOS_MODE"] = "sandbox";
    const auto there = home.xlings({"config"}, sandboxed);
    EXPECT_NE(home_line_(there).find(system.string()), std::string::npos) << there.out;
    // Outside a system a read-only home is only read-only.
    const auto outside = home.xlings({"config"}, asSystem);
    EXPECT_NE(home_line_(outside).find(system.string()), std::string::npos) << outside.out;
}
