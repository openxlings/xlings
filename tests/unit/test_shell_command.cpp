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

// Luban design §A4: a shim's alias that is plain words runs without a shell.
TEST(ShellCommand, PlainWordsSplitAndShellSyntaxDoesNot) {
    using V = std::vector<std::string>;
    const auto split = [](std::string_view c) { return xlings::platform::split_plain_words(c); };
    EXPECT_EQ(split("/opt/gcc/bin/gcc --sysroot=/x/y -B /a"), (V{"/opt/gcc/bin/gcc", "--sysroot=/x/y", "-B", "/a"}));
    EXPECT_EQ(split("prog 'a b' \"c d\" e\\ f"), (V{"prog", "a b", "c d", "e f"}));
    EXPECT_EQ(split("prog \"a \\\"q\\\" b\""), (V{"prog", "a \"q\" b"}));
    for (const auto* shell : {"prog | tee x", "prog > out", "prog $HOME", "prog \"$HOME\"", "a; b", "prog *.c",
                              "FOO=1 prog", "prog `id`", "prog 'unclosed", ""})
        EXPECT_FALSE(split(shell)) << shell;
}
