// Unit tests for the graphics wiring reader behind `xlings subos info`.
//
// Design: .agents/docs/2026-08-10-graphics-stack-architecture-review-and-plan.md
//
// The module takes a subos directory and reads two things — a symlink and a
// small text file — so every case here is a few files in a temp tree. No home,
// no store, no GPU.
//
// What these tests are actually defending: the graphics stack's failure mode
// is that it succeeds. Every assertion below is really the same assertion —
// that a stack which will silently fall back to another driver does not render
// as a healthy one.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

import std;
import xlings.core.subos.graphics;

namespace gfx = xlings::subos::graphics;
namespace fs = std::filesystem;

namespace {

// A subos with a `lib/libGLX.so.0` symlink into a fake libglvnd payload.
//
// The farm entry has to be a real symlink: following it IS what the reader
// does, and a regular file would test a path nothing takes. Creating one
// needs a privilege Windows does not grant by default, so `wire_dispatch`
// skips rather than fails there — the graphics stack is ELF-only and does not
// exist on Windows at all, while everything below that parses or formats the
// record still runs on every platform.
struct Tree {
    fs::path root;
    fs::path subos;
    fs::path payload;      // <root>/store/libglvnd
    fs::path vendorDir;    // <payload>/lib/glx-vendor

    explicit Tree(std::string_view tag) {
        root = fs::temp_directory_path() /
               ("xlings-gfx-" + std::string(tag) + "-" +
                std::to_string(counter_()));
        std::error_code ec;
        fs::remove_all(root, ec);
        subos = root / "subos" / "default";
        payload = root / "store" / "libglvnd";
        vendorDir = payload / "lib" / "glx-vendor";
        fs::create_directories(subos / "lib");
        fs::create_directories(payload / "lib");
    }
    ~Tree() { std::error_code ec; fs::remove_all(root, ec); }

    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;

    // The dispatch library, plus the farm symlink that points at it.
    [[nodiscard]] bool wire_dispatch() {
        auto real = payload / "lib" / "libGLX.so.0";
        std::ofstream(real) << "not really an elf";
        std::error_code ec;
        fs::create_symlink(real, subos / "lib" / "libGLX.so.0", ec);
        return !ec;
    }
    void add_vendor(std::string_view soname) {
        fs::create_directories(vendorDir);
        std::ofstream(vendorDir / std::string(soname)) << "vendor";
    }
    // The nvidia payload records the host driver version it was wired for.
    // The vendor entry is a symlink into that payload, so the stamp is reached
    // by following the link the loader would follow — the same discipline the
    // rest of this module uses instead of searching the store.
    void stamp_driver(std::string_view version) {
        fs::create_directories(vendorDir);
        auto nv = root / "store" / "nvidia";
        fs::create_directories(nv / "lib");
        std::ofstream(nv / "lib" / "libGLX_nvidia.so.0") << "vendor";
        std::ofstream(nv / ".host-driver-version") << version << "\n";
        std::error_code ec;
        fs::remove(vendorDir / "libGLX_nvidia.so.0", ec);
        fs::create_symlink(nv / "lib" / "libGLX_nvidia.so.0",
                           vendorDir / "libGLX_nvidia.so.0", ec);
    }
    void write_record(std::string_view text) {
        fs::create_directories(vendorDir);
        std::ofstream(vendorDir / ".wiring") << text;
    }

private:
    static int counter_() { static int n = 0; return ++n; }
};

#define WIRE_OR_SKIP(t)                                                       \
    if (!(t).wire_dispatch())                                                 \
    GTEST_SKIP() << "symlinks unavailable here; the graphics stack this "     \
                    "reads is ELF-only and does not exist on such a platform"

} // namespace

// ─── the three absences ────────────────────────────────────────────────────

// Most subos do no graphics. They must be untouched by this — an extra
// section on every `subos info` would be noise, and worse, a "graphics: none"
// row invites the reading that something is missing.
TEST(SubosGraphics, NoDispatchMeansTheSubosSimplyDoesNoGL) {
    Tree t("nodispatch");
    auto w = gfx::read_graphics_wiring(t.subos);
    EXPECT_EQ(w.status, gfx::WiringStatus::NoDispatch);
    EXPECT_FALSE(w.has_dispatch());
}

// The failure the whole mechanism exists to catch, and the one that needs no
// record: glvnd with an empty vendor directory dispatches to nothing, falls
// back to llvmpipe, and reports success.
TEST(SubosGraphics, DispatchWithNoVendorsIsTheSoftwareFallbackFailure) {
    Tree t("novendors");
    WIRE_OR_SKIP(t);
    auto w = gfx::read_graphics_wiring(t.subos);
    EXPECT_EQ(w.status, gfx::WiringStatus::NoVendors);
    EXPECT_TRUE(w.has_dispatch());
    EXPECT_EQ(w.vendorFiles, 0);
}

// A stack wired by a graphics recipe older than the probe. The vendors may
// well be fine — but nothing measured them, and reporting "ok" for an
// unmeasured vendor is the silent-success bug in a new place.
TEST(SubosGraphics, VendorsWithNoRecordAreUnrecordedNotHealthy) {
    Tree t("unrecorded");
    WIRE_OR_SKIP(t);
    t.add_vendor("libGLX_nvidia.so.0");
    t.add_vendor("libGLX_mesa.so.0");
    auto w = gfx::read_graphics_wiring(t.subos);
    EXPECT_EQ(w.status, gfx::WiringStatus::Unrecorded);
    EXPECT_EQ(w.vendorFiles, 2);
    EXPECT_TRUE(w.vendors.empty());
}

// A farm entry pointing at a payload that has been deleted is a BROKEN stack,
// not an absent one. `fs::exists` follows the link and answers no, which
// would report "this subos does no GL" about a subos wired to something gone.
TEST(SubosGraphics, DanglingDispatchSymlinkIsNotReadAsNoGraphics) {
    Tree t("dangling");
    WIRE_OR_SKIP(t);
    fs::remove_all(t.payload);
    auto w = gfx::read_graphics_wiring(t.subos);
    EXPECT_TRUE(w.has_dispatch());
    EXPECT_EQ(w.status, gfx::WiringStatus::NoVendors);
}

// ─── the record ────────────────────────────────────────────────────────────

TEST(SubosGraphics, RecordedStatesAreReadBackPerVendor) {
    Tree t("recorded");
    WIRE_OR_SKIP(t);
    t.add_vendor("libGLX_nvidia.so.0");
    t.write_record(
        "dispatch=" + t.payload.string() + "\n"
        "vendor=libGLX_nvidia.so.0 state=ok\n"
        "vendor=libEGL_nvidia.so.0 state=broken reason=runpath-not-transitive\n"
        "vendor=libGLESv2_nvidia.so.2 state=broken missing=libpthread.so.0,librt.so.1\n"
        "vendor=libGLX_mesa.so.0 state=native\n");

    auto w = gfx::read_graphics_wiring(t.subos);
    ASSERT_EQ(w.status, gfx::WiringStatus::Recorded);
    ASSERT_EQ(w.vendors.size(), 4u);
    EXPECT_FALSE(w.dispatchMismatch);

    EXPECT_TRUE(w.vendors[0].is_ok());
    EXPECT_TRUE(w.vendors[1].is_broken());
    EXPECT_EQ(w.vendors[1].reason, "runpath-not-transitive");
    EXPECT_TRUE(w.vendors[2].is_broken());
    ASSERT_EQ(w.vendors[2].missing.size(), 2u);
    EXPECT_EQ(w.vendors[2].missing[0], "libpthread.so.0");
    EXPECT_EQ(w.vendors[2].missing[1], "librt.so.1");
    // `native` passes, and it is a different fact from `ok`: no host driver
    // sits behind it, so there was no closure to complete.
    EXPECT_TRUE(w.vendors[3].is_ok());
    EXPECT_FALSE(w.vendors[3].is_broken());

    EXPECT_EQ(w.broken_count(), 2);
}

// The measured shape on a real NVIDIA host: GLX loads and renders on the GPU
// while all three of the same vendor's other entry points cannot load at all.
// A probe covering only what the GLX wiring touches called this stack healthy.
TEST(SubosGraphics, OneVendorCanBeGoodOnGLXAndBrokenOnEveryOtherEntryPoint) {
    Tree t("percontext");
    WIRE_OR_SKIP(t);
    t.write_record(
        "vendor=libGLX_nvidia.so.0 state=ok\n"
        "vendor=libEGL_nvidia.so.0 state=broken reason=runpath-not-transitive\n"
        "vendor=libGLESv1_CM_nvidia.so.1 state=broken reason=runpath-not-transitive\n"
        "vendor=libGLESv2_nvidia.so.2 state=broken reason=runpath-not-transitive\n");
    auto w = gfx::read_graphics_wiring(t.subos);
    ASSERT_EQ(w.vendors.size(), 4u);
    EXPECT_FALSE(w.vendors[0].is_broken());
    EXPECT_EQ(w.broken_count(), 3);
}

// The farm was rebuilt onto a different libglvnd without re-running the
// assembler, so the record describes libraries nobody here will load. Silence
// about that would present another stack's verdict as this one's.
TEST(SubosGraphics, RecordFromAnotherPayloadIsFlaggedAsStale) {
    Tree t("stale");
    WIRE_OR_SKIP(t);
    t.write_record(
        "dispatch=/somewhere/else/xim-x-libglvnd/1.6.0\n"
        "vendor=libGLX_nvidia.so.0 state=ok\n");
    auto w = gfx::read_graphics_wiring(t.subos);
    EXPECT_EQ(w.status, gfx::WiringStatus::Recorded);
    EXPECT_TRUE(w.dispatchMismatch);
}

// Both sides get canonicalized before comparing. A record written through a
// symlinked prefix (/tmp on macOS, a bind-mounted home) names the same
// payload by a different string, and a textual compare would report every
// such machine as stale. One-sided resolution has produced exactly this class
// of false finding here before.
TEST(SubosGraphics, EquivalentPathsSpelledDifferentlyAreNotStale) {
    Tree t("canonical");
    WIRE_OR_SKIP(t);
    auto viaDotDot = t.payload / "lib" / ".." ;
    t.write_record("dispatch=" + viaDotDot.string() + "\n"
                   "vendor=libGLX_mesa.so.0 state=native\n");
    auto w = gfx::read_graphics_wiring(t.subos);
    EXPECT_FALSE(w.dispatchMismatch);
}

// ─── parser robustness ─────────────────────────────────────────────────────

// A newer writer must be able to add a key without this reader reporting an
// empty stack. Forward compatibility here is not politeness: the index ships
// independently of the client, so a new field WILL reach an old xlings.
TEST(SubosGraphics, UnknownKeysAndLinesAreSkippedNotFatal) {
    auto rec = gfx::parse_wiring_record(
        "schema=2\n"
        "dispatch=/p\n"
        "# a comment nobody promised\n"
        "vendor=libGLX_nvidia.so.0 state=ok driver_version=550.144.03\n"
        "\n"
        "something-else\n");
    EXPECT_EQ(rec.dispatch, "/p");
    ASSERT_EQ(rec.vendors.size(), 1u);
    EXPECT_EQ(rec.vendors[0].state, "ok");
}

// A write interrupted mid-line leaves a vendor entry naming no library and
// carrying no state. Rendering it as a row with a blank name and an unknown
// verdict would put a fourth meaning on the panel; dropping it leaves the
// stack described by the lines that did land.
TEST(SubosGraphics, ATruncatedRecordDropsTheIncompleteEntry) {
    auto rec = gfx::parse_wiring_record(
        "vendor=libGLX_nvidia.so.0 state=ok\n"
        "vendor=");
    ASSERT_EQ(rec.vendors.size(), 1u);
    EXPECT_EQ(rec.vendors[0].soname, "libGLX_nvidia.so.0");
}

// A vendor line with no state must not read as a pass.
TEST(SubosGraphics, AMissingStateFailsClosed) {
    auto rec = gfx::parse_wiring_record("vendor=libEGL_nvidia.so.0\n");
    ASSERT_EQ(rec.vendors.size(), 1u);
    EXPECT_EQ(rec.vendors[0].state, "unverified");
    EXPECT_FALSE(rec.vendors[0].is_ok());
    EXPECT_FALSE(rec.vendors[0].is_broken());
}

// A state this client has never heard of is neither ok nor broken. Treating
// an unknown verdict as a pass is how a client older than the index turns a
// new failure category into silence.
// The verdict that depends on who opens it. `vendor_closure_gaps` judges the
// interposer in isolation and is right about the interposer; a consumer's
// DT_RPATH is transitive and covers the whole chain, so the same vendor is
// usable from an installed program and unusable from one the user just built.
// Measured: DT_RUNPATH consumer cannot open libEGL_nvidia, DT_RPATH loads it.
TEST(SubosGraphics, NeedsTransitiveConsumerIsNeitherOkNorBroken) {
    auto rec = gfx::parse_wiring_record(
        "vendor=libEGL_nvidia.so.0 state=needs-transitive-consumer\n");
    ASSERT_EQ(rec.vendors.size(), 1u);
    const auto& v = rec.vendors[0];
    EXPECT_TRUE(v.needs_transitive_consumer());
    // Not `ok`: a program the user builds here still cannot load it.
    EXPECT_FALSE(v.is_ok());
    // Not `broken` either: installed programs reach the GPU through it, and
    // calling that broken is the under-reporting this state exists to end.
    EXPECT_FALSE(v.is_broken());
}

// The description has to answer the question that brings someone to the panel:
// "the GL app I just compiled renders in software — why". Naming the tag alone
// would read as trivia.
TEST(SubosGraphics, NeedsTransitiveConsumerSaysWhichProgramsAreAffected) {
    gfx::VendorWiring v{"libEGL_nvidia.so.0", "needs-transitive-consumer", "", {}};
    auto d = gfx::describe(v);
    EXPECT_NE(d.find("installed"), std::string::npos);
    EXPECT_NE(d.find("build"), std::string::npos);
}

TEST(SubosGraphics, AnUnknownStateIsNeitherOkNorBroken) {
    auto rec = gfx::parse_wiring_record("vendor=libGLX_x.so.0 state=quarantined\n");
    ASSERT_EQ(rec.vendors.size(), 1u);
    EXPECT_FALSE(rec.vendors[0].is_ok());
    EXPECT_FALSE(rec.vendors[0].is_broken());
}

// Paths may contain spaces; `dispatch=` therefore takes the rest of its line.
TEST(SubosGraphics, ADispatchPathWithSpacesSurvives) {
    auto rec = gfx::parse_wiring_record("dispatch=/home/a b/xlings/store\n");
    EXPECT_EQ(rec.dispatch, "/home/a b/xlings/store");
}

// ─── display mapping ───────────────────────────────────────────────────────

TEST(SubosGraphics, EveryEntryPointGetsAReadableLabel) {
    struct { std::string_view soname, vendor, api; } cases[] = {
        {"libGLX_nvidia.so.0",       "nvidia", "GLX"},
        {"libEGL_nvidia.so.0",       "nvidia", "EGL"},
        {"libGLESv1_CM_nvidia.so.1", "nvidia", "GLESv1"},
        {"libGLESv2_nvidia.so.2",    "nvidia", "GLESv2"},
        {"libGLX_mesa.so.0",         "mesa",   "GLX"},
    };
    for (auto& c : cases) {
        auto l = gfx::label_for(c.soname);
        ASSERT_TRUE(l.has_value()) << c.soname;
        EXPECT_EQ(l->vendor, c.vendor);
        EXPECT_EQ(l->api, c.api);
    }
}

// An entry point this client does not know is shown verbatim rather than
// guessed at — the caller falls back to the SONAME when there is no label.
TEST(SubosGraphics, AnUnrecognizedSonameHasNoLabel) {
    EXPECT_FALSE(gfx::label_for("libGLX_.so.0").has_value());
    EXPECT_FALSE(gfx::label_for("libVulkan_nvidia.so.1").has_value());
}

// The description carries the consequence, not just the verdict. "broken" on
// its own reads as "this feature is unavailable"; what actually happens is
// that GL keeps working on a different driver and says nothing, which is why
// nobody noticed for so long.
TEST(SubosGraphics, ABrokenVendorSaysWhatWillActuallyHappen) {
    gfx::VendorWiring v{"libEGL_nvidia.so.0", "broken", "runpath-not-transitive", {}};
    auto d = gfx::describe(v);
    EXPECT_NE(d.find("BROKEN"), std::string::npos);
    EXPECT_NE(d.find("without saying so"), std::string::npos);
}

// The index ships independently of the client, so a recipe writing a verdict
// this xlings has never heard of is a matter of time. Showing the bare word
// would read as a state that is fine — the panel has to say it cannot
// interpret it.
TEST(SubosGraphics, AnUnknownStateSaysThisClientCannotReadIt) {
    gfx::VendorWiring v{"libGLX_x.so.0", "quarantined", "", {}};
    auto d = gfx::describe(v);
    EXPECT_NE(d.find("quarantined"), std::string::npos);
    EXPECT_NE(d.find("newer than this xlings"), std::string::npos);
    EXPECT_NE(d.find("unassessed"), std::string::npos);
}

// ─── host driver drift ─────────────────────────────────────────────────────
//
// The NVIDIA userspace driver is the one part of this stack we do not own: it
// is in lockstep with the host's kernel module, so we link to the host's files
// rather than shipping them, and a distribution update moves it under us. The
// wiring recorded at install then describes a driver that is no longer there.

TEST(SubosGraphics, AgreeingDriverVersionsAreNotDrift) {
    Tree t("drift-same");
    WIRE_OR_SKIP(t);
    t.add_vendor("libGLX_nvidia.so.0");
    t.stamp_driver("550.144.03");
    auto d = gfx::read_driver_stamp(t.vendorDir);
    EXPECT_TRUE(d.known);
    EXPECT_EQ(d.builtFor, "550.144.03");
}

// An unknown on either side is NOT drift. A machine whose kernel module is not
// loaded right now has not changed driver — it has no driver running — and
// reporting that as a change would cry wolf on every laptop with the GPU
// asleep, which is how the previous generation of hints in this codebase
// became noise and got commented out.
TEST(SubosGraphics, AnUnreadableSideIsNotDrift) {
    Tree t("drift-unknown");
    WIRE_OR_SKIP(t);
    t.add_vendor("libGLX_nvidia.so.0");
    t.stamp_driver("550.144.03");
    auto d = gfx::read_driver_stamp(t.vendorDir);
    // hostNow comes from /sys and is empty on any machine without the module.
    gfx::DriverStamp probe{true, "550.144.03", "", };
    EXPECT_FALSE(probe.drifted());
    gfx::DriverStamp nostamp{false, "", "560.1", };
    EXPECT_FALSE(nostamp.drifted());
    gfx::DriverStamp real{true, "550.144.03", "560.35.03", };
    EXPECT_TRUE(real.drifted()) << "a genuine version change must be reported";
    (void)d;
}

// A stack with no stamp at all (installed by a recipe older than the stamp)
// must not be reported as drifted — nobody recorded what it was built for.
TEST(SubosGraphics, NoStampIsNotDrift) {
    Tree t("drift-nostamp");
    WIRE_OR_SKIP(t);
    t.add_vendor("libGLX_nvidia.so.0");
    auto d = gfx::read_driver_stamp(t.vendorDir);
    EXPECT_FALSE(d.known);
    EXPECT_FALSE(d.drifted());
}

TEST(SubosGraphics, AMissingClosureNamesTheLibraries) {
    gfx::VendorWiring v{"libGLX_nvidia.so.0", "broken", "",
                        {"libpthread.so.0", "librt.so.1"}};
    auto d = gfx::describe(v);
    EXPECT_NE(d.find("libpthread.so.0"), std::string::npos);
    EXPECT_NE(d.find("librt.so.1"), std::string::npos);
}
