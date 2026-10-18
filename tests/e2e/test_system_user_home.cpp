// A user's xlings inside a Luban system (design 2026-10-19 §19, F18): the
// system's home is root's, a user's installs go to their own ~/.xlings. A
// command that only reads makes nothing and says nothing; the first write
// makes the user's home and says once how to install for every user -- from
// the host, never with sudo.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(SystemUserHome, AReadOnlyCommandMakesNothingTheFirstWriteSaysHowOnce,
      .area = "subos", .covers = {"SUBOS-READONLY-USER"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban system is Linux";
    if (xlings::platform::is_root()) GTEST_SKIP() << "root writes the system's home itself";
    auto home = tk::Home::isolated("system-user-home");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // The system's home as a user meets it: root's, not theirs to write.
    fs::permissions(home.dir(), fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read
                    | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    const auto user = home.root() / "alice";
    fs::create_directories(user);
    const std::map<std::string, std::string> inside{{"XLINGS_ROOT_INSTANCE", "box"}, {"HOME", user.string()}};
    for (const std::vector<std::string> args : {std::vector<std::string>{"--version"}, {"list"}, {"info", "xlings"}}) {
        const auto r = home.xlings(args, inside);
        EXPECT_EQ(r.transcript().find("your own home"), std::string::npos) << args[0] << "\n" << r.transcript();
        EXPECT_EQ(r.transcript().find("sudo"), std::string::npos) << r.transcript();
        EXPECT_FALSE(fs::exists(user / ".xlings")) << "a read made a home: " << args[0];
    }
    const auto first = home.xlings({"config", "--mirror", "GLOBAL"}, inside);
    EXPECT_NE(first.transcript().find("your own home"), std::string::npos) << first.transcript();
    EXPECT_NE(first.transcript().find("luban run"), std::string::npos) << first.transcript();
    EXPECT_NE(first.transcript().find("--user root -- xlings install --system"), std::string::npos) << first.transcript();
    EXPECT_EQ(first.transcript().find("sudo"), std::string::npos) << first.transcript();
    EXPECT_TRUE(fs::exists(user / ".xlings"));
    const auto again = home.xlings({"--version"}, inside);
    EXPECT_EQ(again.transcript().find("your own home"), std::string::npos) << "said once\n" << again.transcript();
    fs::permissions(home.dir(), fs::perms::owner_all, fs::perm_options::add);
}
