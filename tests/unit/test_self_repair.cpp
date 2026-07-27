// tests/unit/test_self_repair.cpp — the `self doctor --fix` repair ladder.
//
// The ladder is the part of the repair path that can make a user worse off
// than before (R3 removes a package and then reinstalls it), so it is written
// as a pure decision table over an injected command runner and tested as one.
// Every case below asserts BOTH the verdict and the exact commands run: a
// rung that quietly skipped its work and returned "healed" would otherwise
// pass, which is this codebase's recurring failure shape.

#include <gtest/gtest.h>

import std;
import xlings.core.xself.repair;

using xlings::xself::RepairKind;
using xlings::xself::RepairPolicy;
using xlings::xself::RepairTask;
using xlings::xself::is_shell_safe_token;
using xlings::xself::migration_hint;
using xlings::xself::probe_reinstallable;
using xlings::xself::repair_one;

namespace {

// Records every command and answers from a scripted list of exit codes.
// Anything past the end of the script is an error rather than a default,
// because "ran one more command than expected" is exactly the bug worth
// catching in a ladder.
struct FakeRunner {
    std::vector<std::string> ran;
    std::vector<int>         codes;
    std::size_t              next { 0 };
    bool                     overran { false };

    int operator()(const std::string& cmd) {
        ran.push_back(cmd);
        if (next >= codes.size()) { overran = true; return 0; }
        return codes[next++];
    }
};

RepairTask task(bool reinstallable = true) {
    return RepairTask{
        .kind          = RepairKind::BrokenPayload,
        .target        = "linux-headers",
        .version       = "5.11.1",
        .detail        = "executable not found",
        .reinstallable = reinstallable,
    };
}

auto runner_of(FakeRunner& f) {
    return [&f](const std::string& cmd) { return f(cmd); };
}

} // namespace

// ---------------------------------------------------------------- R2

TEST(SelfRepairLadder, ReRegisterAloneIsEnoughWhenInstallSucceeds) {
    FakeRunner f{.codes = {0}};
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f));

    EXPECT_TRUE(r.healed);
    EXPECT_EQ(r.rung, "re-register");
    ASSERT_EQ(f.ran.size(), 1u);
    EXPECT_EQ(f.ran[0], "xlings install linux-headers@5.11.1 -y");
    EXPECT_FALSE(f.overran);
}

// ---------------------------------------------------------------- R3

TEST(SelfRepairLadder, FallsBackToRemoveThenInstall) {
    FakeRunner f{.codes = {1, 0, 0}};   // install fails, remove ok, install ok
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f));

    EXPECT_TRUE(r.healed);
    EXPECT_EQ(r.rung, "reinstall");
    ASSERT_EQ(f.ran.size(), 3u);
    EXPECT_EQ(f.ran[0], "xlings install linux-headers@5.11.1 -y");
    EXPECT_EQ(f.ran[1], "xlings remove linux-headers@5.11.1 -y");
    EXPECT_EQ(f.ran[2], "xlings install linux-headers@5.11.1 -y");
}

// The outcome that leaves the user worse off than before the repair. It must
// be reported as a failure, must name the rung that did the damage, and must
// hand back the command that finishes the job.
TEST(SelfRepairLadder, RemovedButCouldNotReinstallIsLoudAndActionable) {
    FakeRunner f{.codes = {1, 0, 1}};   // install fails, remove ok, install fails
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f));

    EXPECT_FALSE(r.healed);
    EXPECT_EQ(r.rung, "reinstall");
    EXPECT_NE(r.note.find("REMOVED"), std::string::npos);
    EXPECT_NE(r.note.find("xlings install linux-headers@5.11.1"),
              std::string::npos);
}

TEST(SelfRepairLadder, DoesNotReinstallWhenTheEntryCannotBeRemoved) {
    FakeRunner f{.codes = {1, 1}};      // install fails, remove fails
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f));

    EXPECT_FALSE(r.healed);
    EXPECT_EQ(r.rung, "none");
    // Crucially: no second install. Reinstalling on top of a failed removal
    // is how a half-removed release gets a second registration.
    ASSERT_EQ(f.ran.size(), 2u);
}

// ------------------------------------------------- destructive-rung gates

TEST(SelfRepairLadder, NeverRemovesWhatTheIndexCannotSupplyAgain) {
    FakeRunner f{.codes = {1}};
    auto r = repair_one(task(/*reinstallable=*/false), RepairPolicy{},
                        runner_of(f));

    EXPECT_FALSE(r.healed);
    ASSERT_EQ(f.ran.size(), 1u) << "removal must not be attempted";
    EXPECT_NE(r.note.find("not available from the index"), std::string::npos);
}

TEST(SelfRepairLadder, ReinstallCanBeDisabledWithoutDisablingRepair) {
    FakeRunner f{.codes = {1}};
    RepairPolicy p{.allowNetwork = true, .allowReinstall = false};
    auto r = repair_one(task(), p, runner_of(f));

    EXPECT_FALSE(r.healed);
    ASSERT_EQ(f.ran.size(), 1u) << "R2 still runs; only R3 is off";
    EXPECT_NE(r.note.find("not permitted"), std::string::npos);
}

TEST(SelfRepairLadder, ALocalOnlyPassRunsNothing) {
    FakeRunner f{.codes = {0}};
    RepairPolicy p{.allowNetwork = false};
    auto r = repair_one(task(), p, runner_of(f));

    EXPECT_FALSE(r.healed);
    EXPECT_EQ(r.rung, "none");
    EXPECT_TRUE(f.ran.empty());
}

// ---------------------------------------------------------------- shell safety

// Names come from the versions DB, which recipes write, and the ladder builds
// command lines for std::system. A metacharacter here is not a repair case.
TEST(SelfRepairLadder, RefusesNamesThatAreNotSafeForAShell) {
    for (const auto& bad : {"gcc; rm -rf /", "gcc$(id)", "gcc`id`",
                            "gcc|tee", "gcc&", "gcc>out", "a b", "-rf"}) {
        FakeRunner f{.codes = {0}};
        auto t = task();
        t.target = bad;
        auto r = repair_one(t, RepairPolicy{}, runner_of(f));

        EXPECT_FALSE(r.healed) << bad;
        EXPECT_TRUE(f.ran.empty()) << "ran a command for: " << bad;
        EXPECT_NE(r.note.find("not safe"), std::string::npos) << bad;
    }
}

TEST(SelfRepairLadder, RefusesVersionsThatAreNotSafeForAShell) {
    FakeRunner f{.codes = {0}};
    auto t = task();
    t.version = "1.0; touch /tmp/pwned";
    auto r = repair_one(t, RepairPolicy{}, runner_of(f));

    EXPECT_FALSE(r.healed);
    EXPECT_TRUE(f.ran.empty());
}

// ---------------------------------------------------------------- probe

TEST(SelfRepairProbe, ReinstallabilityComesFromInfoNotFromInstallsExitCode) {
    FakeRunner ok{.codes = {0}};
    EXPECT_TRUE(probe_reinstallable("llvm", "20.1.7", runner_of(ok)));
    ASSERT_EQ(ok.ran.size(), 1u);
    EXPECT_NE(ok.ran[0].find("xlings info llvm@20.1.7"), std::string::npos);
    // The probe's output must not land in doctor's report.
    EXPECT_NE(ok.ran[0].find(">"), std::string::npos);

    FakeRunner absent{.codes = {1}};
    EXPECT_FALSE(probe_reinstallable("nope", "1.0", runner_of(absent)));
}

TEST(SelfRepairProbe, RefusesToProbeUnsafeNames) {
    FakeRunner f{.codes = {0}};
    EXPECT_FALSE(probe_reinstallable("gcc; id", "1.0", runner_of(f)));
    EXPECT_TRUE(f.ran.empty());
}

// ---------------------------------------------------------------- hint

TEST(SelfRepairHint, AppearsOnlyWhenTheRecordedClientDiffers) {
    auto hint = migration_hint("v0.4.69", "2026.7.28.1");
    ASSERT_TRUE(hint.has_value());
    EXPECT_NE(hint->find("0.4.69"), std::string::npos);
    EXPECT_NE(hint->find("xlings self doctor --fix"), std::string::npos);
}

// After a successful --fix stamps the field, the hint has to stop. The `v`
// prefix is written by self install and not by the stamp, so the comparison
// must ignore it or the nag would survive its own cure.
TEST(SelfRepairHint, StopsOnceTheVersionsAgree) {
    EXPECT_FALSE(migration_hint("2026.7.28.1", "2026.7.28.1").has_value());
    EXPECT_FALSE(migration_hint("v2026.7.28.1", "2026.7.28.1").has_value());
    EXPECT_FALSE(migration_hint("2026.7.28.1", "v2026.7.28.1").has_value());
}

// A home with no recorded version has nothing to compare against; nagging
// about it would be permanent and unactionable.
TEST(SelfRepairHint, SaysNothingWhenThereIsNoRecord) {
    EXPECT_FALSE(migration_hint("", "2026.7.28.1").has_value());
    EXPECT_FALSE(migration_hint("v", "2026.7.28.1").has_value());
}

TEST(SelfRepairShellSafety, AcceptsTheShapesRealPackagesUse) {
    EXPECT_TRUE(is_shell_safe_token("gcc"));
    EXPECT_TRUE(is_shell_safe_token("linux-headers"));
    EXPECT_TRUE(is_shell_safe_token("libc++.so.1"));
    EXPECT_TRUE(is_shell_safe_token("xim:llvm"));
    EXPECT_TRUE(is_shell_safe_token("16.1.0"));
    EXPECT_TRUE(is_shell_safe_token("glibc-2.39"));
    EXPECT_TRUE(is_shell_safe_token("2026.7.28.1"));

    EXPECT_FALSE(is_shell_safe_token(""));
    EXPECT_FALSE(is_shell_safe_token("--yes"));
    EXPECT_FALSE(is_shell_safe_token("a\nb"));
}
