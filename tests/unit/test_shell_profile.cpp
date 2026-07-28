// tests/unit/test_shell_profile.cpp — the shell-profile hook registry.
//
// `self install` has to hook every shell that will later start an xlings
// session. On Windows there is more than one: Windows PowerShell 5.1
// (`powershell`) and PowerShell 7+ (`pwsh`) read DIFFERENT startup files, and
// `xlings subos use` spawns pwsh in preference to powershell — so hooking only
// the 5.1 profile leaves the shell xlings itself launches un-hooked (#387).
//
// Everything here is platform-neutral on purpose: the host list is data, the
// probe is injected, and the file mutation is ordinary C++. That is what makes
// the Windows-only policy testable on the Linux CI runner, and it is why the
// old inline `powershell -Command "...Add-Content..."` one-liner had no tests
// at all.
//
// Every case asserts the observable effect, not just the verdict — a hook that
// reported Added while appending nothing is exactly this codebase's recurring
// failure shape.

#include <gtest/gtest.h>

import std;
import xlings.core.xself.shell_profile;

namespace fs = std::filesystem;
namespace sp = xlings::xself::shell_profile;

using sp::HookResult;

namespace {

// Answers each probe command from a scripted table and records what it was
// asked. Anything not in the table is "host not installed" (nullopt), which
// is what a real runner reports for a missing pwsh.
struct FakeRunner {
    std::map<std::string, std::string>  replies;
    mutable std::vector<std::string>    ran;

    std::optional<std::string> operator()(const std::string& cmd) const {
        ran.push_back(cmd);
        if (auto it = replies.find(cmd); it != replies.end()) return it->second;
        return std::nullopt;
    }
};

// A scratch directory per test, removed on teardown.
class ShellProfileFileTest : public ::testing::Test {
protected:
    fs::path dir;

    void SetUp() override {
        auto base = fs::temp_directory_path() / "xlings-shell-profile-test";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base);
        dir = base;
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string read(const fs::path& p) const {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
    void write(const fs::path& p, std::string_view content) const {
        std::ofstream out(p, std::ios::binary);
        out << content;
    }
};

const std::vector<std::string_view> kHosts { "powershell", "pwsh" };

} // namespace

// ─── probe_command ──────────────────────────────────────────────────

// -NoProfile matters: without it the probe would source the very profile it
// is about to edit, and on a half-configured home that profile can fail.
// -NonInteractive keeps a misbehaving profile from parking on a prompt.
TEST(ShellProfileProbe, ProbeCommandStartsHostWithoutItsOwnProfile) {
    auto cmd = sp::probe_command("pwsh");
    EXPECT_TRUE(cmd.starts_with("pwsh ")) << cmd;
    EXPECT_NE(cmd.find("-NoProfile"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("-NonInteractive"), std::string::npos) << cmd;
    EXPECT_NE(cmd.find("$PROFILE"), std::string::npos) << cmd;
}

// The answer is tagged rather than read as "whatever the process printed":
// the runner merges stderr into stdout, so an unrelated warning line would
// otherwise be adopted as a profile path and written to.
TEST(ShellProfileProbe, ProbeCommandTagsTheAnswer) {
    EXPECT_NE(sp::probe_command("powershell").find(sp::kProbePrefix),
              std::string::npos);
}

// ─── parse_probe_output ─────────────────────────────────────────────

TEST(ShellProfileParse, ReadsTheTaggedPath) {
    auto p = sp::parse_probe_output(
        "XLINGS_PROFILE=C:\\Users\\me\\Documents\\PowerShell\\Microsoft.PowerShell_profile.ps1\n");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->string(),
              "C:\\Users\\me\\Documents\\PowerShell\\Microsoft.PowerShell_profile.ps1");
}

// The failure this guards: a deprecation warning printed before the answer
// used to be indistinguishable from the answer itself.
TEST(ShellProfileParse, IgnoresUntaggedNoise) {
    auto p = sp::parse_probe_output(
        "WARNING: module 'foo' shadowed\n"
        "XLINGS_PROFILE=C:\\p\\profile.ps1\n"
        "trailing chatter\n");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->string(), "C:\\p\\profile.ps1");
}

// _popen hands back CRLF; an unstripped CR becomes part of the filename.
TEST(ShellProfileParse, StripsCarriageReturnAndSurroundingSpace) {
    auto p = sp::parse_probe_output("XLINGS_PROFILE=  C:\\p\\profile.ps1  \r\n");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->string(), "C:\\p\\profile.ps1");
}

TEST(ShellProfileParse, RejectsOutputWithoutTheTag) {
    EXPECT_FALSE(sp::parse_probe_output("'pwsh' is not recognized\n").has_value());
    EXPECT_FALSE(sp::parse_probe_output("").has_value());
}

// A host that answers with an empty $PROFILE has nothing to hook; treating
// that as a path would create a file named after the current directory.
TEST(ShellProfileParse, RejectsBlankTaggedValue) {
    EXPECT_FALSE(sp::parse_probe_output("XLINGS_PROFILE=\n").has_value());
    EXPECT_FALSE(sp::parse_probe_output("XLINGS_PROFILE=   \r\n").has_value());
}

// ─── resolve_targets ────────────────────────────────────────────────

TEST(ShellProfileResolve, ReturnsOneTargetPerHostThatAnswers) {
    FakeRunner run {{
        {sp::probe_command("powershell"), "XLINGS_PROFILE=C:\\ps5\\profile.ps1\r\n"},
        {sp::probe_command("pwsh"),       "XLINGS_PROFILE=C:\\ps7\\profile.ps1\r\n"},
    }};

    auto targets = sp::resolve_targets(kHosts, std::ref(run));

    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].host, "powershell");
    EXPECT_EQ(targets[0].path.string(), "C:\\ps5\\profile.ps1");
    EXPECT_EQ(targets[1].host, "pwsh");
    EXPECT_EQ(targets[1].path.string(), "C:\\ps7\\profile.ps1");
}

// The regression under test, from the other side: a machine WITH pwsh must
// yield a pwsh target. Before the fix there was no pwsh entry to yield.
TEST(ShellProfileResolve, HooksPwshEvenWhenItsProfileDiffersFromPowerShell5) {
    FakeRunner run {{
        {sp::probe_command("powershell"),
         "XLINGS_PROFILE=C:\\U\\Documents\\WindowsPowerShell\\Microsoft.PowerShell_profile.ps1\r\n"},
        {sp::probe_command("pwsh"),
         "XLINGS_PROFILE=C:\\U\\Documents\\PowerShell\\Microsoft.PowerShell_profile.ps1\r\n"},
    }};

    auto targets = sp::resolve_targets(kHosts, std::ref(run));

    ASSERT_EQ(targets.size(), 2u);
    EXPECT_NE(targets[0].path, targets[1].path);
}

TEST(ShellProfileResolve, SkipsHostThatIsNotInstalled) {
    FakeRunner run {{
        {sp::probe_command("powershell"), "XLINGS_PROFILE=C:\\ps5\\profile.ps1\r\n"},
    }};

    auto targets = sp::resolve_targets(kHosts, std::ref(run));

    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].host, "powershell");
}

TEST(ShellProfileResolve, SkipsHostWhoseAnswerIsUnusable) {
    FakeRunner run {{
        {sp::probe_command("powershell"), "XLINGS_PROFILE=C:\\ps5\\profile.ps1\r\n"},
        {sp::probe_command("pwsh"),       "pwsh: command not found\r\n"},
    }};

    auto targets = sp::resolve_targets(kHosts, std::ref(run));

    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0].host, "powershell");
}

// The shipped host list is the thing install.cppm actually passes, so assert
// on it rather than only on the hand-rolled list the other cases use. pwsh
// missing from it is the whole of #387.
TEST(ShellProfileResolve, TheShippedHostListCoversBothWindowsPowerShellHosts) {
    FakeRunner run {{
        {sp::probe_command("powershell"), "XLINGS_PROFILE=C:\\ps5\\profile.ps1\r\n"},
        {sp::probe_command("pwsh"),       "XLINGS_PROFILE=C:\\ps7\\profile.ps1\r\n"},
    }};

    auto targets = sp::resolve_targets(sp::kPowerShellHosts, std::ref(run));

    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].host, "powershell");
    EXPECT_EQ(targets[1].host, "pwsh");
}

// Each host is started once. Probing pwsh twice would double a ~1s startup
// on every `self install` / `self update`.
TEST(ShellProfileResolve, StartsEachHostExactlyOnce) {
    FakeRunner run {{
        {sp::probe_command("powershell"), "XLINGS_PROFILE=C:\\ps5\\profile.ps1\r\n"},
        {sp::probe_command("pwsh"),       "XLINGS_PROFILE=C:\\ps7\\profile.ps1\r\n"},
    }};

    sp::resolve_targets(kHosts, std::ref(run));

    ASSERT_EQ(run.ran.size(), 2u);
    EXPECT_EQ(run.ran[0], sp::probe_command("powershell"));
    EXPECT_EQ(run.ran[1], sp::probe_command("pwsh"));
}

// ─── powershell_snippet ─────────────────────────────────────────────

TEST(ShellProfileSnippet, SourcesTheScriptBehindAnExistenceGuard) {
    auto s = sp::powershell_snippet("C:\\home\\config\\shell\\xlings-profile.ps1");
    EXPECT_NE(s.find("Test-Path"), std::string::npos) << s;
    EXPECT_NE(s.find("C:\\home\\config\\shell\\xlings-profile.ps1"), std::string::npos) << s;
}

// hook() is idempotent by looking for the marker in the file, so the snippet
// it writes must carry that marker.
TEST(ShellProfileSnippet, CarriesTheIdempotencyMarker) {
    auto s = sp::powershell_snippet("C:\\home\\config\\shell\\xlings-profile.ps1");
    EXPECT_NE(s.find(sp::kMarker), std::string::npos) << s;
}

// A quote in the home path used to close the PowerShell string literal early
// and leave a syntactically broken profile behind — every later shell then
// started with a parse error. PowerShell escapes ' inside '...' by doubling.
TEST(ShellProfileSnippet, EscapesSingleQuotesInThePath) {
    auto s = sp::powershell_snippet("C:\\Users\\o'brien\\.xlings\\xlings-profile.ps1");
    EXPECT_EQ(s.find("o'brien"), std::string::npos)
        << "raw quote left unescaped: " << s;
    EXPECT_NE(s.find("o''brien"), std::string::npos) << s;
}

// ─── hook ───────────────────────────────────────────────────────────

TEST_F(ShellProfileFileTest, AppendsToAnExistingProfileAndKeepsItsContent) {
    auto prof = dir / "Microsoft.PowerShell_profile.ps1";
    write(prof, "Set-Alias ll Get-ChildItem\n");

    auto r = sp::hook(prof, "if(Test-Path 'p'){. 'p'}  # xlings-profile");

    EXPECT_EQ(r, HookResult::Added);
    auto content = read(prof);
    EXPECT_TRUE(content.starts_with("Set-Alias ll Get-ChildItem\n")) << content;
    EXPECT_NE(content.find("xlings-profile"), std::string::npos) << content;
}

TEST_F(ShellProfileFileTest, SecondHookLeavesTheFileByteIdentical) {
    auto prof = dir / "profile.ps1";
    write(prof, "# user config\n");
    std::string snippet = "if(Test-Path 'p'){. 'p'}  # xlings-profile";

    ASSERT_EQ(sp::hook(prof, snippet), HookResult::Added);
    auto after_first = read(prof);

    EXPECT_EQ(sp::hook(prof, snippet), HookResult::AlreadyHooked);
    EXPECT_EQ(read(prof), after_first);
}

// A fresh Windows account has no Documents\PowerShell directory at all.
TEST_F(ShellProfileFileTest, CreatesTheProfileAndItsParentDirectories) {
    auto prof = dir / "Documents" / "PowerShell" / "Microsoft.PowerShell_profile.ps1";

    auto r = sp::hook(prof, "if(Test-Path 'p'){. 'p'}  # xlings-profile");

    EXPECT_EQ(r, HookResult::Added);
    ASSERT_TRUE(fs::exists(prof));
    EXPECT_NE(read(prof).find("xlings-profile"), std::string::npos);
}

// Failure must be reported, not swallowed: `self install` prints a manual
// fallback hint when it could not hook a shell, and cannot do that if a
// failed write is indistinguishable from a successful one.
TEST_F(ShellProfileFileTest, ReportsFailureWhenTheProfilePathIsUnusable) {
    auto blocker = dir / "Documents";
    write(blocker, "not a directory\n");
    auto prof = blocker / "PowerShell" / "profile.ps1";

    EXPECT_EQ(sp::hook(prof, "# xlings-profile"), HookResult::Failed);
}
