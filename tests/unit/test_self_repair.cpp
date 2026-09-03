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
import xlings.core.xself.update;

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

// ------------------------------------------- R3 removal actually removed
//
// `xlings remove` exits 0 on a multi-subos home while leaving the record in
// place: it detaches the current subos and keeps the version whenever another
// subos still references it. R3 read that zero as "the entry is gone" and
// reported the version REMOVED -- a claim about the user's disk that was not
// true, made in the one message they are most likely to act on.

TEST(SelfRepairLadder, DoesNotClaimRemovalWhenTheRecordSurvives) {
    FakeRunner f{.codes = {1, 0, 0}};   // install fails, remove 0, reinstall 0
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f),
                        [](const std::string&, const std::string&) {
                            return false;   // still registered
                        });

    EXPECT_FALSE(r.healed);
    EXPECT_EQ(r.note.find("REMOVED"), std::string::npos)
        << "must not claim a removal that did not happen: " << r.note;
    EXPECT_NE(r.note.find("still registered"), std::string::npos);
    EXPECT_NE(r.note.find("another subos"), std::string::npos);
}

// A remove that exited 0 must ALWAYS be followed by the install, whatever the
// verifier says.
//
// The verifier used to short-circuit here, and that made the ladder perform
// the destructive half of remove-and-reinstall and skip the half that puts it
// back. On a real home it uninstalled a working `musl-gcc`: the recipe names
// targets the release does not own, those are skipped, so the finding's entry
// outlived the removal -- and the ladder read "records survived" as a reason
// not to reinstall the package it had just taken out.
TEST(SelfRepairLadder, ReinstallsEvenWhenTheRecordSurvivedRemoval) {
    FakeRunner f{.codes = {1, 0, 0}};
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f),
                        [](const std::string&, const std::string&) {
                            return false;   // still registered
                        });

    ASSERT_EQ(f.ran.size(), 3u) << "the package was removed and left out";
    EXPECT_EQ(f.ran[1], "xlings remove linux-headers@5.11.1 -y");
    EXPECT_EQ(f.ran[2], "xlings install linux-headers@5.11.1 -y");
    EXPECT_EQ(r.rung, "reinstall");
}

// ...and when that reinstall fails, the message is the one that says the user
// is now worse off, not the survivor diagnosis.
TEST(SelfRepairLadder, ReportsRemovedWhenTheFollowUpInstallFails) {
    FakeRunner f{.codes = {1, 0, 1}};
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f),
                        [](const std::string&, const std::string&) {
                            return true;
                        });

    EXPECT_FALSE(r.healed);
    EXPECT_EQ(r.rung, "reinstall");
    EXPECT_NE(r.note.find("REMOVED but could not reinstall"),
              std::string::npos) << r.note;
}

TEST(SelfRepairLadder, ProceedsWhenTheVerifierConfirmsTheRemoval) {
    FakeRunner f{.codes = {1, 0, 0}};
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f),
                        [](const std::string& t, const std::string& v) {
                            EXPECT_EQ(t, "linux-headers");
                            EXPECT_EQ(v, "5.11.1");
                            return true;    // really gone
                        });

    EXPECT_TRUE(r.healed);
    EXPECT_EQ(r.rung, "reinstall");
    ASSERT_EQ(f.ran.size(), 3u);
}

// The verifier only ever runs after a removal, so a repair that never reaches
// R3 must not pay for it -- and, more importantly, a caller passing one must
// not change what R2 does.
TEST(SelfRepairLadder, VerifierIsNotConsultedWhenReRegisterSucceeds) {
    FakeRunner f{.codes = {0}};
    bool consulted = false;
    auto r = repair_one(task(), RepairPolicy{}, runner_of(f),
                        [&](const std::string&, const std::string&) {
                            consulted = true;
                            return true;
                        });

    EXPECT_TRUE(r.healed);
    EXPECT_EQ(r.rung, "re-register");
    EXPECT_FALSE(consulted);
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

// ------------------------------------------------- which client runs it

// The ladder must be driven by the client that decided what to repair, not by
// whatever `xlings` happens to resolve to on PATH. They differ whenever doctor
// was started by absolute path -- which is exactly how the upgrade simulation
// runs a candidate build, so the released client on PATH would otherwise be
// the one doing the work.
TEST(SelfRepairLadder, UsesTheInjectedClientPathForEveryRung) {
    FakeRunner f{.codes = {1, 0, 0}};   // force all three commands
    RepairPolicy p;
    p.client = "/opt/xlings/bin/xlings";
    auto r = repair_one(task(), p, runner_of(f));

    EXPECT_TRUE(r.healed);
    ASSERT_EQ(f.ran.size(), 3u);
    for (const auto& cmd : f.ran) {
        EXPECT_EQ(cmd.rfind("/opt/xlings/bin/xlings ", 0), 0u)
            << "rung did not use the injected client: " << cmd;
    }
}

TEST(SelfRepairProbe, UsesTheInjectedClientToo) {
    FakeRunner f{.codes = {0}};
    EXPECT_TRUE(probe_reinstallable("llvm", "20.1.7", runner_of(f),
                                    "/opt/xlings/bin/xlings"));
    ASSERT_EQ(f.ran.size(), 1u);
    EXPECT_EQ(f.ran[0].rfind("/opt/xlings/bin/xlings info llvm@20.1.7", 0), 0u);
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

// ── `self update` must not report success without updating (#554) ────────
//
// Measured on a real home the day 2026.8.17.1 shipped: `self update` exited 0
// and left the user on 0.4.51. `use xlings latest` resolves WITHIN the
// currently active provider, so a home that ever carried a `local:` build
// keeps re-picking it -- and `use` returns 0 because it did activate
// something.
//
// The rule is about the PROVIDER, not the version. Both directions matter and
// the wrong implementation passes only one of them.
TEST(SelfUpdateLanding, ABareVersionIsTheIndexBuild) {
    EXPECT_TRUE(xlings::xself::update_landed_on_index_build("2026.8.17.1"));
    EXPECT_TRUE(xlings::xself::update_landed_on_index_build("0.4.51"));
}

TEST(SelfUpdateLanding, ALocalBuildIsNot) {
    EXPECT_FALSE(xlings::xself::update_landed_on_index_build("local:0.4.51"));
}

// Since 2026.9.2.1 version keys are spelled by identity, so an install from
// the default index may record `xim:2026.9.2.1` -- and a sub-index records its
// own namespace. Both ARE index builds. Judging them by "has a colon" reported
// a successful upgrade as "nothing was upgraded" on a real home (#579).
TEST(SelfUpdateLanding, ANamespacedIndexBuildStillIs) {
    EXPECT_TRUE(xlings::xself::update_landed_on_index_build("xim:2026.9.2.1"));
    EXPECT_TRUE(xlings::xself::update_landed_on_index_build("scode:1.0"));
}

// The false positive that the obvious implementation ships with: an
// already-current home changes nothing, and that is success. A check on "did
// the version move" fails every no-op update.
TEST(SelfUpdateLanding, AnAlreadyCurrentHomeIsNotAFailure) {
    EXPECT_TRUE(xlings::xself::update_landed_on_index_build("2026.8.17.1"))
        << "running `self update` twice must not start failing the second time";
}

// No observation is not a verdict -- the same rule entry_binary::version_of
// follows. A workspace that records no active version has a different defect,
// and this command must not claim to have diagnosed it.
TEST(SelfUpdateLanding, NoRecordedActiveVersionIsNotAVerdict) {
    EXPECT_TRUE(xlings::xself::update_landed_on_index_build(""));
}
