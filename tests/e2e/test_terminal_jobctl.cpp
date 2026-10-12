#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;

// A command running on a terminal whose foreground is another process group
// -- a shell's background job, or what the SubOS broker runs for a joined
// command on its caller's terminal -- writes lines and never changes the
// terminal's modes: the kernel stops the whole group for that (SIGTTOU), and
// inside `luban enter` the group was the session's owner, so the user's
// shell reported `luban enter` itself as stopped.
XTEST(TerminalJobControl, ACommandInTheBackgroundOfATerminalIsNeverStopped, .area = "cli",
      .covers = {"SES-TTY-JOBCTL"}, .requires_ = {"pty", "xlings-bin"}) {
    auto home = tk::Home::isolated("terminal-jobctl");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    for (auto args : {std::vector<std::string>{"config"}, std::vector<std::string>{"list"}}) {
        tk::RunOptions run{.argv = args, .env = {}, .timeout = std::chrono::seconds(60)};
        run.pty = true;
        run.pty_background = true;
        run.env = home.env();
        run.env["TERM"] = "xterm-256color";
        auto r = home.xlings(run);
        EXPECT_NE(r.exit_code, tk::kStoppedByTerminal) << args[0] << "\n" << r.transcript();
        EXPECT_EQ(r.exit_code, 0) << args[0] << "\n" << r.transcript();
    }
}
