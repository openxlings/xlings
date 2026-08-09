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
import xlings.core.xim.payload;
import xlings.core.xim.installer;
import xlings.core.xim.commands;
import xlings.core.xim.inventory;
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
    const xlings::xvm::VersionDB legacyDb;
    EXPECT_TRUE(xlings::xim::detail::target_may_match_filter(
        legacyDb, "gcc", "16.1.0", std::string_view{"xim:gcc"}))
        << "canonical filter dropped a legacy bare workspace key";
    EXPECT_FALSE(xlings::xim::detail::target_may_match_filter(
        legacyDb, "gcc", "other:16.1.0", std::string_view{"xim:gcc"}))
        << "canonical filter admitted the same name from another namespace";

    xlings::xvm::VersionDB bindingDb;
    bindingDb["gcc"].versions["xim:16.1.0"].path =
        "/fixture/xpkgs/xim-x-gcc/16.1.0/bin/gcc";
    bindingDb["g++"].versions["xim:16.1.0"].path =
        "/fixture/xpkgs/xim-x-gcc/16.1.0/bin/g++";
    bindingDb["gcc"].bindings["g++"]["xim:16.1.0"] = "xim:16.1.0";
    bindingDb["g++"].bindings["gcc"]["xim:16.1.0"] = "xim:16.1.0";
    xlings::xim::detail::MetadataLookup fixtureMetadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"xim-x-gcc", {.namespaceName = "xim", .name = "gcc",
                            .canonicalName = "xim:gcc"}},
        }};
    const auto related = xlings::xim::detail::build_related_coordinates(
        bindingDb, "xim:gcc", "/fixture/xpkgs", fixtureMetadata);
    const auto member = related.find({"g++", "xim:16.1.0"});
    ASSERT_NE(member, related.end())
        << "filter dropped a target owned through a legacy reciprocal group";
    EXPECT_EQ(member->second.ns, "xim");
    EXPECT_EQ(member->second.package, "gcc");

    auto& catalog = xlings::xim::get_catalog();
    if (!catalog.is_loaded()) GTEST_SKIP() << "package catalog not available";
    xlings::xim::InventoryTrace trace;
    const auto rows = xlings::xim::collect_inventory(
        catalog, /*allSubos=*/false, std::string_view{"gcc"}, &trace);
    for (const auto& row : rows) {
        EXPECT_TRUE(row.canonicalName.contains("gcc")
                    || row.name.contains("gcc"));
    }
    for (const auto& identity : trace.metadataIdentities) {
        EXPECT_TRUE(identity.contains("gcc"))
            << "list filter loaded unrelated metadata: " << identity;
    }
    for (const auto& versionDir : trace.payloadVersionDirs) {
        EXPECT_TRUE(versionDir.parent_path().filename().string().contains("gcc"))
            << "list filter inspected an unrelated payload version: "
            << versionDir.string();
    }
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
    auto match = catalog.resolve_target("xim:gcc", platform);
    if (!match) GTEST_SKIP() << match.error();
    xlings::xim::InventoryTrace trace;
    const auto rows = xlings::xim::collect_package_inventory(
        catalog, match->canonicalName, /*allSubos=*/true, &trace);
    for (const auto& row : rows) {
        EXPECT_EQ(row.canonicalName, match->canonicalName);
    }
    ASSERT_FALSE(trace.metadataIdentities.empty());
    for (const auto& identity : trace.metadataIdentities) {
        EXPECT_TRUE(identity == match->canonicalName
                    || identity == match->name)
            << "info loaded unrelated metadata: " << identity;
    }
    const auto targetStore = xlings::xim::package_store_name(
        match->namespaceName, match->name);
    for (const auto& versionDir : trace.payloadVersionDirs) {
        EXPECT_EQ(versionDir.parent_path().filename(), targetStore)
            << "info inspected an unrelated payload version: "
            << versionDir.string();
    }
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

// ── payload platform identity (2026-07-30) ───────────────────────────
//
// "Already installed" used to mean "the directory exists and is not empty",
// which a payload built for another platform passes perfectly. The measured
// case was a Windows llvm@20.1.7 sitting in a Linux store: the install hook
// was skipped, the config hook ran anyway, and `libomp.dll` was registered as
// a program while `cc -> clang` warned six times and the command reported
// success.

namespace {

namespace pp = xlings::xim;
namespace fs = std::filesystem;

fs::path make_payload_dir(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("xlings-payload-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir / "bin");
    return dir;
}

void write_bytes(const fs::path& file, std::initializer_list<unsigned char> b) {
    std::ofstream out(file, std::ios::binary);
    for (auto c : b) out.put(static_cast<char>(c));
    // Pad so the file is a plausible executable, not a 4-byte curiosity.
    for (int i = 0; i < 64; ++i) out.put('\0');
}

// A magic number that is NOT this host's, whatever this host is.
void write_foreign_executable(const fs::path& file) {
    if (pp::host_platform_tag() == "windows") {
        write_bytes(file, {0x7F, 'E', 'L', 'F'});          // an ELF on Windows
    } else {
        write_bytes(file, {'M', 'Z', 0x90, 0x00});         // a PE anywhere else
    }
}

void write_host_executable(const fs::path& file) {
    if (pp::host_platform_tag() == "windows") {
        write_bytes(file, {'M', 'Z', 0x90, 0x00});
    } else if (pp::host_platform_tag() == "macosx") {
        write_bytes(file, {0xCF, 0xFA, 0xED, 0xFE});
    } else {
        write_bytes(file, {0x7F, 'E', 'L', 'F'});
    }
}

}  // namespace

TEST(PayloadPlatformTest, ForeignExecutablesAreDetected) {
    auto dir = make_payload_dir("foreign");
    write_foreign_executable(dir / "bin" / "clang.exe");
    EXPECT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Foreign);
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, HostExecutablesAreAccepted) {
    auto dir = make_payload_dir("host");
    write_host_executable(dir / "bin" / "clang");
    EXPECT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Host);
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, OneHostBinaryOutweighsForeignCompanions) {
    // Cross-compilers legitimately ship the other platform's artifacts. A
    // false Foreign costs a needless reinstall, so any host-format file
    // settles it.
    auto dir = make_payload_dir("mixed");
    write_foreign_executable(dir / "bin" / "target-tool.exe");
    write_host_executable(dir / "bin" / "driver");
    EXPECT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Host);
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, ScriptsAreInconclusive) {
    // A payload of shell scripts says nothing about its platform, and must
    // NOT be reinstalled on that basis.
    auto dir = make_payload_dir("scripts");
    xlings::platform::write_string_to_file(
        (dir / "bin" / "tool").string(), "#!/bin/sh\necho hi\n");
    EXPECT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Unknown);
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, MissingDirectoryIsInconclusive) {
    EXPECT_EQ(pp::classify_payload_platform(
                  fs::temp_directory_path() / "xlings-payload-absent"),
              pp::PayloadPlatform::Unknown);
}

TEST(PayloadPlatformTest, StampBeatsTheHeuristic) {
    // The stamp is what the payload's own install wrote. A host-format file
    // that arrived some other way must not overrule it.
    auto dir = make_payload_dir("stamped-foreign");
    write_host_executable(dir / "bin" / "tool");
    xlings::platform::write_string_to_file(
        (dir / ".xpkg-install.json").string(),
        "{\n  \"os\": \"plan9\",\n  \"version\": \"1.0.0\"\n}\n");
    EXPECT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Foreign);
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, WrittenStampRoundTrips) {
    auto dir = make_payload_dir("roundtrip");
    write_foreign_executable(dir / "bin" / "tool.exe");
    ASSERT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Foreign);
    // Self-heal: once this platform installs it, the heuristic is never
    // consulted again.
    pp::write_payload_stamp(dir, "1.0.0");
    EXPECT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Host);
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, StampIsNotWrittenIntoAnEmptyPayload) {
    // Wrapper packages (linux-headers, fromsource:* aliases) legitimately
    // leave install_dir empty; a stamp would make the emptiness probe read
    // "installed".
    auto dir = fs::temp_directory_path() / "xlings-payload-empty";
    fs::remove_all(dir);
    fs::create_directories(dir);
    pp::write_payload_stamp(dir, "1.0.0");
    EXPECT_TRUE(fs::is_empty(dir));
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, AStampAloneIsNotAPayload) {
    // The emptiness probe is what lets a broken payload be reinstalled. If a
    // stamp satisfied it on its own, the record of an install would become
    // evidence of one -- measured: a hook that began with
    // os.tryrm(install_dir), found no artifact to unpack, and left a
    // directory containing nothing but this platform's stamp.
    auto dir = fs::temp_directory_path() / "xlings-payload-stamp-only";
    fs::remove_all(dir);
    fs::create_directories(dir);
    xlings::platform::write_string_to_file(
        (dir / ".xpkg-install.json").string(), "{\n  \"os\": \"linux\"\n}\n");
    EXPECT_FALSE(pp::payload_has_content(dir));

    // The legacy wrapper marker means the opposite and must still count.
    xlings::platform::write_string_to_file((dir / ".xim-installed").string(), "");
    EXPECT_TRUE(pp::payload_has_content(dir));
    fs::remove_all(dir);
}

TEST(PayloadPlatformTest, ContentClassificationIgnoresTheStamp) {
    // "Should this payload be stamped" must never be answered by reading the
    // stamp, or a run that wrote one over a payload it did not install would
    // confirm its own claim forever.
    auto dir = make_payload_dir("content-vs-stamp");
    write_foreign_executable(dir / "bin" / "tool.exe");
    xlings::platform::write_string_to_file(
        (dir / ".xpkg-install.json").string(),
        std::string("{\n  \"os\": \"") + std::string(pp::host_platform_tag())
            + "\"\n}\n");
    EXPECT_EQ(pp::classify_payload_platform(dir), pp::PayloadPlatform::Host);
    EXPECT_EQ(pp::classify_payload_content(dir), pp::PayloadPlatform::Foreign);
    fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────────────
// Dependency version matching for deps_exports
//
// A dep's version half is a range. Matching it by string equality is the
// shape this guards: `xim:glibc@>=2.38` then matched no plan node, glibc's
// exports never reached deps_exports, elfpatch's predicate found no loader
// provider, and the package installed with none of its RPATHs written —
// reporting success the whole way. The first symptom was three layers away:
// glvnd's dlopen of mesa's EGL vendor failing to find libexpat, which EGL
// reports as having no vendor at all.
// ─────────────────────────────────────────────────────────────────────

TEST(DepVersionMatchTest, RangeMatchesTheResolvedVersion) {
    using xlings::xim::detail_::dep_version_matches_;
    EXPECT_TRUE(dep_version_matches_("2.39", ">=2.38"));
    EXPECT_TRUE(dep_version_matches_("15.1.0", ">=15"));
    EXPECT_TRUE(dep_version_matches_("1.7.0", "^1.2"));
    EXPECT_TRUE(dep_version_matches_("2.4.123", ">=2.4"));
}

TEST(DepVersionMatchTest, RangeRejectsAVersionBelowTheFloor) {
    using xlings::xim::detail_::dep_version_matches_;
    EXPECT_FALSE(dep_version_matches_("2.37", ">=2.38"));
    EXPECT_FALSE(dep_version_matches_("14.2.0", ">=15"));
}

TEST(DepVersionMatchTest, ExactPinStillMatchesItself) {
    using xlings::xim::detail_::dep_version_matches_;
    EXPECT_TRUE(dep_version_matches_("2.39", "2.39"));
    EXPECT_FALSE(dep_version_matches_("2.39", "2.38"));
}

TEST(DepVersionMatchTest, AnEmptyDepVersionMatchesAnything) {
    using xlings::xim::detail_::dep_version_matches_;
    EXPECT_TRUE(dep_version_matches_("2.39", ""));
    EXPECT_TRUE(dep_version_matches_("whatever", ""));
}

// A version that is not semver at all — a date, a git hash — has to keep
// matching itself. Equality is tried before the range parser for exactly
// this: satisfies_expr has no opinion about "2024.1" or "deadbeef", and
// leaning on it alone would have broken every recipe pinning one.
TEST(DepVersionMatchTest, NonSemverVersionsMatchByEquality) {
    using xlings::xim::detail_::dep_version_matches_;
    EXPECT_TRUE(dep_version_matches_("2024.1", "2024.1"));
    EXPECT_TRUE(dep_version_matches_("deadbeef", "deadbeef"));
    EXPECT_FALSE(dep_version_matches_("deadbeef", "cafebabe"));
}
