// tests/unit/test_xim_catalog.cpp — xim types, config, catalog, index and resolver — turning a recipe tree
// into a resolvable plan.
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
import xlings.i18n;
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
import xlings.core.notice;
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

xlings::xim::IndexManager make_identity_index(
    std::vector<mcpplibs::xpkg::IndexEntry> entries) {
    mcpplibs::xpkg::PackageIndex index;
    for (auto& entry : entries) {
        index.entries.emplace(entry.entryKey, std::move(entry));
    }
    xlings::xim::IndexManager manager;
    manager.merge(std::move(index));
    return manager;
}

mcpplibs::xpkg::IndexEntry identity_entry(
    std::string namespaceName, std::string name,
    std::string version, std::filesystem::path path,
    std::string ref = {}) {
    const auto canonical = namespaceName + ":" + name;
    return {
        .identity = {
            .namespaceName = std::move(namespaceName),
            .name = std::move(name),
        },
        .canonicalName = canonical,
        .entryKey = version.empty() ? canonical : canonical + "@" + version,
        .name = canonical.substr(canonical.find(':') + 1),
        .version = std::move(version),
        .path = std::move(path),
        .ref = std::move(ref),
    };
}

}  // namespace

// ============================================================
// xim types tests
// ============================================================

TEST(XimTypesTest, InstallPlanEmpty) {
    xlings::xim::InstallPlan plan;
    EXPECT_FALSE(plan.has_errors());
    EXPECT_EQ(plan.pending_count(), 0u);
}

TEST(XimTypesTest, InstallPlanWithNodes) {
    xlings::xim::InstallPlan plan;
    {
        xlings::xim::PlanNode n; n.name = "gcc@15.1.0"; n.version = "15.1.0";
        plan.nodes.push_back(std::move(n));
    }
    {
        xlings::xim::PlanNode n; n.name = "glibc@2.39"; n.version = "2.39"; n.alreadyInstalled = true;
        plan.nodes.push_back(std::move(n));
    }
    {
        xlings::xim::PlanNode n; n.name = "binutils@2.42"; n.version = "2.42";
        plan.nodes.push_back(std::move(n));
    }
    EXPECT_EQ(plan.pending_count(), 2u);  // gcc + binutils (glibc already installed)
}

TEST(XimTypesTest, InstallPlanErrors) {
    xlings::xim::InstallPlan plan;
    plan.errors.push_back("cyclic dependency detected");
    EXPECT_TRUE(plan.has_errors());
}

TEST(XimTypesTest, DownloadTaskInit) {
    xlings::xim::DownloadTask task {
        .name = "gcc@15.1.0",
        .url = "https://example.com/gcc.tar.gz",
        .sha256 = "abcdef",
        .destDir = "/tmp/xim"
    };
    EXPECT_EQ(task.name, "gcc@15.1.0");
    EXPECT_EQ(task.sha256, "abcdef");
}

TEST(XimCatalogTest, CanonicalPackageNameAndStoreName) {
    EXPECT_EQ(xlings::xim::canonical_package_name("xim", "gcc"), "xim:gcc");
    EXPECT_EQ(xlings::xim::canonical_package_name("", "gcc"), "gcc");
    EXPECT_EQ(xlings::xim::package_store_name("xim", "gcc"), "xim-x-gcc");
    EXPECT_EQ(xlings::xim::package_store_name("", "gcc"), "gcc");
}

TEST(XimCatalogTest, FormatAmbiguousCandidates) {
    std::vector<xlings::xim::PackageMatch> matches = {
        {
            .name = "gcc",
            .version = "15.1.0",
            .namespaceName = "xim",
            .canonicalName = "xim:gcc",
            .repoName = "xim",
            .scope = xlings::xim::PackageScope::Global,
        },
        {
            .name = "gcc",
            .version = "15.1.0",
            .namespaceName = "project",
            .canonicalName = "project:gcc",
            .repoName = "project",
            .scope = xlings::xim::PackageScope::Project,
        },
    };

    auto msg = xlings::xim::format_ambiguous_candidates("gcc", matches);
    EXPECT_NE(msg.find("package 'gcc' is ambiguous"), std::string::npos);
    EXPECT_NE(msg.find("1. xim:gcc@15.1.0"), std::string::npos);
    EXPECT_NE(msg.find("2. project:gcc@15.1.0"), std::string::npos);
    EXPECT_NE(msg.find("from global repo 'xim'"), std::string::npos);
    EXPECT_NE(msg.find("from project repo 'project'"), std::string::npos);
    EXPECT_NE(msg.find("xlings install xim:gcc@15.1.0"), std::string::npos);
}

TEST(ConfigTest, WorkspaceInstallTargets) {
    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    ws["node"] = "";

    auto targets = xlings::Config::workspace_install_targets(ws);
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0], "gcc@15.1.0");
    EXPECT_EQ(targets[1], "node");
}

TEST(ConfigTest, MergedWorkspaceAnonymousOverridesGlobal) {
    xlings::xvm::Workspace globalWs;
    globalWs["gcc"] = "15.1.0";
    globalWs["node"] = "22.0.0";

    xlings::xvm::Workspace projectWs;
    projectWs["gcc"] = "14.2.0";
    projectWs["python"] = "3.12.0";

    auto effective = xlings::Config::merged_workspace(
        globalWs, projectWs, {}, xlings::ProjectSubosMode::Anonymous);

    ASSERT_EQ(effective.size(), 3u);
    EXPECT_EQ(effective["gcc"], "14.2.0");
    EXPECT_EQ(effective["node"], "22.0.0");
    EXPECT_EQ(effective["python"], "3.12.0");
}

TEST(ConfigTest, MergedWorkspaceNamedDoesNotInheritGlobal) {
    xlings::xvm::Workspace globalWs;
    globalWs["gcc"] = "15.1.0";
    globalWs["node"] = "22.0.0";

    xlings::xvm::Workspace projectWs;
    projectWs["gcc"] = "14.2.0";

    xlings::xvm::Workspace subosWs;
    subosWs["clang"] = "18.1.0";

    auto effective = xlings::Config::merged_workspace(
        globalWs, projectWs, subosWs, xlings::ProjectSubosMode::Named);

    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective["gcc"], "14.2.0");
    EXPECT_EQ(effective["clang"], "18.1.0");
    EXPECT_FALSE(effective.contains("node"));
}

TEST(ConfigTest, MergedVersionsProjectOverridesGlobal) {
    xlings::xvm::VersionDB globalDb;
    xlings::xvm::add_version(globalDb, "gcc", "15.1.0", "/global/gcc-15");
    xlings::xvm::add_version(globalDb, "node", "22.0.0", "/global/node-22");

    xlings::xvm::VersionDB projectDb;
    xlings::xvm::add_version(projectDb, "gcc", "14.2.0", "/project/gcc-14");
    xlings::xvm::add_version(projectDb, "python", "3.12.0", "/project/python-3.12");

    auto merged = xlings::Config::merged_versions(globalDb, projectDb);
    ASSERT_EQ(merged.size(), 3u);
    EXPECT_TRUE(xlings::xvm::has_version(merged, "gcc", "15.1.0"));
    EXPECT_TRUE(xlings::xvm::has_version(merged, "gcc", "14.2.0"));
    EXPECT_TRUE(xlings::xvm::has_version(merged, "node", "22.0.0"));
    EXPECT_TRUE(xlings::xvm::has_version(merged, "python", "3.12.0"));
}

TEST(ConfigTest, ResolveRepoSourceAbsolutePath) {
#ifdef _WIN32
    xlings::IndexRepo repo { .name = "local", .url = "C:\\tmp\\xim-pkgindex" };
    auto expected = std::filesystem::path("C:\\tmp\\xim-pkgindex");
#else
    xlings::IndexRepo repo { .name = "local", .url = "/tmp/xim-pkgindex" };
    auto expected = std::filesystem::path("/tmp/xim-pkgindex");
#endif
    auto path = xlings::Config::resolve_repo_source(repo, false);
    EXPECT_EQ(path, expected);
    EXPECT_TRUE(xlings::Config::is_local_repo_source(repo, false));
}

TEST(ConfigTest, ResolveRepoSourceFileScheme) {
#ifdef _WIN32
    // Standard file URI: file:///C:/path — production code strips the leading /
    xlings::IndexRepo repo { .name = "local", .url = "file:///C:/tmp/xim-pkgindex" };
    auto expected = std::filesystem::path("C:\\tmp\\xim-pkgindex");
#else
    xlings::IndexRepo repo { .name = "local", .url = "file:///tmp/xim-pkgindex" };
    auto expected = std::filesystem::path("/tmp/xim-pkgindex");
#endif
    auto path = xlings::Config::resolve_repo_source(repo, false);
    EXPECT_EQ(path, expected);
    EXPECT_TRUE(xlings::Config::is_local_repo_source(repo, false));
}

TEST(ConfigTest, ResolveRepoSourceRemoteUrlReturnsEmpty) {
    xlings::IndexRepo repo {
        .name = "xim",
        .url = "https://github.com/openxlings/xim-pkgindex.git"
    };
    EXPECT_TRUE(xlings::Config::resolve_repo_source(repo, false).empty());
    EXPECT_FALSE(xlings::Config::is_local_repo_source(repo, false));
}

// ── #377: index_repos parsing with artifact/source fields ──
TEST(ConfigIndexReposTest, PlainEntryHasNoArtifact) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"a","url":"https://x/a.git"}]})");
    auto repos = xlings::parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].name, "a");
    EXPECT_EQ(repos[0].url, "https://x/a.git");
    EXPECT_TRUE(repos[0].artifactBases.empty());
    EXPECT_TRUE(repos[0].source.empty());
}

TEST(ConfigIndexReposTest, ArtifactStringTrimsTrailingSlash) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"m","url":"https://x/m.git",
         "artifact":"https://github.com/xlings-res/mcpp-index/","source":"auto"}]})");
    auto repos = xlings::parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    ASSERT_EQ(repos[0].artifactBases.size(), 1u);
    EXPECT_EQ(repos[0].artifact_base(), "https://github.com/xlings-res/mcpp-index");
    EXPECT_TRUE(repos[0].artifactBases[0].region.empty());   // flat: no region
    EXPECT_EQ(repos[0].source, "auto");
}

// #598: the mirror decides the ORDER of the declared bases, never the set.
// This test used to assert the opposite -- that a region object resolves to
// one base -- which is exactly the behaviour that sent a 403 from one region
// to git instead of to the other region.
TEST(ConfigIndexReposTest, ArtifactRegionObjectOrdersByMirrorKeepingAll) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"m","url":"https://x/m.git",
         "artifact":{"GLOBAL":"https://github.com/o/r","CN":"https://gitcode.com/o/r"}}]})");

    auto cn = xlings::parse_index_repos_json(j, "CN")[0].artifactBases;
    ASSERT_EQ(cn.size(), 2u);
    EXPECT_EQ(cn[0].url, "https://gitcode.com/o/r");
    EXPECT_EQ(cn[0].region, "CN");
    EXPECT_EQ(cn[1].url, "https://github.com/o/r");
    EXPECT_EQ(cn[1].region, "GLOBAL");

    auto global = xlings::parse_index_repos_json(j, "")[0].artifactBases;
    ASSERT_EQ(global.size(), 2u);
    EXPECT_EQ(global[0].url, "https://github.com/o/r");
    EXPECT_EQ(global[1].url, "https://gitcode.com/o/r");

    // Unknown mirror: GLOBAL leads, and the rest still follow.
    auto unknown = xlings::parse_index_repos_json(j, "XX")[0].artifactBases;
    ASSERT_EQ(unknown.size(), 2u);
    EXPECT_EQ(unknown[0].url, "https://github.com/o/r");
    EXPECT_EQ(unknown[1].url, "https://gitcode.com/o/r");
}

TEST(ConfigRegionChainTest, FlatStringIsOneRegionlessEntry) {
    auto chain = xlings::parse_region_chain(nlohmann::json("https://example.com/idx/"), "CN");
    ASSERT_EQ(chain.size(), 1u);
    EXPECT_EQ(chain[0].url, "https://example.com/idx");   // trailing slash trimmed
    EXPECT_TRUE(chain[0].region.empty());
}

TEST(ConfigRegionChainTest, DuplicateUrlsAppearOnce) {
    auto j = nlohmann::json::parse(
        R"({"GLOBAL":"https://github.com/o/r","CN":"https://github.com/o/r"})");
    auto chain = xlings::parse_region_chain(j, "CN");
    ASSERT_EQ(chain.size(), 1u);
    EXPECT_EQ(chain[0].region, "CN");   // the preferred spelling of the one location
}

TEST(ConfigRegionChainTest, ExtraRegionsFollowGlobalInDeclarationOrder) {
    auto j = nlohmann::json::parse(
        R"({"CN":"https://cn/r","GLOBAL":"https://g/r","EU":"https://eu/r"})");
    auto chain = xlings::parse_region_chain(j, "CN");
    ASSERT_EQ(chain.size(), 3u);
    EXPECT_EQ(chain[0].url, "https://cn/r");
    EXPECT_EQ(chain[1].url, "https://g/r");      // GLOBAL is the canonical fallback
    EXPECT_EQ(chain[2].url, "https://eu/r");
}

TEST(ConfigRegionChainTest, NonStringAndEmptyValuesDropped) {
    auto j = nlohmann::json::parse(R"({"GLOBAL":"","CN":"https://cn/r","X":42})");
    auto chain = xlings::parse_region_chain(j, "CN");
    ASSERT_EQ(chain.size(), 1u);
    EXPECT_EQ(chain[0].url, "https://cn/r");
}

TEST(ConfigIndexReposTest, MalformedEntriesSkipped) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"a"},{"url":"u"},{"name":"b","url":"https://x/b.git"}]})");
    auto repos = xlings::parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].name, "b");
}

// ============================================================
// xim index tests (requires xim-pkgindex repo)
// ============================================================

class XimIndexTest : public ::testing::Test {
protected:
    std::filesystem::path repoDir_;

    void SetUp() override {
        auto repo = find_pkgindex_repo();
        if (!repo) GTEST_SKIP() << "xim-pkgindex repo not found";
        repoDir_ = *repo;
    }
};

TEST_F(XimIndexTest, BuildIndex) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(mgr.is_loaded());
    EXPECT_GT(mgr.size(), 40u);  // should have 50+ entries
}

TEST_F(XimIndexTest, SearchPackage) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto results = mgr.search("gcc");
    EXPECT_FALSE(results.empty());
    // At least one result should contain "gcc"
    bool found = false;
    for (auto& name : results) {
        if (name.find("gcc") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "search for 'gcc' should return gcc-related packages";
}

TEST_F(XimIndexTest, MatchVersion) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto match = mgr.match_version("gcc");
    EXPECT_TRUE(match.has_value()) << "should find a versioned gcc entry";
    if (match) {
        EXPECT_NE(match->find("gcc"), std::string::npos);
    }
}

TEST_F(XimIndexTest, FindEntry) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    // Find a known package
    auto match = mgr.match_version("gcc");
    ASSERT_TRUE(match.has_value());
    auto* entry = mgr.find_entry(*match);
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->path.empty());
}

TEST_F(XimIndexTest, LoadPackage) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto match = mgr.match_version("gcc");
    ASSERT_TRUE(match.has_value());

    auto pkg = mgr.load_package(*match);
    ASSERT_TRUE(pkg.has_value()) << pkg.error();
    EXPECT_EQ(pkg->name, "gcc");
}

TEST_F(XimIndexTest, AllNames) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto names = mgr.all_names();
    EXPECT_GT(names.size(), 40u);
    // Should be sorted
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
}

TEST_F(XimIndexTest, MarkInstalled) {
    xlings::xim::IndexManager mgr(repoDir_);
    auto r = mgr.rebuild();
    ASSERT_TRUE(r.has_value()) << r.error();

    auto match = mgr.match_version("gcc");
    ASSERT_TRUE(match.has_value());

    mgr.mark_installed(*match, true);
    auto* entry = mgr.find_entry(*match);
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->installed);

    mgr.mark_installed(*match, false);
    entry = mgr.find_entry(*match);
    EXPECT_FALSE(entry->installed);
}

TEST_F(XimIndexTest, EmptyRepoDirFails) {
    xlings::xim::IndexManager mgr;
    auto result = mgr.rebuild();
    EXPECT_FALSE(result.has_value());
}

TEST_F(XimIndexTest, NonexistentRepoDirFails) {
    xlings::xim::IndexManager mgr("/tmp/nonexistent_xim_repo_dir_xyz");
    auto result = mgr.rebuild();
    EXPECT_FALSE(result.has_value());
}

TEST(XimNamespaceIndexTest, PreservesSameNameCandidatesAndCanonicalOperations) {
    auto fixture = find_fixture_repo("index-same-name");
    ASSERT_TRUE(fixture.has_value());

    xlings::xim::IndexManager mgr(*fixture, "fixture-default");
    auto result = mgr.rebuild();
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(
        mgr.find_candidates("demo"),
        (std::vector<std::string> { "alpha:demo", "beta:demo" }));
    EXPECT_EQ(
        mgr.find_candidates("demo", std::string_view { "alpha" }),
        (std::vector<std::string> { "alpha:demo" }));

    auto* alphaEntry = mgr.find_entry("alpha:demo");
    auto* betaEntry = mgr.find_entry("beta:demo");
    ASSERT_NE(alphaEntry, nullptr);
    ASSERT_NE(betaEntry, nullptr);
    EXPECT_EQ(alphaEntry->identity.namespaceName, "alpha");
    EXPECT_EQ(betaEntry->identity.namespaceName, "beta");

    auto alphaPackage = mgr.load_package("alpha:demo");
    auto betaPackage = mgr.load_package("beta:demo");
    ASSERT_TRUE(alphaPackage.has_value()) << alphaPackage.error();
    ASSERT_TRUE(betaPackage.has_value()) << betaPackage.error();
    EXPECT_EQ(alphaPackage->description, "Alpha namespace demo package");
    EXPECT_EQ(betaPackage->description, "Beta namespace demo package");

    mgr.mark_installed("alpha:demo", true);
    EXPECT_TRUE(mgr.find_entry("alpha:demo")->installed);
    EXPECT_FALSE(mgr.find_entry("beta:demo")->installed);
    EXPECT_EQ(mgr.entry_path("alpha:demo"), alphaEntry->path);
}

TEST(XimNamespaceIndexTest, RejectsDuplicateEffectiveIdentityWithBothPaths) {
    auto fixture = find_fixture_repo("index-duplicate");
    ASSERT_TRUE(fixture.has_value());

    xlings::xim::IndexManager mgr(*fixture, "xim");
    auto result = mgr.rebuild();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("duplicate package identity 'xim:demo'"),
              std::string::npos);
    EXPECT_NE(result.error().find("implicit.demo.lua"), std::string::npos);
    EXPECT_NE(result.error().find("explicit.demo.lua"), std::string::npos);
}

TEST(XimCatalogLocalIdentityTest,
     VersionedOnlyAndAliasUseIndexResolutionWithoutRecipes) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-local-identity-{}",
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());
    fs::create_directories(root);
    const auto marker = root / "recipe-ran";
    const auto recipe = root / "pkg.lua";
    const auto unrelated = root / "unrelated.lua";
    xlings::platform::write_string_to_file(recipe.string(),
        std::format("local f=io.open('{}','w'); f:write('pkg'); f:close()",
                    marker.string()));
    xlings::platform::write_string_to_file(unrelated.string(),
        std::format("local f=io.open('{}','w'); f:write('other'); f:close()",
                    marker.string()));
    auto index = make_identity_index({
        identity_entry("ns", "pkg", "1.0.0", recipe),
        identity_entry("ns", "shortcut", {}, unrelated, "pkg"),
        identity_entry("other", "unrelated", "9.0.0", unrelated),
    });
    const xlings::xim::RepoIndexSpec spec{
        .name = "global-a",
        .scope = xlings::xim::PackageScope::Global,
    };
    const std::array views{
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = spec.name,
            .scope = spec.scope,
            .subIndex = spec.subIndex,
            .index = &index,
            .storeRoot = root / "store",
        },
    };

    const auto direct =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            views, "ns:pkg");
    const auto alias =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            views, "ns:shortcut");

    ASSERT_TRUE(direct.has_value()) << direct.error();
    EXPECT_EQ(direct->rawName, "ns:pkg@1.0.0");
    EXPECT_EQ(direct->version, "1.0.0");
    EXPECT_EQ(direct->pkgFile, recipe);
    ASSERT_TRUE(alias.has_value()) << alias.error();
    EXPECT_EQ(alias->canonicalName, "ns:pkg");
    EXPECT_EQ(alias->rawName, "ns:pkg@1.0.0");
    EXPECT_EQ(alias->version, "1.0.0");
    EXPECT_FALSE(fs::exists(marker));
    fs::remove_all(root);
}

TEST(XimCatalogLocalIdentityTest, DuplicateGlobalRepositoriesStayAmbiguous) {
    auto first = make_identity_index({
        identity_entry("ns", "pkg", "1.0.0", "/repo-a/pkg.lua"),
    });
    auto second = make_identity_index({
        identity_entry("ns", "pkg", "1.0.0", "/repo-b/pkg.lua"),
    });
    const xlings::xim::RepoIndexSpec specA{
        .name = "global-a",
        .scope = xlings::xim::PackageScope::Global,
    };
    const xlings::xim::RepoIndexSpec specB{
        .name = "global-b",
        .scope = xlings::xim::PackageScope::Global,
    };
    const std::array views{
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = specA.name, .scope = specA.scope,
            .subIndex = specA.subIndex, .index = &first,
            .storeRoot = "/store-a"},
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = specB.name, .scope = specB.scope,
            .subIndex = specB.subIndex, .index = &second,
            .storeRoot = "/store-b"},
    };

    const auto result =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            views, "ns:pkg");

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("ambiguous"), std::string::npos);
    EXPECT_NE(result.error().find("global-a"), std::string::npos);
    EXPECT_NE(result.error().find("global-b"), std::string::npos);
}

TEST(XimCatalogLocalIdentityTest,
     CrossRepoScopeNeverShadowsWithoutXpmVersionProof) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-local-scope-proof-{}",
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());
    fs::create_directories(root);
    const auto marker = root / "recipe-ran";
    const auto recipe = root / "pkg.lua";
    xlings::platform::write_string_to_file(recipe.string(),
        std::format("local f=io.open('{}','w'); f:write('ran'); f:close()",
                    marker.string()));

    auto globalUnversioned = make_identity_index({
        identity_entry("ns", "pkg", {}, recipe),
    });
    auto projectUnversioned = make_identity_index({
        identity_entry("ns", "pkg", {}, recipe),
    });
    auto globalVersioned = make_identity_index({
        identity_entry("ns", "pkg", "1.0.0", recipe),
    });
    auto projectVersioned = make_identity_index({
        identity_entry("ns", "pkg", "1.0.0", recipe),
    });
    auto globalSecond = make_identity_index({
        identity_entry("ns", "pkg", "1.0.0", recipe),
    });
    auto projectSecond = make_identity_index({
        identity_entry("ns", "pkg", "1.0.0", recipe),
    });
    const xlings::xim::RepoIndexSpec globalSpec{
        .name = "global",
        .scope = xlings::xim::PackageScope::Global,
    };
    const xlings::xim::RepoIndexSpec projectSpecA{
        .name = "project-a",
        .scope = xlings::xim::PackageScope::Project,
    };
    const xlings::xim::RepoIndexSpec projectSpecB{
        .name = "project-b",
        .scope = xlings::xim::PackageScope::Project,
    };
    const std::array unversionedViews{
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = projectSpecA.name, .scope = projectSpecA.scope,
            .subIndex = projectSpecA.subIndex,
            .index = &projectUnversioned,
            .storeRoot = "/project-store-a"},
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = globalSpec.name, .scope = globalSpec.scope,
            .subIndex = globalSpec.subIndex,
            .index = &globalUnversioned,
            .storeRoot = "/global-store"},
    };
    const auto unversioned =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            unversionedViews, "ns:pkg");
    EXPECT_FALSE(unversioned.has_value());

    const std::array versionedViews{
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = projectSpecA.name, .scope = projectSpecA.scope,
            .subIndex = projectSpecA.subIndex, .index = &projectVersioned,
            .storeRoot = "/project-store-a"},
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = globalSpec.name, .scope = globalSpec.scope,
            .subIndex = globalSpec.subIndex, .index = &globalVersioned,
            .storeRoot = "/global-store"},
    };
    const auto versioned =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            versionedViews, "ns:pkg");
    EXPECT_FALSE(versioned.has_value());

    const std::array fourRepoViews{
        versionedViews[0],
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = projectSpecB.name, .scope = projectSpecB.scope,
            .subIndex = projectSpecB.subIndex, .index = &projectSecond,
            .storeRoot = "/project-store-b"},
        versionedViews[1],
        xlings::xim::catalog_detail::LocalIdentityRepoView{
            .repoName = "global-b",
            .scope = xlings::xim::PackageScope::Global,
            .index = &globalSecond,
            .storeRoot = "/global-store-b"},
    };
    const auto fourRepos =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            fourRepoViews, "ns:pkg");
    EXPECT_FALSE(fourRepos.has_value());

    const std::array projectViews{fourRepoViews[0], fourRepoViews[1]};
    const std::array globalViews{fourRepoViews[2], fourRepoViews[3]};
    EXPECT_FALSE(
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            projectViews, "ns:pkg").has_value());
    EXPECT_FALSE(
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            globalViews, "ns:pkg").has_value());

    const auto projectOnly =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            std::span{versionedViews}.first(1), "ns:pkg");
    const auto globalOnly =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            std::span{versionedViews}.last(1), "ns:pkg");
    ASSERT_TRUE(projectOnly.has_value()) << projectOnly.error();
    ASSERT_TRUE(globalOnly.has_value()) << globalOnly.error();
    EXPECT_EQ(projectOnly->repoName, "project-a");
    EXPECT_EQ(globalOnly->repoName, "global");
    EXPECT_FALSE(fs::exists(marker));
    fs::remove_all(root);
}

// ============================================================
// xim resolver tests
// ============================================================

// The IndexManager overload of resolve() is gone: it was a second
// version-resolution path that never reached production (only these tests
// called it), and it resolved `@<hint>` by using the hint verbatim as the
// version -- so `xim:python@3` meant a literal version "3" there and a
// semver range everywhere else. Two answers to one question is what these
// tests now guard against, from the other end: pin_target_to_subos is the
// single place a target meets an already-active version.

using xlings::xim::pin_target_to_subos;

// A workspace stub: whatever the test says is active.
static auto active_map(std::map<std::string, std::string> m) {
    return [m = std::move(m)](const std::string& name) -> std::string {
        auto it = m.find(name);
        return it == m.end() ? std::string{} : it->second;
    };
}

TEST(XimPinToActive, NoCallbackLeavesTargetAlone) {
    EXPECT_EQ(pin_target_to_subos("xim:mcpp", {}), "xim:mcpp");
}

TEST(XimPinToActive, NothingActiveLeavesTargetAlone) {
    auto active = active_map({});
    EXPECT_EQ(pin_target_to_subos("xim:mcpp", active), "xim:mcpp");
}

TEST(XimPinToActive, UnpinnedDepTakesTheActiveVersion) {
    // The whole point: `deps = {"xim:mcpp"}` must not drag in the newest
    // mcpp when a perfectly good one is already active.
    auto active = active_map({{"mcpp", "2026.7.30.2"}});
    EXPECT_EQ(pin_target_to_subos("xim:mcpp", active),
              "xim:mcpp@2026.7.30.2");
}

TEST(XimPinToActive, StripsNamespaceWhenLookingUpActive) {
    // The versions DB is keyed by bare program name; the dep is namespaced.
    auto active = active_map({{"python", "3.11.4"}});
    EXPECT_EQ(pin_target_to_subos("xim:python@3", active),
              "xim:python@3.11.4");
    EXPECT_EQ(pin_target_to_subos("python@3", active), "python@3.11.4");
}

TEST(XimPinToActive, RangeConstraintUsesSemverNotPrefix) {
    auto active = active_map({{"lib", "1.10.0"}});
    // `@1.1` must not be satisfied by 1.10.0 -- the bug a starts_with test has.
    EXPECT_EQ(pin_target_to_subos("lib@1.1", active), "lib@1.1");
    EXPECT_EQ(pin_target_to_subos("lib@^1.2", active), "lib@1.10.0");
    EXPECT_EQ(pin_target_to_subos("lib@1", active), "lib@1.10.0");
}

TEST(XimPinToActive, ExactPinIsNeverOverridden) {
    // An exact `@2.39` means 2.39. An active 2.38 does not satisfy it, so the
    // target survives untouched and the install goes ahead.
    auto active = active_map({{"glibc", "2.38"}});
    EXPECT_EQ(pin_target_to_subos("xim:glibc@2.39", active),
              "xim:glibc@2.39");
    auto satisfied = active_map({{"glibc", "2.39"}});
    EXPECT_EQ(pin_target_to_subos("xim:glibc@2.39", satisfied),
              "xim:glibc@2.39");
}

// ── namespace priority: `local` ranks last ────────────────────────────────
//
// Refusing an ambiguous bare name is right where there is NO rule. `local` is
// not a peer of the maintained namespaces -- it is the side-loading one, where
// a dev script's recipe stays forever -- and the refusal had a cost measured
// on a real machine: `xlings self update` refused because `local:xlings` and
// `xim:xlings` tied, so NOTHING upgraded the entry binary, and the home spent
// weeks dispatching every shim through a June client.
//
// The rule is deliberately the smallest one that fixes that: demote `local`,
// invent no order among the others, leave explicitly-qualified targets alone,
// and NAME the loser.

namespace {
std::array<xlings::xim::catalog_detail::LocalIdentityRepoView, 1>
one_repo_view(const xlings::xim::IndexManager& index) {
    return { xlings::xim::catalog_detail::LocalIdentityRepoView{
        .repoName = "global",
        .scope = xlings::xim::PackageScope::Global,
        .subIndex = false,
        .index = &index,
        .storeRoot = "/store",
    } };
}
}  // namespace

TEST(XimNamespacePriorityTest, LocalLosesABareNameAndIsNamed) {
    auto index = make_identity_index({
        identity_entry("xim", "xlings", "2026.8.11.1", "/repo/xim/xlings.lua"),
        identity_entry("local", "xlings", "0.4.51", "/repo/local/xlings.lua"),
    });
    const auto views = one_repo_view(index);
    const auto got =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            views, "xlings");
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_EQ(got->canonicalName, "xim:xlings");
    // The demotion is DATA, not just a log line: a pick nobody can see is the
    // thing this rule must not become.
    ASSERT_EQ(got->demoted.size(), 1u);
    EXPECT_EQ(got->demoted.front().coordinate, "local:xlings@0.4.51");
    EXPECT_EQ(got->demoted.front().version, "0.4.51");
}

TEST(XimNamespacePriorityTest, AnExplicitlyQualifiedLocalTargetIsUntouched) {
    auto index = make_identity_index({
        identity_entry("xim", "xlings", "2026.8.11.1", "/repo/xim/xlings.lua"),
        identity_entry("local", "xlings", "0.4.51", "/repo/local/xlings.lua"),
    });
    const auto views = one_repo_view(index);
    const auto got =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            views, "local:xlings");
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_EQ(got->canonicalName, "local:xlings");
    EXPECT_TRUE(got->demoted.empty())
        << "the user said which one; nothing was overruled";
}

TEST(XimNamespacePriorityTest, TwoNonLocalNamespacesStillRefuse) {
    // There IS no rule between `xim:` and `d2x:`. Inventing one would be
    // exactly the silent pick the deterministic-or-refuse contract forbids.
    auto index = make_identity_index({
        identity_entry("xim", "tool", "1.0.0", "/repo/xim/tool.lua"),
        identity_entry("d2x", "tool", "2.0.0", "/repo/d2x/tool.lua"),
    });
    const auto views = one_repo_view(index);
    const auto got =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            views, "tool");
    ASSERT_FALSE(got.has_value());
    EXPECT_NE(got.error().find("ambiguous"), std::string::npos);
}

TEST(XimNamespacePriorityTest, LocalAloneStillResolves) {
    // Demotion is a tiebreak, not an exclusion: a package only `local` has is
    // still that package.
    auto index = make_identity_index({
        identity_entry("local", "mytool", "0.1.0", "/repo/local/mytool.lua"),
    });
    const auto views = one_repo_view(index);
    const auto got =
        xlings::xim::catalog_detail::resolve_local_identity_from_repos(
            views, "mytool");
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_EQ(got->canonicalName, "local:mytool");
    EXPECT_TRUE(got->demoted.empty());
}

// ── demotion verdict: one rule, shared by the gate and the notice ─────────
//
// `detail_::demotion_verdict` is the single place that decides "is every
// demoted candidate a duplicate of the version already chosen" and builds
// the fingerprint/`losers` string from it -- both `announce_demotion_`'s
// in-process gate and `demotion_notice`'s persisted one call it rather than
// each recomputing the same three lines (review finding: they used to).

TEST(XimDemotionVerdict, SameVersionIsNullopt) {
    xlings::xim::PackageMatch chosen;
    chosen.canonicalName = "xim:xlings";
    chosen.version = "0.4.51";
    chosen.demoted = { { "local:xlings@0.4.51", "0.4.51" } };

    EXPECT_FALSE(xlings::xim::detail_::demotion_verdict("xlings", chosen)
                     .has_value())
        << "every demoted candidate is the version already chosen: nothing "
           "to verdict";
}

TEST(XimDemotionVerdict, NoDemotedCandidatesIsNullopt) {
    xlings::xim::PackageMatch chosen;
    chosen.canonicalName = "xim:xlings";
    chosen.version = "2026.8.11.1";

    EXPECT_FALSE(xlings::xim::detail_::demotion_verdict("xlings", chosen)
                     .has_value());
}

TEST(XimDemotionVerdict, MixedVersionsProducesFingerprintAndLosers) {
    xlings::xim::PackageMatch chosen;
    chosen.canonicalName = "xim:xlings";
    chosen.version = "2026.8.11.1";
    chosen.demoted = { { "local:xlings@0.4.51", "0.4.51" } };

    auto verdict = xlings::xim::detail_::demotion_verdict("xlings", chosen);
    ASSERT_TRUE(verdict.has_value());
    EXPECT_EQ(verdict->losers, "local:xlings@0.4.51");
    EXPECT_EQ(verdict->fingerprint, "xlings\x1f" "xim:xlings\x1f" "local:xlings@0.4.51");
}

// ── demotion notice: a pure duplicate is not a conflict ───────────────────
//
// `local:xlings@0.4.51` sitting next to `xim:xlings@0.4.51` is the ordinary
// shape of a dev machine, not a namespace conflict: the version is
// identical, so there is nothing to pick between. `detail_::demotion_notice`
// is the free function behind `announce_demotion_`'s decision, with an
// injected `notice::Memo` so it is testable without a home -- see
// tests/unit/test_notice.cpp for the same shape of fake.

namespace {

struct CountingMemo {
    int markCalls = 0;
    std::set<std::string, std::less<>> seenKeys;

    xlings::notice::Memo memo() {
        return xlings::notice::Memo{
            .seen = [this](std::string_view id) { return seenKeys.contains(id); },
            .mark = [this](std::string_view id) {
                ++markCalls;
                seenKeys.emplace(id);
            },
        };
    }
};

}  // namespace

TEST(XimDemotionNotice, DuplicateLocalCopyIsSilent) {
    xlings::xim::PackageMatch chosen;
    chosen.canonicalName = "xim:xlings";
    chosen.version = "0.4.51";
    chosen.demoted = { { "local:xlings@0.4.51", "0.4.51" } };

    CountingMemo counting;
    auto memo = counting.memo();

    EXPECT_FALSE(xlings::xim::detail_::demotion_notice("xlings", chosen, memo))
        << "same version as the one chosen: nothing to report";
    EXPECT_EQ(counting.markCalls, 0)
        << "a pure duplicate must not even be remembered as announced";
}

TEST(XimDemotionNotice, BehindLocalCopyNotesOnce) {
    xlings::xim::PackageMatch chosen;
    chosen.canonicalName = "xim:xlings";
    chosen.version = "2026.8.11.1";
    chosen.demoted = { { "local:xlings@0.4.51", "0.4.51" } };

    CountingMemo counting;
    auto memo = counting.memo();

    EXPECT_TRUE(xlings::xim::detail_::demotion_notice("xlings", chosen, memo))
        << "a real alternative (different version) is worth a word";
    EXPECT_EQ(counting.markCalls, 1);

    // Same target, same chosen, same losers -- the Memo already has this
    // fingerprint, so the second call must say nothing.
    EXPECT_FALSE(xlings::xim::detail_::demotion_notice("xlings", chosen, memo));
    EXPECT_EQ(counting.markCalls, 1);
}

// ============================================================
// #590 — a version alias is a name for another key, not a version that
// exists on disk. Range and no-hint selection must answer with the key the
// store can hold; the exact-match branch has always dereferenced it.
// ============================================================

namespace {

mcpplibs::xpkg::Package aliased_jdk_() {
    mcpplibs::xpkg::Package pkg;
    pkg.name = "jdk-temurin";
    pkg.spec = "1";
    mcpplibs::xpkg::PlatformResource real;
    real.url = "https://example.com/jdk-25.0.4+7.tar.gz";
    mcpplibs::xpkg::PlatformResource alias;
    alias.ref = "25.0.4+7";
    mcpplibs::xpkg::PlatformResource latest;
    latest.ref = "25.0.4+7";
    pkg.xpm.entries["linux"]["25.0.4+7"] = real;
    pkg.xpm.entries["linux"]["25.0.4"] = alias;
    pkg.xpm.entries["linux"]["latest"] = latest;
    return pkg;
}

}  // namespace

TEST(XimSelectVersionAlias, RangeHintNeverPicksAnAliasKey) {
    const auto pkg = aliased_jdk_();
    EXPECT_EQ(xlings::xim::detail_::select_version_(pkg, "linux", ">=11"),
              "25.0.4+7");
    EXPECT_EQ(xlings::xim::detail_::select_version_(pkg, "linux", "^25"),
              "25.0.4+7");
}

TEST(XimSelectVersionAlias, ExactAliasHintStillDereferences) {
    const auto pkg = aliased_jdk_();
    EXPECT_EQ(xlings::xim::detail_::select_version_(pkg, "linux", "25.0.4"),
              "25.0.4+7");
}

TEST(XimSelectVersionAlias, NoHintWithoutLatestPicksAConcreteKey) {
    mcpplibs::xpkg::Package pkg;
    pkg.name = "android-platform";
    pkg.spec = "1";
    mcpplibs::xpkg::PlatformResource real;
    real.url = "https://example.com/platform-35_r2.zip";
    mcpplibs::xpkg::PlatformResource alias;
    alias.ref = "35-r2";
    pkg.xpm.entries["linux"]["35-r2"] = real;
    pkg.xpm.entries["linux"]["35"] = alias;
    EXPECT_EQ(xlings::xim::detail_::select_version_(pkg, "linux", ""),
              "35-r2");
}

TEST(XimSelectVersionAlias, AnAliasToAMissingKeyStillNamesItsTarget) {
    mcpplibs::xpkg::Package pkg;
    pkg.name = "x";
    pkg.spec = "1";
    mcpplibs::xpkg::PlatformResource alias;
    alias.ref = "2.0.0";
    pkg.xpm.entries["linux"]["2"] = alias;
    // The alias is the only entry; its target is what a store directory would
    // be called, so that is the answer -- never the alias string.
    EXPECT_EQ(xlings::xim::detail_::select_version_(pkg, "linux", ">=1"),
              "2.0.0");
}

// 2026.9.12 F6: concrete_versions_ used to follow exactly ONE ref hop, so a
// CHAIN of aliases ("8" -> "8.0" -> "8.0.1", the real, concrete key) left
// ">=8" resolving to "8.0" -- itself just another name, never a directory
// the store created. Fixed to follow the chain to its end.
TEST(XimSelectVersionAlias, ChainedAliasResolvesToItsConcreteEnd) {
    mcpplibs::xpkg::Package pkg;
    pkg.name = "openjdk-chain";
    pkg.spec = "1";
    mcpplibs::xpkg::PlatformResource real;
    real.url = "https://example.com/jdk-8.0.1.tar.gz";
    mcpplibs::xpkg::PlatformResource middleAlias;
    middleAlias.ref = "8.0.1";
    mcpplibs::xpkg::PlatformResource headAlias;
    headAlias.ref = "8.0";
    pkg.xpm.entries["linux"]["8.0.1"] = real;
    pkg.xpm.entries["linux"]["8.0"] = middleAlias;
    pkg.xpm.entries["linux"]["8"] = headAlias;

    // A range hint that only "8"'s chain can satisfy: >=8 must land on the
    // real, concrete "8.0.1" -- not on "8.0", the middle link, which is
    // itself just another alias and was never installed on its own.
    //
    // (Exact-match hints ("8", "8.0") go through select_version_'s OWN
    // direct-lookup branch, a separate one-hop dereference this finding
    // does not touch -- concrete_versions_ backs the range/prefix branch
    // only, which is what this asserts.)
    EXPECT_EQ(xlings::xim::detail_::select_version_(pkg, "linux", ">=8"),
              "8.0.1");
}
