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
import xlings.i18n;
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

// ------------------------------------------------- ScopedSubosOverride
//
// `cmd_remove`'s --subos/--all-subos loop switches Config's active-subos
// override (and its XLINGS_ACTIVE_SUBOS twin) for the duration of one
// removal, then must restore both. A code-review-caught bug: restoring the
// override BEFORE the env var made `set_active_subos_override("")` (the
// common case -- there was no override before the switch) fall through to
// reading XLINGS_ACTIVE_SUBOS, which the guard was STILL holding at the
// subos it was leaving -- so `Config::paths().activeSubos` came back wrong
// after the very first switch. These tests exercise the guard directly
// (no XLINGS_HOME is set up; Config's ambient home is whatever the process
// resolves, and the guard is read-only against it -- it only ever flips
// which subos name is "current", never writes anything).
TEST(XimCommandsTest, ScopedSubosOverrideRestoresActiveSubosOnDestruction) {
    const auto before = xlings::Config::paths().activeSubos;
    {
        xlings::xim::ScopedSubosOverride scope("xlings-scoped-subos-probe");
        EXPECT_EQ(xlings::Config::paths().activeSubos,
                 "xlings-scoped-subos-probe");
    }
    EXPECT_EQ(xlings::Config::paths().activeSubos, before)
        << "destruction must restore the subos active before the guard, not "
           "leave it on the one it switched to (the empty-override fallback "
           "reads XLINGS_ACTIVE_SUBOS, which must already be back by then)";
}

TEST(XimCommandsTest, ScopedSubosOverrideNestsBackToTheOuterSubos) {
    const auto before = xlings::Config::paths().activeSubos;
    xlings::xim::ScopedSubosOverride outer("xlings-scoped-subos-outer");
    ASSERT_EQ(xlings::Config::paths().activeSubos, "xlings-scoped-subos-outer");
    {
        xlings::xim::ScopedSubosOverride inner("xlings-scoped-subos-inner");
        EXPECT_EQ(xlings::Config::paths().activeSubos,
                 "xlings-scoped-subos-inner");
    }
    EXPECT_EQ(xlings::Config::paths().activeSubos, "xlings-scoped-subos-outer")
        << "the inner guard must restore to the OUTER override, not to "
           "whatever was active before either guard";
    (void)before;
}

TEST(XimCommandsTest, ScopedSubosOverrideEmptyNameIsANoOp) {
    const auto before = xlings::Config::paths().activeSubos;
    xlings::xim::ScopedSubosOverride scope("");
    EXPECT_EQ(xlings::Config::paths().activeSubos, before);
}

TEST(XimInventoryOwnerTest, CanonicalFilterKeepsUniqueLegacyBareRecord) {
    xlings::xvm::VersionDB db;
    db["gcc"].versions["16.1.0"].kind = "program";
    xlings::xim::detail::MetadataLookup metadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"xim-x-gcc", {.namespaceName = "xim", .name = "gcc",
                            .canonicalName = "xim:gcc"}},
        }};
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"gcc", "16.1.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    const auto related = xlings::xim::detail::build_owner_coordinates(
        db, requested, "xim:gcc", storeRoots, metadata);

    const auto found = related.find({"gcc", "16.1.0"});
    ASSERT_NE(found, related.end());
    EXPECT_EQ(found->second.coordinate.ns, "xim");
    EXPECT_EQ(found->second.coordinate.package, "gcc");
}

TEST(XimInventoryOwnerTest, ContainsFilterRecoversUniqueLegacyBareRecord) {
    xlings::xvm::VersionDB db;
    db["gcc"].versions["16.1.0"].kind = "program";
    xlings::xim::detail::MetadataLookup metadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"xim-x-gcc", {.namespaceName = "xim", .name = "gcc",
                            .canonicalName = "xim:gcc"}},
        }};
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"gcc", "16.1.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    for (const auto filter : {"xim:g", "xim"}) {
        SCOPED_TRACE(filter);
        const auto related = xlings::xim::detail::build_owner_coordinates(
            db, requested, filter, storeRoots, metadata);

        const auto found = related.find({"gcc", "16.1.0"});
        ASSERT_NE(found, related.end());
        EXPECT_EQ(found->second.coordinate.ns, "xim");
        EXPECT_EQ(found->second.coordinate.package, "gcc");
    }
}

TEST(XimInventoryOwnerTest, ContainsFilterDoesNotGuessAcrossNamespaces) {
    xlings::xvm::VersionDB db;
    db["gcc"].versions["16.1.0"].kind = "program";
    xlings::xim::detail::MetadataLookup metadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"alpha-x-gcc", {.namespaceName = "alpha", .name = "gcc",
                              .canonicalName = "alpha:gcc"}},
            {"xim-x-gcc", {.namespaceName = "xim", .name = "gcc",
                            .canonicalName = "xim:gcc"}},
        }};
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"gcc", "16.1.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    for (const auto filter : {"xim:g", "xim"}) {
        SCOPED_TRACE(filter);
        const auto related = xlings::xim::detail::build_owner_coordinates(
            db, requested, filter, storeRoots, metadata);

        EXPECT_FALSE(related.contains({"gcc", "16.1.0"}));
    }
}

TEST(XimInventoryOwnerTest, ResolvesProviderBeforeApplyingIdentityFilter) {
    xlings::xvm::VersionDB db;
    auto& data = db["tool"].versions["xim:1.0.0"];
    data.kind = "program";
    data.path = "/fixture/xpkgs/xim-x-wanted/1.0.0/bin/tool";
    data.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "other:provider",
        .providerVersion = "other:1.0.0",
        .group = "provider-group",
        .rootTarget = "tool",
        .rootVersion = "xim:1.0.0",
    };
    data.bindingMembers = {{"tool", "xim:1.0.0"}};
    data.bindingMembersDeclared = true;
    xlings::xim::detail::MetadataLookup metadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"other-x-provider", {.namespaceName = "other",
                                   .name = "provider",
                                   .canonicalName = "other:provider"}},
            {"xim-x-wanted", {.namespaceName = "xim", .name = "wanted",
                               .canonicalName = "xim:wanted"}},
        }};
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"tool", "xim:1.0.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    const auto related = xlings::xim::detail::build_owner_coordinates(
        db, requested, "xim:wanted", storeRoots, metadata, nullptr,
        xlings::xim::detail::CoordinateMatch::exact);

    EXPECT_FALSE(related.contains({"tool", "xim:1.0.0"}))
        << "filter selected the payload candidate ahead of its provider";
}

TEST(XimInventoryOwnerTest, MalformedLegacyGraphCannotDonatePeerOwner) {
    xlings::xvm::VersionDB db;
    db["member"].type = "program";
    db["member"].versions["xim:1.0.0"].kind = "program";
    db["owner"].type = "program";
    db["owner"].versions["xim:1.0.0"].kind = "program";
    db["owner"].versions["xim:1.0.0"].path =
        "/fixture/xpkgs/xim-x-owner/1.0.0/bin/owner";
    db["member"].bindings["owner"]["xim:1.0.0"] = "xim:1.0.0";
    db["owner"].bindings["member"]["xim:1.0.0"] = "xim:1.0.0";
    db["member"].bindings["dangling"]["xim:1.0.0"] = "xim:1.0.0";
    xlings::xim::detail::MetadataLookup metadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"xim-x-owner", {.namespaceName = "xim", .name = "owner",
                              .canonicalName = "xim:owner"}},
        }};
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"member", "xim:1.0.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    const auto related = xlings::xim::detail::build_owner_coordinates(
        db, requested, "xim:owner", storeRoots, metadata);

    EXPECT_FALSE(related.contains({"member", "xim:1.0.0"}))
        << "a dangling legacy component donated its otherwise valid root";
}

TEST(XimInventoryOwnerTest, AsymmetricIncomingEdgeRejectsLegacyOwner) {
    xlings::xvm::VersionDB db;
    db["member"].type = "program";
    db["member"].versions["xim:1.0.0"].kind = "program";
    db["owner"].type = "program";
    db["owner"].versions["xim:1.0.0"].kind = "program";
    db["owner"].versions["xim:1.0.0"].path =
        "/fixture/xpkgs/xim-x-owner/1.0.0/bin/owner";
    db["member"].bindings["owner"]["xim:1.0.0"] = "xim:1.0.0";
    db["owner"].bindings["member"]["xim:1.0.0"] = "xim:1.0.0";
    db["outsider"].type = "program";
    db["outsider"].versions["xim:1.0.0"].kind = "program";
    db["outsider"].bindings["member"]["xim:1.0.0"] = "xim:1.0.0";
    xlings::xim::detail::MetadataLookup metadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"xim-x-owner", {.namespaceName = "xim", .name = "owner",
                              .canonicalName = "xim:owner"}},
        }};
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"member", "xim:1.0.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    const auto related = xlings::xim::detail::build_owner_coordinates(
        db, requested, "xim:owner", storeRoots, metadata);

    EXPECT_FALSE(related.contains({"member", "xim:1.0.0"}))
        << "an asymmetric incoming edge was ignored outside the DFS component";
}

TEST(XimInventoryOwnerTest, TransitiveHealthyLegacyChainReachesOwnerHint) {
    xlings::xvm::VersionDB db;
    const std::string version = "ns:1.0.0";
    for (const auto* target : {"A", "B", "C"}) {
        db[target].type = "program";
        db[target].versions[version].kind = "program";
    }
    db["A"].bindings["B"][version] = version;
    db["B"].bindings["A"][version] = version;
    db["B"].bindings["C"][version] = version;
    db["C"].bindings["B"][version] = version;
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"A", version},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    for (const auto match : {
             xlings::xim::detail::CoordinateMatch::exact,
             xlings::xim::detail::CoordinateMatch::contains,
         }) {
        xlings::xim::detail::MetadataLookup metadata{
            std::map<std::string, xlings::xim::detail::CatalogMetadata>{
                {"ns-x-C", {.namespaceName = "ns", .name = "C",
                              .canonicalName = "ns:C"}},
            }};

        const auto related = xlings::xim::detail::build_owner_coordinates(
            db, requested, "ns:C", storeRoots, metadata, nullptr, match);

        const auto found = related.find({"A", version});
        ASSERT_NE(found, related.end());
        EXPECT_EQ(found->second.coordinate.ns, "ns");
        EXPECT_EQ(found->second.coordinate.package, "C");
    }
}

TEST(XimInventoryOwnerTest, ValidModernGroupCanFallBackToItsRoot) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef group{
        .provider = "xim:unavailable-provider",
        .providerVersion = "xim:1.0.0",
        .group = "modern-group",
        .rootTarget = "owner",
        .rootVersion = "xim:1.0.0",
    };
    auto& root = db["owner"].versions["xim:1.0.0"];
    root.kind = "program";
    root.bindingGroup = group;
    root.bindingMembers = {
        {"member", "xim:1.0.0"},
        {"owner", "xim:1.0.0"},
    };
    root.bindingMembersDeclared = true;
    auto& member = db["member"].versions["xim:1.0.0"];
    member.kind = "program";
    member.bindingGroup = group;
    xlings::xim::detail::MetadataLookup metadata{
        std::map<std::string, xlings::xim::detail::CatalogMetadata>{
            {"xim-x-owner", {.namespaceName = "xim", .name = "owner",
                              .canonicalName = "xim:owner"}},
        }};
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"member", "xim:1.0.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };

    const auto related = xlings::xim::detail::build_owner_coordinates(
        db, requested, "xim:owner", storeRoots, metadata);

    const auto found = related.find({"member", "xim:1.0.0"});
    ASSERT_NE(found, related.end());
    EXPECT_EQ(found->second.coordinate.ns, "xim");
    EXPECT_EQ(found->second.coordinate.package, "owner");
}

TEST(XimInventoryOwnerTest, ExactFilterSelectsOnlyMatchingBindingComponent) {
    xlings::xvm::VersionDB db;
    std::set<xlings::xim::detail::TargetVersion> requested;
    std::map<std::string, xlings::xim::detail::CatalogMetadata> catalog;
    constexpr std::size_t kComponentCount = 100;
    for (std::size_t index = 0; index < kComponentCount; ++index) {
        const auto suffix = std::format("{:03}", index);
        const auto target = "tool-" + suffix;
        const auto providerName = "pkg-" + suffix;
        const auto provider = "ns:" + providerName;
        const std::string version = "ns:1.0.0";
        auto& data = db[target].versions[version];
        data.kind = "program";
        data.bindingGroup = xlings::xvm::BindingGroupRef{
            .provider = provider,
            .providerVersion = version,
            .group = "group-" + suffix,
            .rootTarget = target,
            .rootVersion = version,
        };
        data.bindingMembers = {{target, version}};
        data.bindingMembersDeclared = true;
        requested.emplace(target, version);
        catalog.emplace("ns-x-" + providerName,
            xlings::xim::detail::CatalogMetadata{
                .namespaceName = "ns",
                .name = providerName,
                .canonicalName = provider,
            });
    }
    xlings::xim::detail::MetadataLookup metadata{std::move(catalog)};
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };
    xlings::xim::InventoryTrace trace;

    const auto related = xlings::xim::detail::build_owner_coordinates(
        db, requested, "ns:pkg-050", storeRoots, metadata, &trace,
        xlings::xim::detail::CoordinateMatch::exact);

    ASSERT_EQ(related.size(), 1u);
    EXPECT_TRUE(related.contains({"tool-050", "ns:1.0.0"}));
    ASSERT_EQ(trace.bindingSelections.size(), 1u);
    EXPECT_EQ(trace.bindingSelections.front(), "tool-050@ns:1.0.0");
}

TEST(XimInventoryOwnerTest,
     ExactFilterBuildsLegacyIncomingIndexOnceForMatchingComponent) {
    xlings::xvm::VersionDB db;
    std::set<xlings::xim::detail::TargetVersion> requested;
    std::map<std::string, xlings::xim::detail::CatalogMetadata> catalog;
    constexpr std::size_t kComponentCount = 100;
    for (std::size_t index = 0; index < kComponentCount; ++index) {
        const auto suffix = std::format("{:03}", index);
        const auto member = "member-" + suffix;
        const auto owner = "owner-" + suffix;
        const std::string version = "ns:1.0.0";
        db[member].type = "program";
        db[member].versions[version].kind = "program";
        db[owner].type = "program";
        db[owner].versions[version].kind = "program";
        db[member].bindings[owner][version] = version;
        db[owner].bindings[member][version] = version;
        requested.emplace(member, version);
        catalog.emplace("ns-x-" + owner,
            xlings::xim::detail::CatalogMetadata{
                .namespaceName = "ns",
                .name = owner,
                .canonicalName = "ns:" + owner,
            });
    }
    xlings::xim::detail::MetadataLookup metadata{std::move(catalog)};
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };
    xlings::xim::InventoryTrace trace;

    const auto related = xlings::xim::detail::build_owner_coordinates(
        db, requested, "ns:owner-050", storeRoots, metadata, &trace,
        xlings::xim::detail::CoordinateMatch::exact);

    ASSERT_EQ(related.size(), 1u);
    EXPECT_TRUE(related.contains({"member-050", "ns:1.0.0"}));
    ASSERT_EQ(trace.bindingSelections.size(), 1u);
    EXPECT_EQ(trace.bindingSelections.front(), "member-050@ns:1.0.0");
    EXPECT_EQ(trace.legacyIncomingIndexBuilds, 1u);
}

TEST(XimCommandsTest, ListWithFilter) {
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
    const std::set<xlings::xim::detail::TargetVersion> requested{
        {"g++", "xim:16.1.0"},
    };
    const std::vector<std::filesystem::path> storeRoots{
        "/fixture/xpkgs",
    };
    const auto related = xlings::xim::detail::build_owner_coordinates(
        bindingDb, requested, "xim:gcc", storeRoots, fixtureMetadata);
    const auto member = related.find({"g++", "xim:16.1.0"});
    ASSERT_NE(member, related.end())
        << "filter dropped a target owned through a legacy reciprocal group";
    EXPECT_EQ(member->second.coordinate.ns, "xim");
    EXPECT_EQ(member->second.coordinate.package, "gcc");

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
    if (rows.empty()) {
        EXPECT_TRUE(trace.metadataIdentities.empty());
        EXPECT_TRUE(trace.payloadVersionDirs.empty());
    } else {
        EXPECT_FALSE(trace.metadataIdentities.empty());
    }
    for (const auto& identity : trace.metadataIdentities) {
        EXPECT_TRUE(identity.contains(match->name))
            << "info loaded a target unrelated to the requested package: "
            << identity;
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

// #598/#600: a sub-index can name its own artifact source -- flat, or
// region-keyed like the git URLs above it. Before this, the lua parser read
// string values only, so the artifact path (the default mechanism) was
// reachable for top-level index_repos entries and for nothing else.
TEST(XimSubReposTest, DiscoverSubReposReadsArtifactDeclaration) {
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_subrepo_artifact_test";
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    std::string lua = R"(xim_indexrepos = {
    ["awesome"] = {
        ["GLOBAL"] = "https://github.com/openxlings/xim-pkgindex-awesome.git",
        ["CN"] = "https://gitee.com/d2learn/xim-pkgindex-awesome.git",
        ["artifact"] = {
            ["GLOBAL"] = "https://github.com/xlings-res/awesome-index",
            ["CN"] = "https://gitcode.com/xlings-res/awesome-index",
        },
        ["source"] = "auto",
    },
    ["flat"] = {
        ["GLOBAL"] = "https://github.com/o/flat.git",
        ["artifact"] = "https://example.com/idx/flat",
    },
    ["plain"] = {
        ["GLOBAL"] = "https://github.com/o/plain.git",
    }
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "xim-indexrepos.lua").string(), lua);

    auto repos = xlings::xim::discover_sub_repos(testDir, "CN");
    ASSERT_EQ(repos.size(), 3u);
    for (auto& r : repos) {
        if (r.name == "awesome") {
            EXPECT_EQ(r.url, "https://gitee.com/d2learn/xim-pkgindex-awesome.git");
            ASSERT_EQ(r.artifactBases.size(), 2u);
            EXPECT_EQ(r.artifactBases[0].url, "https://gitcode.com/xlings-res/awesome-index");
            EXPECT_EQ(r.artifactBases[1].url, "https://github.com/xlings-res/awesome-index");
            EXPECT_EQ(r.source, "auto");
        } else if (r.name == "flat") {
            ASSERT_EQ(r.artifactBases.size(), 1u);
            EXPECT_EQ(r.artifact_base(), "https://example.com/idx/flat");
        } else if (r.name == "plain") {
            // A block that declares none stays exactly as it was: git-managed,
            // and NOT reported as a broken artifact source.
            EXPECT_TRUE(r.artifactBases.empty());
            EXPECT_TRUE(r.source.empty());
        }
    }
    fs::remove_all(testDir);
}

// An artifact table written on ONE line closes on that line. Entering the
// sub-block state there would consume the enclosing block's `}` and drop the
// whole repo entry -- silently, which is the failure mode this code base keeps
// paying for.
TEST(XimSubReposTest, DiscoverSubReposReadsInlineArtifactTable) {
    namespace fs = std::filesystem;
    auto testDir = fs::temp_directory_path() / "xlings_subrepo_inline_artifact";
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    std::string lua = R"(xim_indexrepos = {
    ["one"] = {
        ["GLOBAL"] = "https://github.com/o/one.git",
        ["artifact"] = { ["GLOBAL"] = "https://github.com/x/one-index", ["CN"] = "https://gitcode.com/x/one-index" },
    },
    ["two"] = {
        ["GLOBAL"] = "https://github.com/o/two.git",
    }
}
)";
    xlings::platform::write_string_to_file(
        (testDir / "xim-indexrepos.lua").string(), lua);

    auto repos = xlings::xim::discover_sub_repos(testDir, "CN");
    ASSERT_EQ(repos.size(), 2u);            // "two" must still be there
    for (auto& r : repos) {
        if (r.name == "one") {
            ASSERT_EQ(r.artifactBases.size(), 2u);
            EXPECT_EQ(r.artifactBases[0].url, "https://gitcode.com/x/one-index");
            EXPECT_EQ(r.artifactBases[1].url, "https://github.com/x/one-index");
        } else if (r.name == "two") {
            EXPECT_EQ(r.url, "https://github.com/o/two.git");
            EXPECT_TRUE(r.artifactBases.empty());
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

// The sub_should_attempt_artifact decision table used to live here. It is gone
// with the function: sub-indexes are no longer a special case of the sync, so
// there is no separate gate to tabulate. The behaviours it encoded -- git mode
// disables artifacts, a declared artifact source always attempts, an
// undeclared name never does -- are asserted against the predicate that
// replaced it, in tests/unit/test_index_peer_sync.cpp.
//
// The C1 "migrate the sub once the main is artifact-managed" rows have no
// successor because the state they gated on is gone: every repo converges to
// its own artifact independently, so there is no main/sub split to keep in
// step.
TEST(XimSubReposTest, SubReposJsonObjectFormatRoundTrip) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "xlings-test-subrepos-json";
    fs::create_directories(dir);
    auto file = dir / "xim-indexrepos.json";

    std::vector<xlings::IndexRepo> repos;
    repos.push_back({"plain", "https://x/plain.git", {}, ""});
    repos.push_back({"custom", "https://x/custom.git",
                     {{"", "https://github.com/o/custom-index"}}, "auto"});
    xlings::xim::save_sub_repos_json(file, repos);

    auto loaded = xlings::xim::load_sub_repos_json(file);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].name, "custom");   // nlohmann object keys sort alphabetically
    EXPECT_EQ(loaded[0].url, "https://x/custom.git");
    EXPECT_EQ(loaded[0].artifact_base(), "https://github.com/o/custom-index");
    EXPECT_EQ(loaded[0].source, "auto");
    EXPECT_EQ(loaded[1].name, "plain");
    EXPECT_TRUE(loaded[1].artifactBases.empty());

    // plain entries must persist as plain strings (old-xlings tolerant)
    auto text = xlings::platform::read_file_to_string(file.string());
    auto j = nlohmann::json::parse(text);
    EXPECT_TRUE(j["plain"].is_string());
    EXPECT_TRUE(j["custom"].is_object());
    fs::remove_all(dir);
}

// #598: a region object must survive a save -> load round trip. Writing back
// the base this run happened to prefer collapsed a two-region declaration to
// one, permanently, one sync at a time.
TEST(SubReposJsonTest, RegionArtifactSurvivesRoundTrip) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "xlings_subrepos_region_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto file = dir / "xim-indexrepos.json";

    std::vector<xlings::IndexRepo> repos;
    repos.push_back({"custom", "https://x/custom.git",
                     {{"CN", "https://gitcode.com/o/i"}, {"GLOBAL", "https://github.com/o/i"}},
                     "auto"});
    xlings::xim::save_sub_repos_json(file, repos);

    auto text = xlings::platform::read_file_to_string(file.string());
    auto j = nlohmann::json::parse(text);
    ASSERT_TRUE(j["custom"]["artifact"].is_object());
    EXPECT_EQ(j["custom"]["artifact"]["CN"], "https://gitcode.com/o/i");
    EXPECT_EQ(j["custom"]["artifact"]["GLOBAL"], "https://github.com/o/i");

    auto loaded = xlings::xim::load_sub_repos_json(file);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].artifactBases.size(), 2u);   // both regions still there
    fs::remove_all(dir);
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

// ─────────────────────────────────────────────────────────────────────
// detail_::stage_extracted_payload_ — the staging function
// mcpp-community/mcpp#636 is about (xpkg-manifest-v1 §6): a hookless
// install must receive exactly the entries of its own archive, laid out
// as the archive lays them out.
//
// Before the fix this function had a second branch that collapsed a
// SINGLE top-level directory onto installDir directly, stripping it. That
// branch was unreachable as long as its caller was ever handed the shared
// runtime directory (a download's archive file always sat beside the
// extracted tree, so entries.size() was never 1) -- dead code, not a
// feature. Calling this function directly, bypassing that accident, is
// exactly how the deleted branch would have shown up: these tests are the
// regression guard for its removal.
// ─────────────────────────────────────────────────────────────────────

namespace {

fs::path make_stage_dir(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("xlings-stage-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

TEST(StageExtractedPayloadTest, KeepsASingleTopLevelDirectoryUnstripped) {
    using xlings::xim::detail_::stage_extracted_payload_;

    auto extractRoot = make_stage_dir("single-top-extract");
    auto installDir  = make_stage_dir("single-top-install");
    fs::remove_all(installDir);   // stage_extracted_payload_ may create it

    fs::create_directories(extractRoot / "gcc-13.3.0" / "bin");
    xlings::platform::write_string_to_file(
        (extractRoot / "gcc-13.3.0" / "bin" / "gcc").string(), "binary");

    EXPECT_TRUE(stage_extracted_payload_(extractRoot, installDir));

    // Kept, not stripped: the archive's own top-level directory is a
    // subdirectory of installDir, exactly as xpkg-manifest-v1 §6 requires.
    EXPECT_TRUE(fs::is_regular_file(
        installDir / "gcc-13.3.0" / "bin" / "gcc"));
    // The old stripping branch would have placed this at installDir/bin/gcc
    // directly; assert that layout did NOT happen.
    EXPECT_FALSE(fs::exists(installDir / "bin"));

    fs::remove_all(extractRoot);
    fs::remove_all(installDir);
}

TEST(StageExtractedPayloadTest, MovesEveryTopLevelEntryOfAMultiEntryArchive) {
    using xlings::xim::detail_::stage_extracted_payload_;

    auto extractRoot = make_stage_dir("multi-top-extract");
    auto installDir  = make_stage_dir("multi-top-install");
    fs::remove_all(installDir);

    fs::create_directories(extractRoot / "bin");
    fs::create_directories(extractRoot / "lib");
    xlings::platform::write_string_to_file(
        (extractRoot / "bin" / "tool").string(), "binary");
    xlings::platform::write_string_to_file(
        (extractRoot / "README.md").string(), "docs");

    EXPECT_TRUE(stage_extracted_payload_(extractRoot, installDir));

    EXPECT_TRUE(fs::is_regular_file(installDir / "bin" / "tool"));
    EXPECT_TRUE(fs::is_directory(installDir / "lib"));
    EXPECT_TRUE(fs::is_regular_file(installDir / "README.md"));

    fs::remove_all(extractRoot);
    fs::remove_all(installDir);
}

TEST(StageExtractedPayloadTest, AnAlreadyNonEmptyInstallDirIsLeftUntouched) {
    using xlings::xim::detail_::stage_extracted_payload_;

    auto extractRoot = make_stage_dir("noop-extract");
    auto installDir  = make_stage_dir("noop-install");

    fs::create_directories(extractRoot / "payload");
    xlings::platform::write_string_to_file(
        (installDir / "already-here").string(), "sentinel");

    EXPECT_TRUE(stage_extracted_payload_(extractRoot, installDir));

    // The pre-existing file survived, and nothing from extractRoot landed:
    // a non-empty installDir short-circuits, it is never merged into.
    EXPECT_TRUE(fs::is_regular_file(installDir / "already-here"));
    EXPECT_FALSE(fs::exists(installDir / "payload"));

    fs::remove_all(extractRoot);
    fs::remove_all(installDir);
}

TEST(StageExtractedPayloadTest, AnEmptyExtractRootFails) {
    using xlings::xim::detail_::stage_extracted_payload_;

    auto extractRoot = make_stage_dir("empty-extract");
    auto installDir  = make_stage_dir("empty-install");
    fs::remove_all(installDir);

    EXPECT_FALSE(stage_extracted_payload_(extractRoot, installDir));

    fs::remove_all(extractRoot);
    fs::remove_all(installDir);
}

// ─────────────────────────────────────────────────────────────────────
// detail_::private_stage_dir_ — the per-installation extraction directory
// a hookless (or hook-that-did-nothing) install stages from. Same volume
// as runtimeDir, sanitized to a single safe path component, unique per
// process so two concurrent installs of the same package cannot collide.
// ─────────────────────────────────────────────────────────────────────

TEST(PrivateStageDirTest, LivesUnderADotStageSubdirectoryOfRuntimeDir) {
    using xlings::xim::detail_::private_stage_dir_;

    auto runtimeDir = fs::path("/home/x/.xlings/data/runtimedir");
    auto dir = private_stage_dir_(runtimeDir, "xim:gcc@13.3.0");

    EXPECT_EQ(dir.parent_path().parent_path(), runtimeDir);
    EXPECT_EQ(dir.parent_path().filename(), ".stage");
}

TEST(PrivateStageDirTest, SanitizesCharactersAWindowsPathCannotCarry) {
    using xlings::xim::detail_::private_stage_dir_;

    auto dir = private_stage_dir_("/runtimedir", "xim:gcc@13.3.0");
    auto name = dir.filename().string();

    EXPECT_EQ(name.find(':'), std::string::npos);
    EXPECT_EQ(name.find('@'), std::string::npos);
    // The sanitized plan key is still a recognizable prefix of the name.
    EXPECT_EQ(name.rfind("xim_gcc_13.3.0-", 0), 0u);
}

TEST(PrivateStageDirTest, TwoDifferentPlanKeysNeverCollide) {
    using xlings::xim::detail_::private_stage_dir_;

    auto a = private_stage_dir_("/runtimedir", "xim:gcc@13.3.0");
    auto b = private_stage_dir_("/runtimedir", "xim:gcc@16.1.0");
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────
// xim::swept_payload_marker — the sweep fingerprint doctor's
// FindingKind::SweptPayload is built on (mcpp-community/mcpp#636).
//
// Anchored on a zero-length "<name>.lock" (only the downloader ever
// writes one, and only in runtimedir): a filename ALONE, however much it
// looks like a download, must never be enough -- a package's own archive
// can legitimately carry a top-level "setup.exe" or "data.zip" as its own
// content. The negative cases here are the regression guard for exactly
// that false positive.
// ─────────────────────────────────────────────────────────────────────

namespace {

fs::path make_marker_dir(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("xlings-swept-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_zero_length(const fs::path& file) {
    std::ofstream(file, std::ios::binary);  // open + close: 0 bytes
}

}  // namespace

TEST(SweptPayloadMarkerTest, ADownloadShapedNameAloneIsNotFlagged) {
    using xlings::xim::swept_payload_marker;

    auto dir = make_marker_dir("names-alone");
    xlings::platform::write_string_to_file(
        (dir / "setup.exe").string(), "not a download, just this package's own file");
    xlings::platform::write_string_to_file(
        (dir / "data.zip").string(), "also this package's own content");
    xlings::platform::write_string_to_file(
        (dir / "notes.meta").string(), "package-authored metadata, not a sidecar");
    // A NON-empty "*.lock" -- e.g. a bundled yarn.lock -- must not match
    // either: the anchor requires the lock file itself to be zero-length.
    xlings::platform::write_string_to_file(
        (dir / "yarn.lock").string(), "# THIS IS AN AUTOGENERATED FILE...\n");

    EXPECT_EQ(swept_payload_marker(dir), "");
    fs::remove_all(dir);
}

TEST(SweptPayloadMarkerTest, FlagsAZeroLengthLockPairedWithItsSibling) {
    using xlings::xim::swept_payload_marker;

    auto dir = make_marker_dir("sibling-pair");
    xlings::platform::write_string_to_file(
        (dir / "hooked.tar.gz").string(), "archive bytes");
    write_zero_length(dir / "hooked.tar.gz.lock");

    EXPECT_EQ(swept_payload_marker(dir), "hooked.tar.gz.lock");
    fs::remove_all(dir);
}

TEST(SweptPayloadMarkerTest, FlagsAnOrphanLockWhoseBaseIsADownloadName) {
    using xlings::xim::swept_payload_marker;

    // The archive side already moved (or the download never finished),
    // leaving only the lock -- exactly the measured
    // "glibc-2.44.2-linux-x86_64.tar.gz.lock" shape.
    auto dir = make_marker_dir("orphan-lock");
    write_zero_length(dir / "glibc-2.44.2-linux-x86_64.tar.gz.lock");

    EXPECT_EQ(swept_payload_marker(dir), "glibc-2.44.2-linux-x86_64.tar.gz.lock");
    fs::remove_all(dir);
}

TEST(SweptPayloadMarkerTest, FlagsALockCorroboratedByAMetaSidecarAlone) {
    using xlings::xim::swept_payload_marker;

    // Neither a sibling "LICENSE.TXT" nor a download-shaped extension --
    // only the downloader's own ".meta" sidecar corroborates the lock.
    auto dir = make_marker_dir("meta-only");
    write_zero_length(dir / "LICENSE.TXT.lock");
    xlings::platform::write_string_to_file(
        (dir / "LICENSE.TXT.meta").string(), "{\"size\":1071}");

    EXPECT_EQ(swept_payload_marker(dir), "LICENSE.TXT.lock");
    fs::remove_all(dir);
}

TEST(SweptPayloadMarkerTest, ANonEmptyLockIsNotFlagged) {
    using xlings::xim::swept_payload_marker;

    auto dir = make_marker_dir("nonempty-lock");
    xlings::platform::write_string_to_file(
        (dir / "hooked.tar.gz").string(), "archive bytes");
    // Not zero-length: the anchor is specifically what the downloader's
    // FileLock leaves behind, and that lock is always empty.
    xlings::platform::write_string_to_file(
        (dir / "hooked.tar.gz.lock").string(), "pid=12345\n");

    EXPECT_EQ(swept_payload_marker(dir), "");
    fs::remove_all(dir);
}

TEST(SweptPayloadMarkerTest, ADirectoryNamedLikeALockIsNotFlagged) {
    using xlings::xim::swept_payload_marker;

    auto dir = make_marker_dir("dir-not-file");
    fs::create_directories(dir / "hooked.tar.gz.lock");

    EXPECT_EQ(swept_payload_marker(dir), "");
    fs::remove_all(dir);
}

TEST(SweptPayloadMarkerTest, ACleanPayloadIsNotFlagged) {
    using xlings::xim::swept_payload_marker;

    auto dir = make_marker_dir("clean");
    fs::create_directories(dir / "bin");
    xlings::platform::write_string_to_file((dir / "bin" / "tool").string(), "binary");
    xlings::platform::write_string_to_file(
        (dir / ".xpkg-install.json").string(), "{}");

    EXPECT_EQ(swept_payload_marker(dir), "");
    fs::remove_all(dir);
}
