// Unit tests for owner-anchored shim dispatch (0.4.48).
// Design: .agents/docs/2026-06-04-shim-owner-anchoring-design.md
#include <gtest/gtest.h>

import std;
import xlings.core.xvm.shim;
import xlings.core.xvm.db;
import xlings.core.home_identity;
import xlings.platform;

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr const char* kXlingsBin = "xlings.exe";
#else
constexpr const char* kXlingsBin = "xlings";
#endif

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path()
             / ("xlings-anchor-test-" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path& p, std::string_view content = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream os(p);
    os << content;
}

// Build a minimal structural home at `root`: .xlings.json + bin/xlings +
// subos/default/bin. Returns the subos bin dir.
fs::path make_home(const fs::path& root, std::string_view versionsJson = "{}") {
    touch(root / ".xlings.json",
          std::string("{ \"activeSubos\": \"default\", \"versions\": ")
              + std::string(versionsJson) + " }");
    touch(root / "bin" / kXlingsBin);
    auto bin = root / "subos" / "default" / "bin";
    fs::create_directories(bin);
    return bin;
}

} // namespace

// ── home_identity::is_home ───────────────────────────────────────────

TEST(ShimAnchorHomeRoot, RealHomeMatches) {
    TempDir tmp;
    auto home = tmp.path / "home";
    make_home(home);
    EXPECT_TRUE(xlings::home_identity::is_home(home));
}

TEST(ShimAnchorHomeRoot, SubosDirDoesNotMatch) {
    TempDir tmp;
    auto home = tmp.path / "home";
    make_home(home);
    // subos/current carries a workspace .xlings.json and (as a shim) a
    // bin/xlings — but it has no subos/ inside it.
    auto current = home / "subos" / "current";
    touch(current / ".xlings.json", "{ \"workspace\": {} }");
    touch(current / "bin" / kXlingsBin);
    EXPECT_FALSE(xlings::home_identity::is_home(current));
}

// #617: every SubOS now has an empty `subos/` of its own, so a SubOS carries
// the whole legacy layout. It is still not a home -- with the parent marked,
// and with the parent recognised only by its layout.
TEST(ShimAnchorHomeRoot, SubosWithItsOwnSubosDirIsNotAHome) {
    TempDir tmp;
    for (bool marked : {false, true}) {
        auto home = tmp.path / (marked ? "marked" : "legacy");
        make_home(home);
        if (marked) ASSERT_TRUE(xlings::home_identity::write_marker(home));
        auto sub = home / "subos" / "v615";
        touch(sub / ".xlings.json", "{ \"workspace\": {} }");
        touch(sub / "bin" / kXlingsBin);
        fs::create_directories(sub / "subos");
        EXPECT_FALSE(xlings::home_identity::is_home(sub)) << "marked=" << marked;
        EXPECT_TRUE(xlings::home_identity::is_home(home)) << "marked=" << marked;
    }
}

TEST(ShimAnchorHomeRoot, ProjectStateDirDoesNotMatch) {
    TempDir tmp;
    // <project>/.xlings has subos/ but no bin/xlings — a project state
    // dir owns no payloads and must never anchor a shim.
    auto state = tmp.path / "project" / ".xlings";
    touch(state / ".xlings.json", "{}");
    fs::create_directories(state / "subos" / "_" / "bin");
    EXPECT_FALSE(xlings::home_identity::is_home(state));
}

// The marker alone makes a home, whatever the layout; a legacy home is adopted
// once and keeps its identity.
TEST(ShimAnchorHomeRoot, MarkerIsTheDeclarationAndAdoptionWritesItOnce) {
    TempDir tmp;
    auto bare = tmp.path / "bare";
    fs::create_directories(bare);
    EXPECT_FALSE(xlings::home_identity::is_home(bare));
    ASSERT_TRUE(xlings::home_identity::write_marker(bare));
    EXPECT_TRUE(xlings::home_identity::is_home(bare));

    auto legacy = tmp.path / "legacy";
    make_home(legacy);
    EXPECT_FALSE(xlings::home_identity::has_marker(legacy));
    EXPECT_TRUE(xlings::home_identity::adopt_legacy_home(legacy));
    ASSERT_TRUE(xlings::home_identity::has_marker(legacy));
    std::ifstream first(xlings::home_identity::marker_path(legacy));
    const std::string before{std::istreambuf_iterator<char>(first), {}};
    EXPECT_TRUE(xlings::home_identity::adopt_legacy_home(legacy));
    std::ifstream second(xlings::home_identity::marker_path(legacy));
    const std::string after{std::istreambuf_iterator<char>(second), {}};
    EXPECT_EQ(before, after);

    // A SubOS is never adopted, whatever it contains.
    auto sub = legacy / "subos" / "s1";
    touch(sub / ".xlings.json", "{}");
    touch(sub / "bin" / kXlingsBin);
    fs::create_directories(sub / "subos");
    EXPECT_FALSE(xlings::home_identity::adopt_legacy_home(sub));
    EXPECT_FALSE(xlings::home_identity::has_marker(sub));
}

// ── resolve_owner_home ───────────────────────────────────────────────

TEST(ShimAnchorOwner, ShimInSubosBinAnchorsToHome) {
    TempDir tmp;
    auto home = tmp.path / "home";
    auto bin = make_home(home);
    auto shim = bin / "tool";
    touch(shim);

    auto owner = xlings::xvm::resolve_owner_home(shim);
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(fs::weakly_canonical(*owner), fs::weakly_canonical(home));
}

TEST(ShimAnchorOwner, NestedHomeAnchorsToInnermost) {
    TempDir tmp;
    auto outer = tmp.path / "outer";
    make_home(outer);
    auto inner = outer / "data" / "xpkgs" / "xim-x-embed" / "0.0.1" / "registry";
    auto innerBin = make_home(inner);
    auto shim = innerBin / "tool";
    touch(shim);

    auto owner = xlings::xvm::resolve_owner_home(shim);
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(fs::weakly_canonical(*owner), fs::weakly_canonical(inner));
}

TEST(ShimAnchorOwner, OrphanShimHasNoOwner) {
    TempDir tmp;
    auto shim = tmp.path / "loose" / "tool";
    touch(shim);
    // Walks up into the temp dir and beyond — no home signature anywhere
    // on the way (the walk stops at filesystem root).
    auto owner = xlings::xvm::resolve_owner_home(shim);
    EXPECT_FALSE(owner.has_value());
}

// ── home_knows_program ───────────────────────────────────────────────

TEST(ShimAnchorKnows, DirectKeyMatch) {
    TempDir tmp;
    auto home = tmp.path / "home";
    make_home(home, R"({
        "git": { "type": "program",
                 "versions": { "2.51.1": { "path": "data/xpkgs/g" } } }
    })");
    EXPECT_TRUE(xlings::xvm::home_knows_program(home, "git"));
    EXPECT_FALSE(xlings::xvm::home_knows_program(home, "python"));
}

TEST(ShimAnchorKnows, FilenameStemMatch) {
    TempDir tmp;
    auto home = tmp.path / "home";
    make_home(home, R"({
        "python3": { "type": "program", "filename": "python.exe",
                     "versions": { "3.12.0": { "path": "data/xpkgs/p" } } }
    })");
    EXPECT_TRUE(xlings::xvm::home_knows_program(home, "python"));
}

TEST(ShimAnchorKnows, EmptyVersionsIsNotAHit) {
    TempDir tmp;
    auto home = tmp.path / "home";
    make_home(home, R"({ "git": { "type": "program", "versions": {} } })");
    EXPECT_FALSE(xlings::xvm::home_knows_program(home, "git"));
}

TEST(ShimAnchorKnows, MissingConfigIsNotAHit) {
    TempDir tmp;
    EXPECT_FALSE(xlings::xvm::home_knows_program(tmp.path / "nope", "git"));
}

// ── resolve_dispatch_home (chain ordering) ───────────────────────────

namespace {

struct EnvGuard {
    std::string key;
    std::string old;
    EnvGuard(const std::string& k, const std::string& v) : key(k) {
        if (const char* o = std::getenv(k.c_str())) old = o;
        xlings::platform::set_env_variable(k, v);
    }
    ~EnvGuard() { xlings::platform::set_env_variable(key, old); }
};

} // namespace

TEST(ShimAnchorChain, OwnerWinsOverEnv) {
    TempDir tmp;
    auto ownerHome = tmp.path / "owner";
    auto ownerBin = make_home(ownerHome, R"({
        "tool": { "type": "program",
                  "versions": { "1.0.0": { "path": "data/xpkgs/t" } } }
    })");
    auto envHome = tmp.path / "env";
    make_home(envHome, R"({
        "tool": { "type": "program",
                  "versions": { "9.9.9": { "path": "data/xpkgs/t" } } }
    })");
    auto shim = ownerBin / "tool";
    touch(shim);

    EnvGuard g1("XLINGS_HOME", envHome.string());
    EnvGuard g2("XLINGS_SHIM_ANCHOR", "");
    auto chosen = xlings::xvm::resolve_dispatch_home(
        "tool", shim.string().c_str());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(fs::weakly_canonical(*chosen), fs::weakly_canonical(ownerHome));
}

TEST(ShimAnchorChain, EnvFallbackWhenOwnerDoesNotKnow) {
    TempDir tmp;
    auto ownerHome = tmp.path / "owner";
    auto ownerBin = make_home(ownerHome, "{}");
    auto envHome = tmp.path / "env";
    make_home(envHome, R"({
        "tool": { "type": "program",
                  "versions": { "1.0.0": { "path": "data/xpkgs/t" } } }
    })");
    auto shim = ownerBin / "tool";
    touch(shim);

    EnvGuard g1("XLINGS_HOME", envHome.string());
    EnvGuard g2("XLINGS_SHIM_ANCHOR", "");
    auto chosen = xlings::xvm::resolve_dispatch_home(
        "tool", shim.string().c_str());
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(fs::weakly_canonical(*chosen), fs::weakly_canonical(envHome));
}

TEST(ShimAnchorChain, OwnerBoundOnTotalMiss) {
    TempDir tmp;
    auto ownerHome = tmp.path / "owner";
    auto ownerBin = make_home(ownerHome, "{}");
    auto shim = ownerBin / "tool";
    touch(shim);

    EnvGuard g1("XLINGS_HOME", "");
    EnvGuard g2("XLINGS_SHIM_ANCHOR", "");
    auto chosen = xlings::xvm::resolve_dispatch_home(
        "tool", shim.string().c_str());
    // Nothing knows the tool → bind to the owner so the error names the
    // home this shim belongs to.
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(fs::weakly_canonical(*chosen), fs::weakly_canonical(ownerHome));
}

TEST(ShimAnchorChain, LegacyModeDisablesAnchoring) {
    TempDir tmp;
    auto ownerHome = tmp.path / "owner";
    auto ownerBin = make_home(ownerHome, R"({
        "tool": { "type": "program",
                  "versions": { "1.0.0": { "path": "data/xpkgs/t" } } }
    })");
    auto shim = ownerBin / "tool";
    touch(shim);

    EnvGuard g("XLINGS_SHIM_ANCHOR", "legacy");
    auto chosen = xlings::xvm::resolve_dispatch_home(
        "tool", shim.string().c_str());
    EXPECT_FALSE(chosen.has_value());
}

// #617: a shim in a SubOS that has a `subos/` of its own anchors to the home
// above it, not to the SubOS.
TEST(ShimAnchorOwner, ShimInASubosWithItsOwnSubosDirAnchorsToTheHome) {
    TempDir tmp;
    auto home = tmp.path / "home";
    make_home(home);
    auto sub = home / "subos" / "v615";
    touch(sub / ".xlings.json", "{ \"workspace\": {} }");
    touch(sub / "bin" / kXlingsBin);
    fs::create_directories(sub / "subos");
    auto shim = sub / "bin" / "ninja";
    touch(shim);

    auto owner = xlings::xvm::resolve_owner_home(shim);
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(fs::weakly_canonical(*owner), fs::weakly_canonical(home));
}

// #624: a home nested under another home's SubOS keeps its own paths. The
// outer home's `subos/<name>/` segment precedes the inner home, and the path
// is the inner home's: normalising it for the inner home's active SubOS must
// leave it alone, while the inner home's own SubOS paths are still re-rooted.
TEST(ShimAnchorOwner, NestedHomePathsAreNotReRootedByTheOuterSubos) {
    TempDir tmp;
    auto outer = tmp.path / ".xlings";
    make_home(outer);
    auto inner = outer / "subos" / "eco" / "work" / "mcpphome" / "registry";
    make_home(inner);
    const auto script = (inner / "data" / "xpkgs" / "xim-x-gcc-specs-config" / "0.0.1"
                         / "gcc-specs-config.lua").string();
    const auto innerActive = (inner / "subos" / "default").string();
    const auto alias = "xlings script " + script;
    EXPECT_EQ(xlings::xvm::normalize_subos_paths(alias, inner.string(), innerActive),
              alias);
    // The inner home's own SubOS path still follows the active SubOS.
    const auto baked = "--sysroot=" + (inner / "subos" / "old").string();
    EXPECT_EQ(xlings::xvm::normalize_subos_paths(baked, inner.string(), innerActive),
              "--sysroot=" + innerActive);
}

// Deployment S (design part 2 §2.1): the home's entry is a link to a system
// package's binary outside every home. A shim's owner is where the SHIM is,
// not where its link chain ends -- resolving the chain anchored every shim of
// such a home to nothing.
TEST(ShimAnchorOwner, AShimOfAHomeWhoseEntryIsASystemBinaryAnchorsToThatHome) {
    if constexpr (xlings::platform::is_windows) GTEST_SKIP() << "symlinked entries are POSIX";
    TempDir tmp;
    auto home = tmp.path / "home";
    auto bin = make_home(home);
    auto system = tmp.path / "usr" / "bin" / kXlingsBin;
    touch(system);
    fs::remove(home / "bin" / kXlingsBin);
    fs::create_symlink(system, home / "bin" / kXlingsBin);
    fs::create_symlink(fs::path("..") / ".." / ".." / "bin" / kXlingsBin, bin / "gcc");
    auto owner = xlings::xvm::resolve_owner_home(bin / "gcc");
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(fs::weakly_canonical(*owner), fs::weakly_canonical(home));

    // A link from outside every home to that shim still reaches the home.
    auto outside = tmp.path / "elsewhere" / "gcc";
    fs::create_directories(outside.parent_path());
    fs::create_symlink(bin / "gcc", outside);
    owner = xlings::xvm::resolve_owner_home(outside);
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(fs::weakly_canonical(*owner), fs::weakly_canonical(home));
}
