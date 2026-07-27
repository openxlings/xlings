// tests/unit/test_xim_install.cpp — the installer, xim commands, sub-index repos and local xpkg repos.
//
// Split out of the former single 12.7k-line test_main.cpp. Section order
// and contents are unchanged; only the file boundary is new.

#include <gtest/gtest.h>
#include <iomanip>
#ifdef __unix__
#include <sys/wait.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>  // geteuid — AtomicWriteTest skips permission cases as root
#endif

import std;
import xlings.core.i18n;
import xlings.core.log;
import xlings.core.utils;
import xlings.ui;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.xim.commands;
import xlings.core.xim.repo;
import xlings.core.xim.extract;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.xvm.inspect;
import xlings.core.xvm.lock;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.core.xvm.commands;
import xlings.core.compact;
import xlings.core.config;
import xlings.core.home_config;
import xlings.platform;
import xlings.libs.json;
import xlings.core.xself;
import xlings.core.profile;
import xlings.core.subos.gpu;
import xlings.core.xim.downloader;
import xlings.runtime;
import xlings.capabilities;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.cmdline;

namespace {

struct ScopedEnvVar {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    ScopedEnvVar(std::string_view key, std::string_view value) : name(key) {
        if (auto* prev = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = prev;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_prev) set(prev_value);
        else set("");
    }

    void set(std::string_view value) {
        xlings::platform::set_env_variable(name, std::string(value));
    }
};

std::optional<std::filesystem::path> find_pkgindex_repo() {
    namespace fs = std::filesystem;

    if (auto env = std::getenv("XIM_PKGINDEX_DIR")) {
        fs::path path(env);
        if (fs::exists(path / "pkgs")) return path;
    }

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures/xim-pkgindex",
        fs::current_path() / "../xim-pkgindex",
        fs::current_path() / "../d2learn/xim-pkgindex",
        fs::current_path() / "../../xim-pkgindex",
        fs::current_path() / "../../d2learn/xim-pkgindex",
    };

    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) return fs::weakly_canonical(path, ec);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> find_fixture_repo(std::string_view name) {
    namespace fs = std::filesystem;

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures" / name,
        fs::current_path() / "../../tests/fixtures" / name,
    };
    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) {
            return fs::weakly_canonical(path, ec);
        }
    }
    return std::nullopt;
}

}  // namespace

// ============================================================
// xim installer tests
// ============================================================

class XimInstallerTest : public ::testing::Test {
protected:
    std::filesystem::path repoDir_;
    xlings::xim::IndexManager mgr_;

    void SetUp() override {
        auto repo = find_pkgindex_repo();
        if (!repo) GTEST_SKIP() << "xim-pkgindex repo not found";
        repoDir_ = *repo;
        mgr_ = xlings::xim::IndexManager(repoDir_);
        auto r = mgr_.rebuild();
        if (!r) GTEST_SKIP() << "rebuild failed: " << r.error();
    }
};

TEST_F(XimInstallerTest, InstallerConstruction) {
    xlings::xim::Installer installer(mgr_);
    // Just verify it can be constructed without error
    SUCCEED();
}

TEST_F(XimInstallerTest, ExecuteEmptyPlan) {
    xlings::xim::Installer installer(mgr_);
    xlings::xim::InstallPlan plan;
    xlings::xim::DownloaderConfig config;

    auto result = installer.execute(plan, config, nullptr);
    EXPECT_TRUE(result.has_value());
}

TEST_F(XimInstallerTest, ExecutePlanWithErrors) {
    xlings::xim::Installer installer(mgr_);
    xlings::xim::InstallPlan plan;
    plan.errors.push_back("test error");
    xlings::xim::DownloaderConfig config;

    auto result = installer.execute(plan, config, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST_F(XimInstallerTest, UninstallNonexistent) {
    xlings::xim::Installer installer(mgr_);
    auto result = installer.uninstall("nonexistent_pkg_xyz_999");
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// xim commands tests
// ============================================================

TEST(XimCommandsTest, DetectPlatform) {
    auto platform = xlings::xim::detect_platform();
    #if defined(__linux__)
        EXPECT_EQ(platform, "linux");
    #elif defined(__APPLE__)
        EXPECT_EQ(platform, "macosx");
    #elif defined(_WIN32)
        EXPECT_EQ(platform, "windows");
    #endif
}

TEST(XimCommandsTest, SearchNonexistentReturnsZero) {
    // cmd_search uses get_catalog() which loads from Config (global ~/.xlings/data),
    // not from test fixtures. Skip if catalog cannot load.
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_search("zzz_nonexistent_pkg_xyz_999", stream);
    EXPECT_EQ(rc, 0);  // returns 0 with "no packages found" message
}

TEST(XimCommandsTest, ListWithFilter) {
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_list("gcc", stream);
    EXPECT_EQ(rc, 0);
}

TEST(XimCommandsTest, InfoKnownPackage) {
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    auto platform = xlings::xim::detect_platform();
    // gcc fixture only has linux entries; skip on other platforms
    if (platform != "linux") GTEST_SKIP() << "gcc fixture not available on " << platform;
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_info("xim:gcc", stream);
    EXPECT_EQ(rc, 0);
}

TEST(XimCommandsTest, InfoUnknownPackage) {
    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    xlings::EventStream stream;
    auto rc = xlings::xim::cmd_info("nonexistent_pkg_xyz_999", stream);
    EXPECT_EQ(rc, 1);
}

// ============================================================
// xim sub-index repos tests
// ============================================================

TEST(XimSubReposTest, DiscoverSubReposFromLuaFile) {
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_subrepo_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    // Write a mock xim-indexrepos.lua
    std::string lua = R"(xim_indexrepos = {
    ["awesome"] = {
        ["GLOBAL"] = "https://github.com/openxlings/xim-pkgindex-awesome.git",
        ["CN"] = "https://gitee.com/d2learn/xim-pkgindex-awesome.git",
    },
    ["scode"] = {
        ["GLOBAL"] = "https://github.com/openxlings/xim-pkgindex-scode.git",
    }
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "xim-indexrepos.lua").string(), lua);

    // Test GLOBAL mirror
    auto repos = xlings::xim::discover_sub_repos(testDir, "GLOBAL");
    ASSERT_EQ(repos.size(), 2u);
    // Order depends on iteration, so check by name
    bool foundAwesome = false, foundScode = false;
    for (auto& r : repos) {
        if (r.name == "awesome") {
            foundAwesome = true;
            EXPECT_EQ(r.url, "https://github.com/openxlings/xim-pkgindex-awesome.git");
        } else if (r.name == "scode") {
            foundScode = true;
            EXPECT_EQ(r.url, "https://github.com/openxlings/xim-pkgindex-scode.git");
        }
    }
    EXPECT_TRUE(foundAwesome);
    EXPECT_TRUE(foundScode);

    // Test CN mirror — awesome should use CN URL, scode falls back to GLOBAL
    auto reposCN = xlings::xim::discover_sub_repos(testDir, "CN");
    ASSERT_EQ(reposCN.size(), 2u);
    for (auto& r : reposCN) {
        if (r.name == "awesome") {
            EXPECT_EQ(r.url, "https://gitee.com/d2learn/xim-pkgindex-awesome.git");
        } else if (r.name == "scode") {
            EXPECT_EQ(r.url, "https://github.com/openxlings/xim-pkgindex-scode.git");
        }
    }

    fs::remove_all(testDir);
}

TEST(XimSubReposTest, DiscoverSubReposNoFile) {
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_subrepo_empty";
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    auto repos = xlings::xim::discover_sub_repos(testDir, "GLOBAL");
    EXPECT_TRUE(repos.empty());

    fs::remove_all(testDir);
}

TEST(XimSubReposTest, SyncRepoUrlKeepsGithubOnCNMirror) {
    auto url = xlings::xim::sync_repo_url(
        "https://github.com/openxlings/xim-pkgindex-awesome.git", "CN");
    EXPECT_EQ(url, "https://github.com/openxlings/xim-pkgindex-awesome.git");
}

// ── merge_sub_repos: lua defaults are authoritative for their own names ──
// (C4) The official index may move a default to a new org/URL; the lua URL must
// win over a stale json entry so the sub still classifies as default-official.
// json contributes ONLY names absent from lua (user-added sub-indexes).
TEST(XimSubReposTest, MergeSubReposLuaDefaultWinsOverStaleJson) {
    std::vector<xlings::IndexRepo> lua = {
        {"awesome", "https://github.com/openxlings/xim-pkgindex-awesome.git"},
        {"scode",   "https://github.com/openxlings/xim-pkgindex-scode.git"},
    };
    // json: stale org for awesome (old d2learn) + a user-added fork.
    std::vector<xlings::IndexRepo> json = {
        {"awesome",    "https://github.com/d2learn/xim-pkgindex-awesome.git"},
        {"fromsource", "https://github.com/d2learn/xim-pkgindex-fromsource.git"},
    };

    auto merged = xlings::xim::merge_sub_repos(lua, json);
    ASSERT_EQ(merged.size(), 3u);
    std::unordered_map<std::string, std::string> byName;
    for (auto& r : merged) byName[r.name] = r.url;
    // lua wins for the default that drifted org
    EXPECT_EQ(byName["awesome"], "https://github.com/openxlings/xim-pkgindex-awesome.git");
    EXPECT_EQ(byName["scode"],   "https://github.com/openxlings/xim-pkgindex-scode.git");
    // user-added json-only repo preserved
    EXPECT_EQ(byName["fromsource"], "https://github.com/d2learn/xim-pkgindex-fromsource.git");
}

TEST(XimSubReposTest, MergeSubReposJsonOnlyPreserved) {
    std::vector<xlings::IndexRepo> lua = {};
    std::vector<xlings::IndexRepo> json = {
        {"fromsource", "https://github.com/d2learn/xim-pkgindex-fromsource.git"},
    };
    auto merged = xlings::xim::merge_sub_repos(lua, json);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].name, "fromsource");
}

// ── sub_should_attempt_artifact: the C1 migration gate ──
TEST(XimSubReposTest, SubArtifactAutoFreshDefault) {
    // auto + default + fresh (no pkgs) → artifact
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        /*isDefaultOfficial=*/true, "auto",
        /*subManaged=*/false, /*subHasPkgs=*/false, /*mainArtifactManaged=*/false));
}

TEST(XimSubReposTest, SubArtifactAutoExistingGitNoMainMigration) {
    // auto + default + existing git checkout (has pkgs, not managed) +
    // main NOT artifact-managed → stay git (no migration)
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        true, "auto", /*subManaged=*/false, /*subHasPkgs=*/true,
        /*mainArtifactManaged=*/false));
}

TEST(XimSubReposTest, SubArtifactAutoMigratesWhenMainArtifactManaged) {
    // auto + default + existing git checkout + MAIN is artifact-managed
    // → migrate the sub to artifact too (the whole index is one unit). [C1]
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        true, "auto", /*subManaged=*/false, /*subHasPkgs=*/true,
        /*mainArtifactManaged=*/true));
}

TEST(XimSubReposTest, SubReposJsonObjectFormatRoundTrip) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "xlings-test-subrepos-json";
    fs::create_directories(dir);
    auto file = dir / "xim-indexrepos.json";

    std::vector<xlings::IndexRepo> repos;
    repos.push_back({"plain", "https://x/plain.git", "", ""});
    repos.push_back({"custom", "https://x/custom.git",
                     "https://github.com/o/custom-index", "auto"});
    xlings::xim::save_sub_repos_json(file, repos);

    auto loaded = xlings::xim::load_sub_repos_json(file);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].name, "custom");   // nlohmann object keys sort alphabetically
    EXPECT_EQ(loaded[0].url, "https://x/custom.git");
    EXPECT_EQ(loaded[0].artifactBase, "https://github.com/o/custom-index");
    EXPECT_EQ(loaded[0].source, "auto");
    EXPECT_EQ(loaded[1].name, "plain");
    EXPECT_TRUE(loaded[1].artifactBase.empty());

    // plain entries must persist as plain strings (old-xlings tolerant)
    auto text = xlings::platform::read_file_to_string(file.string());
    auto j = nlohmann::json::parse(text);
    EXPECT_TRUE(j["plain"].is_string());
    EXPECT_TRUE(j["custom"].is_object());
    fs::remove_all(dir);
}

// ── #377: custom repos with a declared artifact source ──
TEST(XimSubReposTest, SubArtifactCustomAutoAlwaysAttempts) {
    // custom + artifact declared: attempts even for an existing git checkout
    // with main not artifact-managed (no C1 gate — atomic swap migrates safely)
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        false, "auto", false, true, false, true));
}

TEST(XimSubReposTest, SubArtifactCustomForcedArtifact) {
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        false, "artifact", false, true, false, true));
}

TEST(XimSubReposTest, SubArtifactCustomGitForced) {
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        false, "git", false, true, false, true));
}

TEST(XimSubReposTest, SubArtifactNoSourceStaysGit) {
    // default param: prior behavior for repos without artifact declarations
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        false, "auto", false, false, true));
}

TEST(XimSubReposTest, SubArtifactAutoAlreadyManaged) {
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        true, "auto", /*subManaged=*/true, /*subHasPkgs=*/true, false));
}

TEST(XimSubReposTest, SubArtifactNonDefaultNeverArtifact) {
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        /*isDefaultOfficial=*/false, "auto", false, false, true));
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        /*isDefaultOfficial=*/false, "artifact", false, false, true));
}

TEST(XimSubReposTest, SubArtifactSourceOverrides) {
    // git mode disables artifact even for a fresh default
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        true, "git", false, false, true));
    // artifact mode forces it for a default regardless of fs state
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        true, "artifact", false, true, false));
}

// ============================================================
// xim add-xpkg / local repo tests
// ============================================================

TEST(XimAddXpkgTest, LocalRepoLetterSubdir) {
    // Verify that add-xpkg places files under pkgs/<letter>/ subdirectory
    // by testing the IndexManager can find packages in that structure
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_addxpkg_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir / "pkgs" / "t");

    // Create a minimal valid xpkg lua file (libxpkg table format)
    std::string lua = R"(package = {
    spec = "1",
    name = "test-pkg",
    description = "A test package",
    type = "package",
    status = "dev",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.com/test-1.0.0.tar.gz",
                sha256 = "abc123",
            },
        },
    },
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "pkgs" / "t" / "test-pkg.lua").string(), lua);

    // Build index — should find the package
    xlings::xim::IndexManager mgr(testDir);
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_GE(mgr.size(), 1u);

    // Verify entry exists
    auto* entry = mgr.find_entry("test-pkg");
    EXPECT_NE(entry, nullptr);

    fs::remove_all(testDir);
}

TEST(XimAddXpkgTest, FlatPkgsDirNotIndexed) {
    // Files placed flat in pkgs/ (not in letter subdir) should NOT be found
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_addxpkg_flat_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir / "pkgs");

    std::string lua = R"(package = {
    spec = "1",
    name = "flat-pkg",
    description = "A flat test package",
    type = "package",
    status = "dev",
    xpm = {
        linux = {
            ["1.0.0"] = {
                url = "https://example.com/flat-1.0.0.tar.gz",
                sha256 = "abc123",
            },
        },
    },
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "pkgs" / "flat-pkg.lua").string(), lua);

    xlings::xim::IndexManager mgr(testDir);
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(mgr.size(), 0u);  // flat file not picked up

    fs::remove_all(testDir);
}
