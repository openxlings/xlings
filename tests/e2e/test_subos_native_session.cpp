// A native SubOS on macOS and Windows under the same contract as Linux
// (SubOS design part 3 §6.3): on macOS the home redirect runs under the
// supervisor -- start, join, stop, --timeout, the exit-code table; on Windows
// its command runs in a Job Object -- the whole tree ends at --timeout (124)
// and the exit code is the command's.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.caps;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.spec;
import xlings.confine;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("native");
    Box() {
        auto r = home.xlings({"subos", "new", "box"});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }
    tk::RunResult exec(std::vector<std::string> args) const {
        std::vector<std::string> a{"subos", "exec", "box", "--sandbox"};
        a.insert(a.end(), args.begin(), args.end());
        return home.xlings(a);
    }
};

}  // namespace

XTEST(NativeSession, TheHomeRedirectLaunchesAsTheSupervisorsSessionInit,
      .area = "subos", .covers = {"SESSION-NATIVE-SUPERVISED"}) {
    // The mechanism the macOS supervisor stands on, on any host: a home
    // redirect is started as this binary's __session-init with its command
    // after it -- the same entry a Linux sandbox's supervisor starts.
    namespace sp = xlings::subos::spec;
    xlings::subos::caps::Caps mac;
    mac.platform = "macos";
    sp::Request r;
    r.instance = "box";
    r.instance_dir = "/h/subos/box";
    r.user = "u";
    r.argv = {"/bin/echo", "hi"};
    auto compiled = xlings::confine::compile(xlings::subos::policy::legacy(), xlings::subos::HomeView{"/h"}, mac, r);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->backend, sp::Backend::HomeRedirect);
    auto launched = *compiled;
    launched.argv = {"/run/xlings/xlings", "__session-init", "--", "/bin/echo", "hi"};
    EXPECT_EQ(xlings::confine::launch_argv(launched, std::nullopt, "/Applications/x/bin/xlings"),
              (std::vector<std::string>{"/Applications/x/bin/xlings", "__session-init", "--", "/bin/echo", "hi"}));
}

XTEST(NativeSession, MacosRunsTheHomeRedirectUnderTheSupervisor,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"SESSION-NATIVE-SUPERVISED"}, .requires_ = {"macos", "xlings-bin"}) {
    Box box;
    EXPECT_EQ(box.exec({"--", "/bin/sh", "-c", "exit 3"}).exit_code, 3);
    EXPECT_EQ(box.exec({"--", "no-such-program"}).exit_code, 127);
    EXPECT_EQ(box.exec({"--timeout", "1", "--", "/bin/sleep", "30"}).exit_code, 124);
    EXPECT_EQ(box.exec({"--", "/bin/sh", "-c", "kill -TERM $$"}).exit_code, 128 + 15);
    // A hot session: what the first command leaves, the second -- joining --
    // sees; its home is the instance's (the redirect).
    auto started = box.home.xlings({"subos", "start", "box", "--ttl", "60"});
    ASSERT_EQ(started.exit_code, 0) << started.transcript();
    EXPECT_TRUE(fs::exists(box.home.dir() / "run" / "subos" / "box" / "session.json"));
    EXPECT_EQ(box.exec({"--", "/bin/sh", "-c", "echo hot > \"$HOME/state\""}).exit_code, 0);
    auto r = box.exec({"--", "/bin/sh", "-c", "cat \"$HOME/state\"; echo \"$HOME\""});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("hot\n"), std::string::npos) << r.transcript();
    EXPECT_NE(r.out.find((box.home.dir() / "subos" / "box").string()), std::string::npos) << r.transcript();
    EXPECT_EQ(box.home.xlings({"subos", "stop", "box"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(box.home.dir() / "run" / "subos" / "box" / "session.json"));
}

XTEST(NativeSession, WindowsRunsTheCommandInAJobAndEndsTheWholeTreeAtTheTimeout,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"SESSION-NATIVE-SUPERVISED"}, .requires_ = {"windows", "xlings-bin"}) {
    Box box;
    EXPECT_EQ(box.exec({"--", "cmd.exe", "/c", "exit 3"}).exit_code, 3);
    const auto started = std::chrono::steady_clock::now();
    // cmd starts ping: a tree, not one process -- the job ends both.
    EXPECT_EQ(box.exec({"--timeout", "2", "--", "cmd.exe", "/c", "ping -n 30 127.0.0.1 >NUL"}).exit_code, 124);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(20));
}
