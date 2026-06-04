// Unit tests for owner-anchored shim dispatch (0.4.48).
// Design: .agents/docs/2026-06-04-shim-owner-anchoring-design.md
#include <gtest/gtest.h>

import std;
import xlings.core.xvm.shim;
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

// ── is_home_root ─────────────────────────────────────────────────────

TEST(ShimAnchorHomeRoot, RealHomeMatches) {
    TempDir tmp;
    auto home = tmp.path / "home";
    make_home(home);
    EXPECT_TRUE(xlings::xvm::is_home_root(home));
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
    EXPECT_FALSE(xlings::xvm::is_home_root(current));
}

TEST(ShimAnchorHomeRoot, ProjectStateDirDoesNotMatch) {
    TempDir tmp;
    // <project>/.xlings has subos/ but no bin/xlings — a project state
    // dir owns no payloads and must never anchor a shim.
    auto state = tmp.path / "project" / ".xlings";
    touch(state / ".xlings.json", "{}");
    fs::create_directories(state / "subos" / "_" / "bin");
    EXPECT_FALSE(xlings::xvm::is_home_root(state));
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
