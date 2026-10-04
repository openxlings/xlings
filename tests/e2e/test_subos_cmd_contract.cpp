// `subos use <name> --sandbox --cmd <c>`: the command's exit code is the
// exit code, and the command saw the sandbox's home (T1's proof case).
//
// Replaces subos_cmd_contract_test.sh (macOS) and .ps1 (Windows), which were
// one contract written twice. The macOS copy never asserted anything: macOS
// ships bash 3.2, where a failing `[[ ]]` does not trip `set -e`, so the run
// on 2026-10-04 printed `cat: .../sandbox-home/marker: No such file` and then
// `ok`. The path it checked was never the one xlings uses. One C++ test, one
// set of assertions, three platforms.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

fs::path sandbox_home(const tk::Home& home, std::string_view subos) {
    return home.dir() / "subos" / subos / "home" / tk::current_user();
}

}  // namespace

XTEST(SubosCmdContract, ExitCodeAndHomeOnLinux,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"SUBOS-CMD-EXIT", "SUBOS-CMD-HOME"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    auto home = tk::Home::isolated("cmd-contract");
    ASSERT_TRUE(home.seed_sandbox_backend());
    auto created = home.xlings({"subos", "new", "probe"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();

    auto r = home.xlings({"subos", "use", "probe", "--sandbox", "--cmd",
                          R"(printf "%s" "$HOME" > "$HOME/marker"; exit 37)"});
    EXPECT_EQ(r.exit_code, 37) << r.transcript();
    // Inside, HOME is /home/<user>; outside, that is <subos>/home/<user>.
    EXPECT_EQ(tk::read_file(sandbox_home(home, "probe") / "marker"),
              "/home/" + tk::current_user());
}

XTEST(SubosCmdContract, ExitCodeAndHomeOnMacos,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"SUBOS-CMD-EXIT", "SUBOS-CMD-HOME"},
      .requires_ = {"macos", "xlings-bin"}) {
    auto home = tk::Home::isolated("cmd-contract");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "probe"}).exit_code, 0);

    auto r = home.xlings({"subos", "use", "probe", "--sandbox", "--cmd",
                          R"(printf "%s" "$HOME" > "$HOME/marker"; exit 37)"});
    EXPECT_EQ(r.exit_code, 37) << r.transcript();
    // home-redirect: HOME is the sandbox home's real path.
    auto expected = sandbox_home(home, "probe");
    EXPECT_EQ(fs::path(tk::read_file(expected / "marker")).lexically_normal(),
              expected.lexically_normal());
}

XTEST(SubosCmdContract, ExitCodeHomeAndCmdQuotingOnWindows,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"SUBOS-CMD-EXIT", "SUBOS-CMD-HOME", "SUBOS-CMD-WIN-QUOTING"},
      .requires_ = {"windows", "xlings-bin"}) {
    auto home = tk::Home::isolated("cmd-contract");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "probe"}).exit_code, 0);

    auto r = home.xlings({"subos", "use", "probe", "--sandbox", "--cmd",
        R"($env:USERPROFILE | Set-Content -NoNewline "$env:USERPROFILE\marker"; exit 37)"});
    EXPECT_EQ(r.exit_code, 37) << r.transcript();
    auto expected = sandbox_home(home, "probe");
    EXPECT_EQ(fs::path(tk::read_file(expected / "marker")), expected);

    // cmd.exe has its own quote / metacharacter grammar; the CRT quoting used
    // for PowerShell must not leak into what follows `cmd /s /c`.
    auto c = home.xlings({"subos", "use", "probe", "--sandbox", "--cmd",
                          R"(echo quoted^&pipe>"%USERPROFILE%\cmd-marker" & exit /b 37)"},
                         {{"XLINGS_SHELL", "cmd.exe"}});
    EXPECT_EQ(c.exit_code, 37) << c.transcript();
    auto text = tk::read_file(expected / "cmd-marker");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    EXPECT_EQ(text, "quoted&pipe");
}
