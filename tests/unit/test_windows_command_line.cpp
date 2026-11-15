// How an argv is spelled for a Windows program and for a command script. The
// spelling is host-independent, so every lane checks it; the Windows lane runs
// it for real through a shim (tests/e2e/test_shim_argv.cpp).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

namespace p = xlings::platform;
using V = std::vector<std::string>;

namespace {

// The arguments after the program name, split as CommandLineToArgvW and the
// MSVC C runtime split them: white space separates outside quotes; 2n
// backslashes before a `"` give n and the quote toggles quoting, 2n+1 give n
// and a literal `"`; other backslashes are literal; `""` inside quotes is a
// literal quote.
V split_like_windows(std::string_view line) {
    V out;
    std::size_t i = 0;
    const auto n = line.size();
    for (;;) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= n) break;
        std::string arg;
        bool quoted = false;
        while (i < n) {
            const char c = line[i];
            if (!quoted && (c == ' ' || c == '\t')) break;
            if (c == '\\') {
                std::size_t b = 0;
                while (i < n && line[i] == '\\') { ++b; ++i; }
                if (i < n && line[i] == '"') {
                    arg.append(b / 2, '\\');
                    if (b % 2) { arg += '"'; ++i; }
                } else {
                    arg.append(b, '\\');
                }
                continue;
            }
            if (c == '"') {
                if (quoted && i + 1 < n && line[i + 1] == '"') { arg += '"'; i += 2; continue; }
                quoted = !quoted;
                ++i;
                continue;
            }
            arg += c;
            ++i;
        }
        out.push_back(std::move(arg));
    }
    return out;
}

const V kHard{"plain", "two words", "", "say \"hi\"", "a&echo injected>marker", "100%", "%PATH%",
              "caret^x", "trail\\", "trail space\\ \\", "back\\\\\"slash", "\\\\server\\share\\",
              "|<>()", "tab\there", "\"", "\\\"", "C:\\Program Files\\x\\", "e\xc3\xa9\xe4\xb8\xad"};

}  // namespace

XTEST(WindowsCommandLine, EveryArgumentSplitsBackToItself,
      .area = "platform", .covers = {"SHIM-ARGV-INTACT"}) {
    for (const auto& arg : kHard)
        EXPECT_EQ(split_like_windows(p::windows_quote_arg(arg)), V{arg}) << p::windows_quote_arg(arg);
    V argv{"C:\\tools\\prog.exe"};
    argv.insert(argv.end(), kHard.begin(), kHard.end());
    const auto line = p::windows_command_line(argv);
    auto split = split_like_windows(line);
    EXPECT_EQ(split, argv) << line;
}

XTEST(WindowsCommandLine, QuotesOnlyWhatTheSplitterReads,
      .area = "platform", .covers = {"SHIM-ARGV-INTACT"}) {
    EXPECT_EQ(p::windows_quote_arg("plain"), "plain");
    EXPECT_EQ(p::windows_quote_arg(""), "\"\"");
    EXPECT_EQ(p::windows_quote_arg("a b"), "\"a b\"");
    EXPECT_EQ(p::windows_quote_arg("a\\b"), "a\\b") << "a backslash not before a quote is literal";
    EXPECT_EQ(p::windows_quote_arg("a b\\"), "\"a b\\\\\"") << "doubled before the closing quote";
    EXPECT_EQ(p::windows_quote_arg("say \"hi\""), "\"say \\\"hi\\\"\"");
    EXPECT_EQ(p::windows_quote_arg("x&y%z^"), "x&y%z^") << "no interpreter reads a directly created command line";
}

XTEST(WindowsCommandLine, ACommandScriptRefusesWhatItsInterpreterReads,
      .area = "platform", .covers = {"SHIM-SCRIPT-ARGV"}) {
    EXPECT_TRUE(p::is_command_script("C:\\x\\run.bat"));
    EXPECT_TRUE(p::is_command_script("C:\\x\\RUN.CMD"));
    EXPECT_FALSE(p::is_command_script("C:\\x\\run.exe"));
    EXPECT_FALSE(p::is_command_script("run"));
    EXPECT_EQ(p::command_script_problem(V{"C:\\x\\run.exe", "a&b", "%x%"}), "") << "a program is not a script";
    EXPECT_EQ(p::command_script_problem(V{"C:\\x\\run.bat", "two words", "a=b", "trail\\", "!x!"}), "");
    for (const auto* bad : {"a&b", "50%", "x^y", "a|b", "a<b", "a>b", "say \"hi\"", "line\nbreak", "cr\rx"}) {
        const auto problem = p::command_script_problem(V{"C:\\x\\run.cmd", "fine", bad});
        EXPECT_NE(problem.find("argument 2"), std::string::npos) << bad << ": " << problem;
        EXPECT_NE(problem.find("run.cmd"), std::string::npos) << problem;
    }
    EXPECT_NE(p::command_interpreter_problem(V{"ok", "a&b"}).find("`&`"), std::string::npos);
    EXPECT_EQ(p::command_interpreter_problem(V{"ok", "two words"}), "");
}

XTEST(WindowsCommandLine, ACommandScriptLineIsWhatCmdReads,
      .area = "platform", .covers = {"SHIM-SCRIPT-ARGV"}) {
    EXPECT_EQ(p::windows_script_command_line(V{"C:\\x y\\run.bat", "a", "b c", "d=e", "t\\", ""}),
              "/d /v:off /s /c \"\"C:\\x y\\run.bat\" a \"b c\" \"d=e\" t\\ \"\"\"");
}
