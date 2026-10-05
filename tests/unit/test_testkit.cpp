// The test library's own tests (T1). Everything else trusts these four
// answers -- which binary, which home, which environment, why it did not run
// -- so they are checked first.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(Testkit, RunReportsExitCodeAndBothStreams,
      .area = "testkit", .covers = {"TK-RUN"}, .requires_ = {"posix"}) {
    auto r = tk::run({ .argv = {"/bin/sh", "-c", "echo out; echo err >&2; exit 7"},
                       .env = {{"PATH", "/usr/bin:/bin"}} });
    EXPECT_EQ(r.exit_code, 7) << r.transcript();
    EXPECT_EQ(r.out, "out\n");
    EXPECT_EQ(r.err, "err\n");
    EXPECT_FALSE(r.timed_out);
}

XTEST(Testkit, RunStartsFromTheGivenEnvironmentOnly,
      .area = "testkit", .covers = {"TK-HERMETIC"}, .requires_ = {"posix"}) {
    // Whatever the developer's shell exports must not reach the child: this
    // is the leak that made test_interface_protocol see the developer's subos.
    auto r = tk::run({ .argv = {"/usr/bin/env"}, .env = {{"ONLY", "1"}} });
    EXPECT_EQ(r.out, "ONLY=1\n") << r.transcript();
}

XTEST(Testkit, RunKillsATimedOutProcessGroup,
      .area = "testkit", .covers = {"TK-TIMEOUT"}, .requires_ = {"posix"}) {
    auto r = tk::run({ .argv = {"/bin/sh", "-c", "sleep 30 & sleep 30"},
                       .env = {{"PATH", "/usr/bin:/bin"}},
                       .timeout = std::chrono::milliseconds(300) });
    EXPECT_TRUE(r.timed_out);
    EXPECT_EQ(r.exit_code, -1);
    EXPECT_LT(r.elapsed, std::chrono::seconds(5));
}

XTEST(Testkit, PtyWithNoInputExposesAProgramThatWaits,
      .area = "testkit", .covers = {"TK-PTY"}, .requires_ = {"pty"}) {
    // `read` on a terminal nobody types into blocks: the agent contract scan
    // relies on exactly this to catch a command that asks a question.
    auto waits = tk::run({ .argv = {"/bin/sh", "-c", "read answer; echo got"},
                           .env = {{"PATH", "/usr/bin:/bin"}},
                           .timeout = std::chrono::milliseconds(500), .pty = true });
    EXPECT_TRUE(waits.timed_out) << waits.transcript();

    auto answered = tk::run({ .argv = {"/bin/sh", "-c", "read answer; echo got-$answer"},
                              .env = {{"PATH", "/usr/bin:/bin"}},
                              .timeout = std::chrono::seconds(5),
                              .stdin_data = "y\n", .pty = true });
    EXPECT_FALSE(answered.timed_out);
    EXPECT_NE(answered.out.find("got-y"), std::string::npos) << answered.transcript();
}

XTEST(Testkit, IsolatedHomeIsOutsideTheUsersHomeAndIsRemoved,
      .area = "testkit", .covers = {"TK-HOME"}) {
    fs::path root;
    {
        auto home = tk::Home::isolated("self");
        root = home.root();
        EXPECT_TRUE(fs::is_directory(home.dir()));
        EXPECT_EQ(home.dir(), home.root() / ".xlings");
        if (const char* h = std::getenv("HOME"); h && *h) {
            auto real = fs::path(h).lexically_normal().string();
            EXPECT_FALSE(home.root().string().starts_with(real + "/.xlings"));
        }
        auto env = home.env();
        EXPECT_EQ(env["XLINGS_HOME"], home.dir().string());
        EXPECT_FALSE(env.contains("XLINGS_ACTIVE_SUBOS"));
        EXPECT_FALSE(env.contains("XLINGS_SUBOS_MODE"));
    }
    EXPECT_FALSE(fs::exists(root));
}

XTEST(Testkit, UnknownCapabilityIsAFailureNotASkip,
      .area = "testkit", .covers = {"TK-CAPS"}) {
    auto verdict = tk::check_requirements({ .requires_ = {"bwarp"} });
    ASSERT_TRUE(verdict.has_value());
    EXPECT_TRUE(verdict->fail);
    EXPECT_NE(verdict->reason.find("unknown capability"), std::string::npos);
}

XTEST(Testkit, NetworkIsOptIn,
      .area = "testkit", .covers = {"TK-CAPS"}) {
    if (tk::lane_declares("network") || std::getenv("XDEV_NETWORK")) GTEST_SKIP() << "lane opts in";
    auto verdict = tk::check_requirements({ .requires_ = {"network"} });
    ASSERT_TRUE(verdict.has_value());
    EXPECT_FALSE(verdict->fail);
}

XTEST(Testkit, MetadataIsRegisteredBeforeAnyTestRuns,
      .area = "testkit", .covers = {"TK-META"}) {
    const auto& reg = tk::registry();
    auto it = reg.find("Testkit.RunReportsExitCodeAndBothStreams");
    ASSERT_NE(it, reg.end());
    EXPECT_EQ(it->second.area, "testkit");
    EXPECT_EQ(it->second.covers, std::vector<std::string>{"TK-RUN"});
}

XTEST(Testkit, ALaneThatDeclaresACapabilityFailsWhenItIsMissing,
      .area = "testkit", .covers = {"F13"}) {
    // The rule that replaces "skip when the backend is missing" (F13): a skip
    // on a developer machine, a failure on the lane that promised it.
    const tk::Meta needs_other_os{ .requires_ = {tk::is_windows ? "linux" : "windows"} };
    const std::vector<std::string> nothing;
    const std::vector<std::string> promised{"linux", "windows", "macos"};

    auto dev = tk::check_requirements(needs_other_os, nothing);
    ASSERT_TRUE(dev.has_value());
    EXPECT_FALSE(dev->fail) << dev->reason;

    auto lane = tk::check_requirements(needs_other_os, promised);
    ASSERT_TRUE(lane.has_value());
    EXPECT_TRUE(lane->fail);
    EXPECT_NE(lane->reason.find("declares"), std::string::npos) << lane->reason;
}
