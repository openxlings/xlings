// Unit tests for the one answerer to "is this package installed".
//
// Design: .agents/docs/2026-08-11-five-issues-triage-and-plan.md
//
// The question had four answerers reading four sources, and on a real home all
// four disagreed at once. What these tests defend is not "the predicate is
// right" but the two directions it can be wrong in, which have very different
// costs:
//
//   * saying `installed` about a payload that registered nothing is how a
//     graphics stack wired to nothing reported success;
//   * saying `incomplete` about the 86 superseded payloads a normal home
//     accumulates would reinstall them forever.
//
// So most of these tests assert that something is NOT reported -- they are the
// falsification half. A predicate that only ever says "incomplete" would pass
// the interesting-looking tests and fail these.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

import std;
import xlings.core.xim.install_state;
import xlings.core.xim.payload;
import xlings.core.xvm.types;

namespace xim = xlings::xim;
namespace xvm = xlings::xvm;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_root(const std::string& leaf) {
    auto root = fs::temp_directory_path()
              / ("xlings-install-state-" + leaf
                 + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

// `<store>/xpkgs/<ns>-x-<name>/<version>` -- the layout `installation_state`
// recovers the package identity from. Writing it out rather than composing it
// from a helper is deliberate: the layout IS the contract with
// coordinate_from_payload_path, and a helper shared with the code under test
// would let both drift together.
fs::path payload_dir(const fs::path& home, const std::string& ns,
                     const std::string& name, const std::string& version) {
    auto dir = home / "data" / "xpkgs"
             / (ns.empty() ? name : ns + "-x-" + name) / version;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void write_file(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p);
    out << text;
}

// A ledger that references one payload directory, in the shape the real DB
// stores it: a target whose VData.path points into the payload.
xvm::VersionDB ledger_with(const std::string& target,
                           const std::string& versionKey,
                           const fs::path& path) {
    xvm::VersionDB db;
    xvm::VData data;
    data.path = path.string();
    data.kind = "lib";
    xvm::VInfo info;
    info.type = "lib";
    info.versions.emplace(versionKey, std::move(data));
    db.emplace(target, std::move(info));
    return db;
}

}  // namespace

// ── the direction that reports a problem ───────────────────────────────────

// The state that used to be unrecoverable. A hook that fails leaves a payload
// directory behind; every older probe answered "already installed" from that
// directory alone, so the hook never ran again and the failure was permanent.
TEST(InstallState, FailedInstallIsIncompleteAndRerunsTheHook) {
    const auto home = make_temp_root("failed");
    const auto dir = payload_dir(home, "xim", "demo", "1.0");
    write_file(dir / "lib" / "libdemo.so", "partial");
    xim::write_payload_failure_marker(dir, "1.0", "install hook returned false");

    const xim::LedgerIndex ledger(xvm::VersionDB{}, home.string());
    const auto state = xim::installation_state(ledger, "xim", "demo", "1.0", dir);

    EXPECT_TRUE(state.is_incomplete());
    EXPECT_TRUE(state.should_run_install_hook());
    fs::remove_all(home);
}

// The same failure, with nothing unpacked at all. Both shapes a failed install
// leaves must reach the same verdict, or the retry depends on how far the hook
// got before it died.
TEST(InstallState, FailedInstallWithEmptyPayloadIsAlsoIncomplete) {
    const auto home = make_temp_root("failed-empty");
    const auto dir = payload_dir(home, "xim", "demo", "1.0");
    xim::write_payload_failure_marker(dir, "1.0", "install hook returned false");

    const xim::LedgerIndex ledger(xvm::VersionDB{}, home.string());
    const auto state = xim::installation_state(ledger, "xim", "demo", "1.0", dir);

    EXPECT_TRUE(state.is_incomplete());
    // The marker must not make an empty directory look like a payload: that
    // confusion is what made the failure unrecoverable to begin with.
    EXPECT_FALSE(state.payloadPresent);
    fs::remove_all(home);
}

// An install that recorded work it cannot show. This is the graphics stack's
// shape on a real home: stamp present, `installed` reported, ledger empty.
TEST(InstallState, StampClaimingRegistrationsTheLedgerLacksIsIncomplete) {
    const auto home = make_temp_root("claimed");
    const auto dir = payload_dir(home, "xim", "libglvnd", "1.7.0.1");
    write_file(dir / "lib" / "libGLX.so.0", "payload");
    xim::write_payload_stamp(dir, "1.7.0.1", /*registered=*/12);

    const xim::LedgerIndex ledger(xvm::VersionDB{}, home.string());
    const auto state =
        xim::installation_state(ledger, "xim", "libglvnd", "1.7.0.1", dir);

    EXPECT_TRUE(state.is_incomplete());
    EXPECT_EQ(state.stampedRegistrations, 12);
    fs::remove_all(home);
}

// The other direction of divergence: records naming files that are gone.
TEST(InstallState, LedgerWithoutPayloadIsIncomplete) {
    const auto home = make_temp_root("no-payload");
    const auto dir = payload_dir(home, "xim", "demo", "1.0");
    const auto db = ledger_with("libdemo.so", "1.0", dir);

    const xim::LedgerIndex ledger(db, home.string());
    const auto state = xim::installation_state(ledger, "xim", "demo", "1.0", dir);

    EXPECT_TRUE(state.is_incomplete());
    fs::remove_all(home);
}

// ── the direction that must stay quiet ─────────────────────────────────────

// 86 payloads on the home this was written against are superseded versions
// whose ledger entry a newer install replaced: `linux-headers` with a full
// include tree, twenty-five old `xlings`. They have no stamp because they
// predate it. Reporting them would reinstall them forever -- the
// non-convergence this repo already has a postmortem about.
TEST(InstallState, PreStampPayloadWithNoLedgerEntryGetsNoVerdict) {
    const auto home = make_temp_root("pre-stamp");
    const auto dir = payload_dir(home, "xim", "linux-headers", "5.11.1");
    write_file(dir / "include" / "linux" / "types.h", "typedef int a;");

    const xim::LedgerIndex ledger(xvm::VersionDB{}, home.string());
    const auto state =
        xim::installation_state(ledger, "xim", "linux-headers", "5.11.1", dir);

    EXPECT_FALSE(state.is_incomplete());
    EXPECT_FALSE(state.should_run_install_hook());
    EXPECT_EQ(state.stampedRegistrations, xim::kRegisteredUnrecorded);
    fs::remove_all(home);
}

// A package that legitimately registers nothing declares it, and zero is a
// declaration -- not the absence of one.
TEST(InstallState, StampDeclaringZeroRegistrationsIsInstalled) {
    const auto home = make_temp_root("zero");
    const auto dir = payload_dir(home, "xim", "graphics", "0.1.3");
    write_file(dir / ".xpkg.lua", "-- meta package");
    xim::write_payload_stamp(dir, "0.1.3", /*registered=*/0);

    const xim::LedgerIndex ledger(xvm::VersionDB{}, home.string());
    const auto state =
        xim::installation_state(ledger, "xim", "graphics", "0.1.3", dir);

    EXPECT_TRUE(state.is_installed());
    EXPECT_FALSE(state.should_run_install_hook());
    fs::remove_all(home);
}

// The ordinary healthy case, and the one that must never pay for the checks
// above: payload on disk, ledger pointing into it.
TEST(InstallState, PayloadAndLedgerAgreeingIsInstalled) {
    const auto home = make_temp_root("healthy");
    const auto dir = payload_dir(home, "xim", "zlib", "1.3.1");
    write_file(dir / "lib" / "libz.so.1", "payload");
    xim::write_payload_stamp(dir, "1.3.1", /*registered=*/3);
    const auto db = ledger_with("libz.so.1", "1.3.1", dir / "lib");

    const xim::LedgerIndex ledger(db, home.string());
    const auto state = xim::installation_state(ledger, "xim", "zlib", "1.3.1", dir);

    EXPECT_TRUE(state.is_installed());
    EXPECT_TRUE(state.ledgerPresent);
    fs::remove_all(home);
}

// Nothing anywhere.
TEST(InstallState, NothingOnDiskAndNothingRecordedIsAbsent) {
    const auto home = make_temp_root("absent");
    const auto dir = payload_dir(home, "xim", "demo", "9.9");

    const xim::LedgerIndex ledger(xvm::VersionDB{}, home.string());
    const auto state = xim::installation_state(ledger, "xim", "demo", "9.9", dir);

    EXPECT_EQ(state.state, xim::InstallState::Absent);
    EXPECT_TRUE(state.should_run_install_hook());
    fs::remove_all(home);
}

// A ledger entry for a DIFFERENT version must not vouch for this one. The
// store keys payloads by version and so must the predicate; without this the
// upgrade case ("0.1.1 is registered, 0.1.2 is a wreck") reads as healthy.
TEST(InstallState, LedgerEntryForAnotherVersionDoesNotCount) {
    const auto home = make_temp_root("other-version");
    const auto older = payload_dir(home, "xim", "demo", "0.1.1");
    const auto newer = payload_dir(home, "xim", "demo", "0.1.2");
    write_file(older / "lib" / "libdemo.so", "old");
    write_file(newer / "lib" / "libdemo.so", "new");
    xim::write_payload_stamp(newer, "0.1.2", /*registered=*/5);
    const auto db = ledger_with("libdemo.so", "0.1.1", older / "lib");

    const xim::LedgerIndex ledger(db, home.string());
    EXPECT_TRUE(ledger.references("xim", "demo", "0.1.1"));
    EXPECT_FALSE(ledger.references("xim", "demo", "0.1.2"));
    EXPECT_TRUE(
        xim::installation_state(ledger, "xim", "demo", "0.1.2", newer)
            .is_incomplete());
    fs::remove_all(home);
}

// ── the stamp itself ───────────────────────────────────────────────────────

// Absent is not zero. A stamp written before the field existed describes a run
// we never observed, and a default in place of an observation states something
// about it we do not know.
TEST(InstallState, StampWithoutTheFieldReportsUnrecordedNotZero) {
    const auto home = make_temp_root("unrecorded");
    const auto dir = payload_dir(home, "xim", "demo", "1.0");
    write_file(dir / "lib" / "libdemo.so", "payload");
    xim::write_payload_stamp(dir, "1.0");   // old signature, no count

    EXPECT_EQ(xim::stamped_registration_count(dir), xim::kRegisteredUnrecorded);
    EXPECT_NE(xim::stamped_registration_count(dir), 0);
    fs::remove_all(home);
}

// A successful install must clear a previous failure, or the marker outlives
// the repair and the package is stuck the other way round.
TEST(InstallState, SuccessfulStampClearsAnEarlierFailureMarker) {
    const auto home = make_temp_root("clears");
    const auto dir = payload_dir(home, "xim", "demo", "1.0");
    write_file(dir / "lib" / "libdemo.so", "payload");
    xim::write_payload_failure_marker(dir, "1.0", "install hook returned false");
    ASSERT_TRUE(xim::stamped_incomplete(dir));

    xim::write_payload_stamp(dir, "1.0", /*registered=*/2);
    EXPECT_FALSE(xim::stamped_incomplete(dir));
    fs::remove_all(home);
}

// The failure marker is diagnostic, and the reader is hand-written. A reason
// carrying a quote must not make the stamp unreadable -- an unparseable stamp
// reads as "no failure recorded", which is the failure hiding itself.
TEST(InstallState, FailureReasonWithQuotesStaysReadable) {
    const auto home = make_temp_root("quotes");
    const auto dir = payload_dir(home, "xim", "demo", "1.0");
    write_file(dir / "lib" / "libdemo.so", "payload");
    xim::write_payload_failure_marker(
        dir, "1.0", "hook said \"no\"\nand stopped");

    EXPECT_TRUE(xim::stamped_incomplete(dir));
    fs::remove_all(home);
}
