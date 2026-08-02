#include <gtest/gtest.h>

import std;
import xlings.platform;

TEST(ShellCommand, BuildsPlatformArguments) {
    EXPECT_EQ(xlings::platform::shell_command_argv("/bin/sh", "exit 37", false),
              (std::vector<std::string>{"/bin/sh", "-c", "exit 37"}));
    EXPECT_EQ(xlings::platform::shell_command_argv("pwsh.exe", "exit 37", false),
              (std::vector<std::string>{"pwsh.exe", "-NoLogo", "-NonInteractive",
                                        "-Command", "exit 37"}));
    EXPECT_EQ(xlings::platform::shell_command_argv("cmd.exe", "exit 37", false),
              (std::vector<std::string>{"cmd.exe", "/d", "/s", "/c", "exit 37"}));
}

#if !defined(_WIN32)
TEST(ShellCommand, PropagatesExitStatus) {
    EXPECT_EQ(xlings::platform::run_shell("exit 37", false), 37);
}
#endif
