// tests/unit/test_xvm_owner.cpp — a payload path is a package identity, and
// the predicate that reads it as one.
//
// WHAT THESE DEFEND. The versions DB is keyed by xvm TARGET — a program name.
// Every other answer to "is this package installed" is keyed by package
// identity. `payload_path_names_another_package` is the bridge, and it is the
// fix for mcpp#533, where a `compat:libdrm` install was skipped because a
// `xim:libdrm` payload at the same upstream version was in the store: the
// install hook never ran, the tree it should have built did not exist, and the
// consumer's build failed four layers away in a linker command line.
//
// ⚠️ MOST OF THESE ASSERT A *false*. The predicate answers "can I PROVE this
// belongs to someone else", not "is this mine", and the difference is the whole
// design: a path that does not parse as a store coordinate proves nothing and
// must be accepted, or every self-managed tool and relocated payload would be
// reclassified as a mismatch and reinstalled forever. A predicate that only
// ever said "foreign" would pass the interesting-looking test below and fail
// every one of the negatives.

#include <gtest/gtest.h>

import std;
import xlings.core.xvm.owner;

namespace xvm = xlings::xvm;

namespace {

// The layout the installer writes, spelled out rather than composed from a
// helper: the layout IS the contract with coordinate_from_payload_path, and a
// shared helper would let the test and the code drift together.
std::string store_path(std::string_view ns, std::string_view name,
                       std::string_view version) {
    if (ns.empty())
        return std::format("/home/u/.xlings/data/xpkgs/{}/{}", name, version);
    return std::format("/home/u/.xlings/data/xpkgs/{}-x-{}/{}", ns, name,
                       version);
}

}  // namespace

// ─── the case that was wrong ────────────────────────────────────────────────

// mcpp#533 exactly: two packages, same short name, same upstream version, two
// namespaces. Mesa always pulls in the first, so the second was never built.
TEST(PayloadOwnership, SameShortNameInAnotherNamespaceIsAnotherPackage) {
    const auto ximPayload = store_path("xim", "libdrm", "2.4.123");

    EXPECT_TRUE(xvm::payload_path_names_another_package(ximPayload, "compat",
                                                        "libdrm"));
    // …and the coordinate recovered from it names the real owner, which is
    // what the diagnostic prints.
    auto coord = xvm::coordinate_from_payload_path(ximPayload);
    ASSERT_TRUE(coord.has_value());
    EXPECT_EQ(coord->canonical(), "xim:libdrm@2.4.123");
}

// The whole ecosystem shape, not one instance: these short names each occupy
// two or more namespaces in a real store today.
TEST(PayloadOwnership, EveryCrossNamespaceShortNameCollisionIsCaught) {
    struct Row {
        const char* owner;
        const char* asker;
        const char* name;
    };
    const Row rows[] = {
        {"xim", "compat", "expat"},   {"xim", "fromsource", "zlib"},
        {"xim", "local", "cairo"},    {"xim", "scode", "linux-headers"},
        {"compat", "xim", "libffi"},  {"fromsource", "xim", "libpng"},
    };
    for (const auto& r : rows) {
        SCOPED_TRACE(std::format("{} asking about {}:{}", r.owner, r.asker,
                                 r.name));
        EXPECT_TRUE(xvm::payload_path_names_another_package(
            store_path(r.owner, r.name, "1.0.0"), r.asker, r.name));
    }
    // Denominator: the loop above must actually have run.
    EXPECT_EQ(std::size(rows), 6u);
}

// ─── the cases that must stay accepted ──────────────────────────────────────

TEST(PayloadOwnership, OwnPayloadIsNotForeign) {
    EXPECT_FALSE(xvm::payload_path_names_another_package(
        store_path("compat", "libdrm", "2.4.123"), "compat", "libdrm"));
}

// A different VERSION of the same package is that package's own business. The
// caller has already resolved a version through the DB; this predicate answers
// identity only.
TEST(PayloadOwnership, DifferentVersionOfTheSamePackageIsNotForeign) {
    EXPECT_FALSE(xvm::payload_path_names_another_package(
        store_path("compat", "libdrm", "2.4.100"), "compat", "libdrm"));
}

// The compatibility guarantee. Anything that does not parse as a store
// coordinate proves nothing, so the caller keeps its previous behaviour.
TEST(PayloadOwnership, UnparseablePathsProveNothing) {
    const char* paths[] = {
        "",
        "/usr/local/bin/tool",              // installed outside the store
        "/home/u/.xlings/bin",              // no xpkgs component
        "relative/path/without/xpkgs",
    };
    for (const auto* p : paths) {
        SCOPED_TRACE(p);
        EXPECT_FALSE(
            xvm::payload_path_names_another_package(p, "compat", "libdrm"));
    }
    EXPECT_EQ(std::size(paths), 4u);
}

// An unnamespaced payload (`<store>/xpkgs/<name>/<version>`) is its own
// identity, and asking about it from a namespace is a genuine mismatch —
// but asking about it as unnamespaced is not.
TEST(PayloadOwnership, UnnamespacedPayloadsCompareOnTheEmptyNamespace) {
    const auto bare = store_path("", "libdrm", "2.4.123");
    EXPECT_FALSE(xvm::payload_path_names_another_package(bare, "", "libdrm"));
    EXPECT_TRUE(xvm::payload_path_names_another_package(bare, "compat",
                                                        "libdrm"));
}

// Windows-written records reach a Linux reader with backslashes, and that
// record is exactly the one nothing else can identify — so the predicate must
// not start guessing there either.
TEST(PayloadOwnership, SeparatorStyleDoesNotChangeTheAnswer) {
    const std::string win =
        R"(C:\Users\u\.xlings\data\xpkgs\xim-x-libdrm\2.4.123)";
    EXPECT_TRUE(
        xvm::payload_path_names_another_package(win, "compat", "libdrm"));
    EXPECT_FALSE(xvm::payload_path_names_another_package(win, "xim", "libdrm"));
}

// A payload path pointing INTO the package (a bindir) still identifies it:
// coordinate_from_payload_path keeps the two components after `xpkgs` and
// discards the rest, and the DB stores bindirs.
TEST(PayloadOwnership, BindirPathsStillIdentifyTheOwner) {
    const auto bindir =
        store_path("xim", "libdrm", "2.4.123") + "/bin";
    EXPECT_TRUE(
        xvm::payload_path_names_another_package(bindir, "compat", "libdrm"));
    EXPECT_FALSE(
        xvm::payload_path_names_another_package(bindir, "xim", "libdrm"));
}
