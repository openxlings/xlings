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

// One priority chain for both families. Windows honoured XLINGS_SHELL while
// POSIX read only SHELL, so the documented override worked on one platform and
// was silently ignored on the other.
TEST(ShellCommand, XlingsShellOverridesOnEveryPlatform) {
    const auto* previous = std::getenv("XLINGS_SHELL");
    const std::string saved = previous ? previous : "";

    xlings::platform::set_env_variable("XLINGS_SHELL", "/opt/probe-shell");
    EXPECT_EQ(xlings::platform::resolve_shell(), "/opt/probe-shell");
    EXPECT_EQ(xlings::platform::shell_candidates().front(), "/opt/probe-shell");

    xlings::platform::set_env_variable("XLINGS_SHELL", "");
    // Cleared, so the platform default takes over again -- and whatever it is,
    // the reported shell and the first candidate must be the same string. A
    // second, independent fallback at a call site is how `subos_entering` came
    // to announce a shell the process never started.
    EXPECT_EQ(xlings::platform::resolve_shell(),
              xlings::platform::shell_candidates().front());
    EXPECT_FALSE(xlings::platform::shell_candidates().empty());

    if (!saved.empty()) {
        xlings::platform::set_env_variable("XLINGS_SHELL", saved);
    }
}
