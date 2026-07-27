// tests/unit/test_xvm_bindings.cpp — the version database, its JSON round-trip, and the binding tree:
// registration, selection, removal and the xim -> xvm adapter.
//
// Split out of the former single 12.7k-line test_main.cpp. Section order
// and contents are unchanged; only the file boundary is new.
//
// Why the version database, its JSON round-trip and the binding tree share
// one file: GCC 16 ICEs (in finish_member_declaration, stl_map.h:107) when
// the first instantiation of std::map<std::string, xvm::VInfo> in a
// translation unit is required from a namespace-scope helper -- which is
// what the binding-tree fixtures are. The db and JSON sections instantiate
// it inside TEST bodies first, which is the accident that kept the old
// single test_main.cpp compiling. Splitting them apart reproduces the ICE,
// and an ICE additionally leaves a truncated .gcm behind that fails
// unrelated targets with "Bad file data". Keep them together until the
// compiler bug is fixed.

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
// xvm types tests
// ============================================================

TEST(XvmTypesTest, VDataConstruction) {
    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    vdata.alias.push_back("15");
    vdata.envs["GCC_HOME"] = "/usr/lib/gcc";

    EXPECT_EQ(vdata.path, "/usr/bin");
    ASSERT_EQ(vdata.alias.size(), 1u);
    EXPECT_EQ(vdata.alias[0], "15");
    ASSERT_EQ(vdata.envs.size(), 1u);
    EXPECT_EQ(vdata.envs.at("GCC_HOME"), "/usr/lib/gcc");
}

TEST(XvmTypesTest, VInfoConstruction) {
    xlings::xvm::VInfo info;
    info.type = "program";
    info.filename = "gcc";

    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    info.versions["15.1.0"] = std::move(vdata);

    info.bindings["g++"]["15.1.0"] = "g++-15";

    EXPECT_EQ(info.type, "program");
    EXPECT_EQ(info.filename, "gcc");
    EXPECT_EQ(info.versions.size(), 1u);
    EXPECT_TRUE(info.versions.contains("15.1.0"));
    EXPECT_EQ(info.bindings["g++"]["15.1.0"], "g++-15");
}

TEST(XvmTypesTest, VersionDBAndWorkspace) {
    xlings::xvm::VersionDB db;
    EXPECT_TRUE(db.empty());

    xlings::xvm::VInfo info;
    info.type = "program";
    db["gcc"] = std::move(info);
    EXPECT_EQ(db.size(), 1u);

    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    EXPECT_EQ(ws["gcc"], "15.1.0");
}

// ============================================================
// xvm db tests
// ============================================================

TEST(XvmDbTest, AddAndRemoveVersion) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin", "program", "gcc");
    EXPECT_TRUE(xlings::xvm::has_target(db, "gcc"));
    EXPECT_TRUE(xlings::xvm::has_version(db, "gcc", "15.1.0"));

    xlings::xvm::add_version(db, "gcc", "14.2.0", "/opt/gcc14/bin", "program", "gcc");
    EXPECT_TRUE(xlings::xvm::has_version(db, "gcc", "14.2.0"));

    auto all = xlings::xvm::get_all_versions(db, "gcc");
    EXPECT_EQ(all.size(), 2u);

    ASSERT_TRUE(
        xlings::xvm::remove_version(db, "gcc", "14.2.0").has_value());
    EXPECT_FALSE(xlings::xvm::has_version(db, "gcc", "14.2.0"));
    EXPECT_TRUE(xlings::xvm::has_version(db, "gcc", "15.1.0"));

    // Remove last version removes the target entirely
    ASSERT_TRUE(
        xlings::xvm::remove_version(db, "gcc", "15.1.0").has_value());
    EXPECT_FALSE(xlings::xvm::has_target(db, "gcc"));
}

TEST(XvmDbTest, FuzzyVersionMatch) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin");
    xlings::xvm::add_version(db, "gcc", "14.2.0", "/opt/gcc14/bin");
    xlings::xvm::add_version(db, "gcc", "14.1.0", "/opt/gcc141/bin");
    xlings::xvm::add_version(db, "gcc", "13.3.0", "/opt/gcc13/bin");

    // Exact match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "15.1.0"), "15.1.0");

    // Prefix match: "15" -> "15.1.0"
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "15"), "15.1.0");

    // Prefix match: "14" -> "14.2.0" (highest)
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "14"), "14.2.0");

    // Prefix match: "14.1" -> "14.1.0"
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "14.1"), "14.1.0");

    // No match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "16"), "");
    EXPECT_EQ(xlings::xvm::match_version(db, "nonexistent", "1"), "");
}

TEST(XvmDbTest, GetActiveVersion) {
    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    ws["node"] = "22.0.0";

    EXPECT_EQ(xlings::xvm::get_active_version(ws, "gcc"), "15.1.0");
    EXPECT_EQ(xlings::xvm::get_active_version(ws, "node"), "22.0.0");
    EXPECT_EQ(xlings::xvm::get_active_version(ws, "python"), "");
}

TEST(XvmDbTest, GetVDataAndVInfo) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin", "program", "gcc");

    auto* vinfo = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(vinfo, nullptr);
    EXPECT_EQ(vinfo->type, "program");
    EXPECT_EQ(vinfo->filename, "gcc");

    auto* vdata = xlings::xvm::get_vdata(db, "gcc", "15.1.0");
    ASSERT_NE(vdata, nullptr);
    EXPECT_EQ(vdata->path, "/usr/bin");

    EXPECT_EQ(xlings::xvm::get_vdata(db, "gcc", "99.0.0"), nullptr);
    EXPECT_EQ(xlings::xvm::get_vinfo(db, "nonexistent"), nullptr);
}

TEST(XvmDbTest, AddVersionStoresMaterializationMetadataPerVersion) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "tool", "1.0.0", "/provider-a", "program", "tool-a", "", "repo-a");
    xlings::xvm::add_version(
        db, "tool", "2.0.0", "/provider-b", "lib", "libtool-b.so", "", "repo-b");

    const auto* first =
        xlings::xvm::get_vdata(db, "tool", "repo-a:1.0.0");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->kind, "program");
    EXPECT_EQ(first->sourceName, "tool-a");
    EXPECT_EQ(first->destinationName, "tool");

    const auto* second =
        xlings::xvm::get_vdata(db, "tool", "repo-b:2.0.0");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->kind, "lib");
    EXPECT_EQ(second->sourceName, "libtool-b.so");
    EXPECT_EQ(second->destinationName, "libtool-b.so");

    const auto* legacyInfo = xlings::xvm::get_vinfo(db, "tool");
    ASSERT_NE(legacyInfo, nullptr);
    EXPECT_EQ(legacyInfo->type, "program");
    EXPECT_EQ(legacyInfo->filename, "tool-a");
}

TEST(XvmDbTest, AddVersionPreservesVirtualGroupWithoutProgramPayload) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "provider-root", "1.0.0", "/provider", "group");

    const auto* root =
        xlings::xvm::get_vdata(db, "provider-root", "1.0.0");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->kind, "group");
    EXPECT_TRUE(root->sourceName.empty());
    EXPECT_TRUE(root->destinationName.empty());

    const auto* legacyInfo = xlings::xvm::get_vinfo(db, "provider-root");
    ASSERT_NE(legacyInfo, nullptr);
    EXPECT_EQ(legacyInfo->type, "group");
    EXPECT_TRUE(legacyInfo->filename.empty());
}

TEST(XvmDbTest, GetBinding) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin");
    db["gcc"].bindings["g++"]["15.1.0"] = "g++-15";
    db["gcc"].bindings["g++"]["14.2.0"] = "g++-14";

    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "g++", "15.1.0"), "g++-15");
    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "g++", "14.2.0"), "g++-14");
    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "g++", "99.0.0"), "");
    EXPECT_EQ(xlings::xvm::get_binding(db, "gcc", "clang", "15.1.0"), "");
}

TEST(XvmDbTest, ExpandPath) {
    EXPECT_EQ(xlings::xvm::expand_path("${XLINGS_HOME}/data/xpkgs/gcc", "/home/user/.xlings"),
              "/home/user/.xlings/data/xpkgs/gcc");
    EXPECT_EQ(xlings::xvm::expand_path("/absolute/path", "/home/user/.xlings"),
              "/absolute/path");
    EXPECT_EQ(xlings::xvm::expand_path("${XLINGS_HOME}/a/${XLINGS_HOME}/b", "/X"),
              "/X/a//X/b");
    EXPECT_EQ(xlings::xvm::expand_path("no_placeholder", "/X"), "no_placeholder");
}

// ============================================================
// xvm JSON serialization tests
// ============================================================

namespace {

nlohmann::json valid_binding_group_json() {
    return {
        {"provider", "repo:provider"},
        {"version", "1.0.0"},
        {"group", "provider-group"},
        {"rootTarget", "provider-root"},
        {"rootVersion", "1.0.0"},
    };
}

xlings::xvm::VData reload_vdata(const xlings::xvm::VData& data) {
    return xlings::xvm::vdata_from_json(
        xlings::xvm::vdata_to_json(data));
}

void expect_single_binding_integrity_issue(
    const xlings::xvm::VData& data,
    std::string_view code,
    std::string_view path) {
    ASSERT_EQ(data.bindingIntegrityIssues.size(), 1u);
    EXPECT_EQ(data.bindingIntegrityIssues[0].code, code);
    EXPECT_EQ(data.bindingIntegrityIssues[0].path, path);
}

void expect_metadata_integrity_failure(
    const xlings::xvm::VData& data,
    std::string_view code,
    std::string_view path) {
    xlings::xvm::VersionDB db;
    db["subject"].type = "program";
    db["subject"].versions["1.0.0"] = data;

    auto result =
        xlings::xvm::resolve_binding_selection(db, "subject", "1.0.0");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xvm::BindingErrorKind::MetadataIntegrityIssue);
    EXPECT_EQ(result.error().target, "subject");
    EXPECT_EQ(result.error().version, "1.0.0");
    EXPECT_NE(result.error().message.find(code), std::string::npos);
    EXPECT_NE(result.error().message.find(path), std::string::npos);
}

}  // namespace

TEST(XvmJsonTest, VDataRoundTrip) {
    xlings::xvm::VData original;
    original.path = "/usr/bin";
    original.alias = {"15", "latest"};
    original.envs["GCC_HOME"] = "/usr/lib/gcc";
    original.envs["PATH"] = "/usr/bin";

    auto j = xlings::xvm::vdata_to_json(original);
    auto restored = xlings::xvm::vdata_from_json(j);

    EXPECT_EQ(restored.path, original.path);
    EXPECT_EQ(restored.alias, original.alias);
    EXPECT_EQ(restored.envs, original.envs);
}

TEST(XvmJsonTest, VDataMinimal) {
    xlings::xvm::VData original;
    original.path = "/usr/bin";
    // No alias, no envs

    auto j = xlings::xvm::vdata_to_json(original);
    EXPECT_FALSE(j.contains("alias"));
    EXPECT_FALSE(j.contains("envs"));

    auto restored = xlings::xvm::vdata_from_json(j);
    EXPECT_EQ(restored.path, "/usr/bin");
    EXPECT_TRUE(restored.alias.empty());
    EXPECT_TRUE(restored.envs.empty());
}

TEST(XvmJsonTest, BindingGroupManifestAndMaterializationRoundTrip) {
    xlings::xvm::VData original;
    original.path = "/pkg/gcc-15";
    original.kind = "group";
    original.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "xim:gcc",
        .providerVersion = "15.1.0",
        .group = "xim-gnu-gcc",
        .rootTarget = "xim-gnu-gcc",
        .rootVersion = "xim:15.1.0",
    };
    original.bindingMembers = {
        {"g++", "xim:15.1.0"},
        {"gcc", "xim:15.1.0"},
        {"gcc-ar", "xim:gcc-15.1.0"},
        {"xim-gnu-gcc", "xim:15.1.0"},
    };
    original.bindingHeaders = {
        {
            .sourceDir = "include/c++/15.1.0",
            .destinationPrefix = "c++/15.1.0",
        },
        {
            .sourceDir = "include-fixed",
            .destinationPrefix = "",
        },
    };

    auto j = xlings::xvm::vdata_to_json(original);
    ASSERT_TRUE(j.contains("bindingGroup"));
    EXPECT_EQ(j["bindingGroup"]["provider"], "xim:gcc");
    EXPECT_EQ(j["bindingGroup"]["version"], "15.1.0");
    EXPECT_EQ(j["bindingGroup"]["group"], "xim-gnu-gcc");
    EXPECT_EQ(j["bindingGroup"]["rootTarget"], "xim-gnu-gcc");
    EXPECT_EQ(j["bindingGroup"]["rootVersion"], "xim:15.1.0");
    EXPECT_EQ(j["bindingMembers"]["gcc-ar"], "xim:gcc-15.1.0");
    EXPECT_EQ(j["kind"], "group");
    EXPECT_FALSE(j.contains("sourceName"));
    EXPECT_FALSE(j.contains("destinationName"));
    ASSERT_EQ(j["bindingHeaders"].size(), 2u);
    EXPECT_EQ(j["bindingHeaders"][0]["sourceDir"], "include/c++/15.1.0");
    EXPECT_EQ(j["bindingHeaders"][0]["destinationPrefix"], "c++/15.1.0");
    EXPECT_EQ(j["bindingHeaders"][1]["sourceDir"], "include-fixed");
    EXPECT_EQ(j["bindingHeaders"][1]["destinationPrefix"], "");
    EXPECT_FALSE(j.contains("bindingIntegrityIssues"));

    auto restored = xlings::xvm::vdata_from_json(j);
    ASSERT_TRUE(restored.bindingGroup.has_value());
    EXPECT_EQ(restored.bindingGroup->provider, "xim:gcc");
    EXPECT_EQ(restored.bindingGroup->providerVersion, "15.1.0");
    EXPECT_EQ(restored.bindingGroup->group, "xim-gnu-gcc");
    EXPECT_EQ(restored.bindingGroup->rootTarget, "xim-gnu-gcc");
    EXPECT_EQ(restored.bindingGroup->rootVersion, "xim:15.1.0");
    EXPECT_EQ(restored.bindingMembers, original.bindingMembers);
    EXPECT_EQ(restored.kind, "group");
    EXPECT_TRUE(restored.sourceName.empty());
    EXPECT_TRUE(restored.destinationName.empty());
    ASSERT_EQ(restored.bindingHeaders.size(), 2u);
    EXPECT_EQ(restored.bindingHeaders[0].sourceDir, "include/c++/15.1.0");
    EXPECT_EQ(restored.bindingHeaders[0].destinationPrefix, "c++/15.1.0");
    EXPECT_EQ(restored.bindingHeaders[1].sourceDir, "include-fixed");
    EXPECT_TRUE(restored.bindingHeaders[1].destinationPrefix.empty());
    EXPECT_TRUE(restored.bindingIntegrityIssues.empty());
}

TEST(XvmJsonTest, LegacyVDataOmitsProviderAndMaterializationMetadata) {
    auto legacy = nlohmann::json::parse(R"({
        "path": "/usr/bin",
        "alias": ["15"]
    })");

    auto restored = xlings::xvm::vdata_from_json(legacy);
    EXPECT_FALSE(restored.bindingGroup.has_value());
    EXPECT_TRUE(restored.bindingMembers.empty());
    EXPECT_TRUE(restored.kind.empty());
    EXPECT_TRUE(restored.sourceName.empty());
    EXPECT_TRUE(restored.destinationName.empty());
    EXPECT_TRUE(restored.bindingHeaders.empty());
    EXPECT_TRUE(restored.bindingIntegrityIssues.empty());

    auto serialized = xlings::xvm::vdata_to_json(restored);
    EXPECT_FALSE(serialized.contains("bindingGroup"));
    EXPECT_FALSE(serialized.contains("bindingMembers"));
    EXPECT_FALSE(serialized.contains("kind"));
    EXPECT_FALSE(serialized.contains("sourceName"));
    EXPECT_FALSE(serialized.contains("destinationName"));
    EXPECT_FALSE(serialized.contains("bindingHeaders"));
    EXPECT_FALSE(serialized.contains("bindingIntegrityIssues"));
}

TEST(XvmJsonTest,
     MalformedCanonicalEntriesRecordAndPersistIntegrityIssues) {
    auto corrupt = nlohmann::json::parse(R"({
        "path": "/pkg/provider",
        "bindingGroup": {
            "provider": "repo:provider",
            "version": "1.0.0",
            "group": "provider-group",
            "rootTarget": "provider-root",
            "rootVersion": "1.0.0"
        },
        "bindingMembers": {
            "provider-root": "1.0.0",
            "bad/member~name": 7
        },
        "bindingHeaders": [
            {
                "sourceDir": "include",
                "destinationPrefix": ""
            },
            {
                "sourceDir": false,
                "destinationPrefix": "broken"
            },
            {
                "sourceDir": "include-valid",
                "destinationPrefix": 9
            }
        ]
    })");

    auto parsed = xlings::xvm::vdata_from_json(corrupt);

    ASSERT_EQ(parsed.bindingMembers.size(), 1u);
    EXPECT_EQ(parsed.bindingMembers.at("provider-root"), "1.0.0");
    ASSERT_EQ(parsed.bindingHeaders.size(), 1u);
    EXPECT_EQ(parsed.bindingHeaders.front().sourceDir, "include");
    ASSERT_EQ(parsed.bindingIntegrityIssues.size(), 3u);
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].code,
              "binding-member-version-not-string");
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].path,
              "/bindingMembers/bad~1member~0name");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].code,
              "binding-header-source-dir-not-string");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].path,
              "/bindingHeaders/1/sourceDir");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].code,
              "binding-header-destination-prefix-not-string");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].path,
              "/bindingHeaders/2/destinationPrefix");

    auto serialized = xlings::xvm::vdata_to_json(parsed);
    ASSERT_TRUE(serialized.contains("bindingIntegrityIssues"));
    ASSERT_EQ(serialized["bindingIntegrityIssues"].size(), 3u);
    EXPECT_EQ(serialized["bindingIntegrityIssues"][0]["code"],
              "binding-member-version-not-string");
    EXPECT_EQ(serialized["bindingIntegrityIssues"][0]["path"],
              "/bindingMembers/bad~1member~0name");

    auto restored = xlings::xvm::vdata_from_json(serialized);
    ASSERT_EQ(restored.bindingIntegrityIssues.size(), 3u);
    EXPECT_EQ(restored.bindingIntegrityIssues[0].code,
              "binding-member-version-not-string");
    EXPECT_EQ(restored.bindingIntegrityIssues[0].path,
              "/bindingMembers/bad~1member~0name");
    EXPECT_EQ(restored.bindingIntegrityIssues[1].code,
              "binding-header-source-dir-not-string");
    EXPECT_EQ(restored.bindingIntegrityIssues[1].path,
              "/bindingHeaders/1/sourceDir");
    EXPECT_EQ(restored.bindingIntegrityIssues[2].code,
              "binding-header-destination-prefix-not-string");
    EXPECT_EQ(restored.bindingIntegrityIssues[2].path,
              "/bindingHeaders/2/destinationPrefix");
}

TEST(XvmJsonTest, MalformedCanonicalContainersRecordIntegrityIssues) {
    auto corrupt = nlohmann::json::parse(R"({
        "path": "/pkg/provider",
        "bindingMembers": [],
        "bindingHeaders": {}
    })");

    auto parsed = xlings::xvm::vdata_from_json(corrupt);

    ASSERT_EQ(parsed.bindingIntegrityIssues.size(), 3u);
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].code,
              "binding-members-not-object");
    EXPECT_EQ(parsed.bindingIntegrityIssues[0].path, "/bindingMembers");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].code,
              "binding-headers-not-array");
    EXPECT_EQ(parsed.bindingIntegrityIssues[1].path, "/bindingHeaders");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].code,
              "binding-group-missing");
    EXPECT_EQ(parsed.bindingIntegrityIssues[2].path, "/bindingGroup");
}

TEST(XvmJsonTest, NonObjectBindingGroupPersistsIntegrityFailure) {
    nlohmann::json malformed{
        {"path", "/subject"},
        {"kind", "program"},
        {"bindingGroup", false},
    };

    auto parsed = xlings::xvm::vdata_from_json(malformed);
    expect_single_binding_integrity_issue(
        parsed, "binding-group-not-object", "/bindingGroup");
    expect_metadata_integrity_failure(
        parsed, "binding-group-not-object", "/bindingGroup");

    for (int cycle = 0; cycle < 3; ++cycle) {
        SCOPED_TRACE(cycle);
        parsed = reload_vdata(parsed);
        expect_single_binding_integrity_issue(
            parsed, "binding-group-not-object", "/bindingGroup");
        expect_metadata_integrity_failure(
            parsed, "binding-group-not-object", "/bindingGroup");
    }
}

TEST(XvmJsonTest, EveryInvalidBindingGroupFieldPersistsIntegrityFailure) {
    const std::array<std::string_view, 5> fields{
        "provider",
        "version",
        "group",
        "rootTarget",
        "rootVersion",
    };
    const std::array<std::string_view, 3> invalidClasses{
        "missing",
        "wrong-type",
        "empty",
    };

    for (const auto field : fields) {
        for (const auto invalidClass : invalidClasses) {
            SCOPED_TRACE(std::string(field) + ":" + std::string(invalidClass));
            nlohmann::json malformed{
                {"path", "/subject"},
                {"kind", "program"},
                {"bindingGroup", valid_binding_group_json()},
            };
            auto& group = malformed["bindingGroup"];
            if (invalidClass == "missing") {
                group.erase(std::string(field));
            } else if (invalidClass == "wrong-type") {
                group[std::string(field)] = false;
            } else {
                group[std::string(field)] = "";
            }
            const auto path = "/bindingGroup/" + std::string(field);

            auto parsed = xlings::xvm::vdata_from_json(malformed);
            expect_single_binding_integrity_issue(
                parsed, "binding-group-field-invalid", path);
            expect_metadata_integrity_failure(
                parsed, "binding-group-field-invalid", path);

            for (int cycle = 0; cycle < 3; ++cycle) {
                SCOPED_TRACE(cycle);
                parsed = reload_vdata(parsed);
                expect_single_binding_integrity_issue(
                    parsed, "binding-group-field-invalid", path);
                expect_metadata_integrity_failure(
                    parsed, "binding-group-field-invalid", path);
            }
        }
    }
}

TEST(XvmJsonTest, MalformedPersistedIssueStateBecomesDurableFailure) {
    struct Case {
        std::string_view name;
        nlohmann::json issueState;
        std::string_view code;
        std::string_view path;
    };
    const std::vector<Case> cases{
        {
            "container-not-array",
            nlohmann::json::object(),
            "binding-integrity-issues-not-array",
            "/bindingIntegrityIssues",
        },
        {
            "item-not-object",
            nlohmann::json::array({false}),
            "binding-integrity-issue-not-object",
            "/bindingIntegrityIssues/0",
        },
        {
            "code-missing",
            nlohmann::json::array({
                {{"path", "/bindingGroup/provider"}},
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/code",
        },
        {
            "code-wrong-type",
            nlohmann::json::array({
                {
                    {"code", false},
                    {"path", "/bindingGroup/provider"},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/code",
        },
        {
            "code-empty",
            nlohmann::json::array({
                {
                    {"code", ""},
                    {"path", "/bindingGroup/provider"},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/code",
        },
        {
            "path-missing",
            nlohmann::json::array({
                {{"code", "existing-integrity-issue"}},
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/path",
        },
        {
            "path-wrong-type",
            nlohmann::json::array({
                {
                    {"code", "existing-integrity-issue"},
                    {"path", false},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/path",
        },
        {
            "path-empty",
            nlohmann::json::array({
                {
                    {"code", "existing-integrity-issue"},
                    {"path", ""},
                },
            }),
            "binding-integrity-issue-field-invalid",
            "/bindingIntegrityIssues/0/path",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        nlohmann::json malformed{
            {"path", "/subject"},
            {"kind", "program"},
            {"bindingIntegrityIssues", testCase.issueState},
        };

        auto parsed = xlings::xvm::vdata_from_json(malformed);
        expect_single_binding_integrity_issue(
            parsed, testCase.code, testCase.path);
        expect_metadata_integrity_failure(
            parsed, testCase.code, testCase.path);

        for (int cycle = 0; cycle < 3; ++cycle) {
            SCOPED_TRACE(cycle);
            parsed = reload_vdata(parsed);
            expect_single_binding_integrity_issue(
                parsed, testCase.code, testCase.path);
            expect_metadata_integrity_failure(
                parsed, testCase.code, testCase.path);
        }
    }
}

TEST(XvmJsonTest, DuplicateDerivedIntegrityIssuesCollapseAcrossReloads) {
    nlohmann::json malformed{
        {"path", "/subject"},
        {"kind", "program"},
        {"bindingGroup", valid_binding_group_json()},
        {
            "bindingIntegrityIssues",
            nlohmann::json::array({
                {
                    {"code", "binding-group-field-invalid"},
                    {"path", "/bindingGroup/provider"},
                },
                {
                    {"code", "binding-group-field-invalid"},
                    {"path", "/bindingGroup/provider"},
                },
            }),
        },
    };
    malformed["bindingGroup"]["provider"] = "";

    auto parsed = xlings::xvm::vdata_from_json(malformed);
    for (int cycle = 0; cycle < 4; ++cycle) {
        SCOPED_TRACE(cycle);
        expect_single_binding_integrity_issue(
            parsed,
            "binding-group-field-invalid",
            "/bindingGroup/provider");
        expect_metadata_integrity_failure(
            parsed,
            "binding-group-field-invalid",
            "/bindingGroup/provider");
        parsed = reload_vdata(parsed);
    }
}

TEST(XvmJsonTest, EmptyBindingMemberTargetAndVersionPersistFailures) {
    struct Case {
        std::string_view name;
        std::string target;
        std::string version;
        std::string_view code;
        std::string_view path;
    };
    const std::vector<Case> cases{
        {
            "empty-target",
            "",
            "1.0.0",
            "binding-member-target-empty",
            "/bindingMembers/",
        },
        {
            "empty-version",
            "tool",
            "",
            "binding-member-version-empty",
            "/bindingMembers/tool",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.name);
        nlohmann::json members = nlohmann::json::object();
        members[testCase.target] = testCase.version;
        nlohmann::json malformed{
            {"path", "/subject"},
            {"kind", "group"},
            {"bindingGroup", valid_binding_group_json()},
            {"bindingMembers", std::move(members)},
        };

        auto parsed = xlings::xvm::vdata_from_json(malformed);
        expect_single_binding_integrity_issue(
            parsed, testCase.code, testCase.path);
        expect_metadata_integrity_failure(
            parsed, testCase.code, testCase.path);
        EXPECT_TRUE(parsed.bindingMembers.empty());

        for (int cycle = 0; cycle < 3; ++cycle) {
            SCOPED_TRACE(cycle);
            parsed = reload_vdata(parsed);
            expect_single_binding_integrity_issue(
                parsed, testCase.code, testCase.path);
            expect_metadata_integrity_failure(
                parsed, testCase.code, testCase.path);
            EXPECT_TRUE(parsed.bindingMembers.empty());
        }
    }
}

TEST(XvmJsonTest, EmptyHeaderSourcePersistsWhileEmptyDestinationIsValid) {
    nlohmann::json malformed{
        {"path", "/subject"},
        {"kind", "group"},
        {"bindingGroup", valid_binding_group_json()},
        {
            "bindingHeaders",
            nlohmann::json::array({
                {
                    {"sourceDir", ""},
                    {"destinationPrefix", ""},
                },
            }),
        },
    };

    auto parsed = xlings::xvm::vdata_from_json(malformed);
    expect_single_binding_integrity_issue(
        parsed,
        "binding-header-source-dir-empty",
        "/bindingHeaders/0/sourceDir");
    expect_metadata_integrity_failure(
        parsed,
        "binding-header-source-dir-empty",
        "/bindingHeaders/0/sourceDir");
    EXPECT_TRUE(parsed.bindingHeaders.empty());

    for (int cycle = 0; cycle < 3; ++cycle) {
        SCOPED_TRACE(cycle);
        parsed = reload_vdata(parsed);
        expect_single_binding_integrity_issue(
            parsed,
            "binding-header-source-dir-empty",
            "/bindingHeaders/0/sourceDir");
        expect_metadata_integrity_failure(
            parsed,
            "binding-header-source-dir-empty",
            "/bindingHeaders/0/sourceDir");
        EXPECT_TRUE(parsed.bindingHeaders.empty());
    }

    malformed["bindingHeaders"][0]["sourceDir"] = "include";
    auto valid = xlings::xvm::vdata_from_json(malformed);
    EXPECT_TRUE(valid.bindingIntegrityIssues.empty());
    ASSERT_EQ(valid.bindingHeaders.size(), 1u);
    EXPECT_EQ(valid.bindingHeaders[0].sourceDir, "include");
    EXPECT_TRUE(valid.bindingHeaders[0].destinationPrefix.empty());
}

TEST(XvmJsonTest, PerVersionMetadataSupportsDifferentProvidersForOneTarget) {
    xlings::xvm::VersionDB original;
    original["tool"].type = "program";
    original["tool"].filename = "first-provider-tool";

    auto& first = original["tool"].versions["repo-a:1.0.0"];
    first.path = "/pkg/provider-a";
    first.kind = "program";
    first.sourceName = "tool-a";
    first.destinationName = "tool";
    first.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "repo-a:provider",
        .providerVersion = "1.0.0",
        .group = "provider-a",
        .rootTarget = "provider-a",
        .rootVersion = "repo-a:1.0.0",
    };

    auto& second = original["tool"].versions["repo-b:2.0.0"];
    second.path = "/pkg/provider-b";
    second.kind = "lib";
    second.sourceName = "libtool-b.so";
    second.destinationName = "libtool.so";
    second.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "repo-b:provider",
        .providerVersion = "2.0.0",
        .group = "provider-b",
        .rootTarget = "provider-b",
        .rootVersion = "repo-b:2.0.0",
    };

    auto restored =
        xlings::xvm::versions_from_json(xlings::xvm::versions_to_json(original));

    const auto& firstRestored =
        restored.at("tool").versions.at("repo-a:1.0.0");
    EXPECT_EQ(firstRestored.kind, "program");
    EXPECT_EQ(firstRestored.sourceName, "tool-a");
    EXPECT_EQ(firstRestored.destinationName, "tool");
    ASSERT_TRUE(firstRestored.bindingGroup.has_value());
    EXPECT_EQ(firstRestored.bindingGroup->provider, "repo-a:provider");

    const auto& secondRestored =
        restored.at("tool").versions.at("repo-b:2.0.0");
    EXPECT_EQ(secondRestored.kind, "lib");
    EXPECT_EQ(secondRestored.sourceName, "libtool-b.so");
    EXPECT_EQ(secondRestored.destinationName, "libtool.so");
    ASSERT_TRUE(secondRestored.bindingGroup.has_value());
    EXPECT_EQ(secondRestored.bindingGroup->provider, "repo-b:provider");

    EXPECT_EQ(restored.at("tool").type, "program");
    EXPECT_EQ(restored.at("tool").filename, "first-provider-tool");
}

TEST(XvmJsonTest, VInfoRoundTrip) {
    xlings::xvm::VInfo original;
    original.type = "program";
    original.filename = "gcc";
    original.versions["15.1.0"].path = "/usr/bin";
    original.versions["15.1.0"].alias = {"15"};
    original.versions["14.2.0"].path = "/opt/gcc14/bin";
    original.bindings["g++"]["15.1.0"] = "g++-15";
    original.bindings["g++"]["14.2.0"] = "g++-14";

    auto j = xlings::xvm::vinfo_to_json(original);
    auto restored = xlings::xvm::vinfo_from_json(j);

    EXPECT_EQ(restored.type, "program");
    EXPECT_EQ(restored.filename, "gcc");
    ASSERT_EQ(restored.versions.size(), 2u);
    EXPECT_EQ(restored.versions.at("15.1.0").path, "/usr/bin");
    EXPECT_EQ(restored.versions.at("15.1.0").alias.size(), 1u);
    EXPECT_EQ(restored.versions.at("14.2.0").path, "/opt/gcc14/bin");
    ASSERT_EQ(restored.bindings.size(), 1u);
    EXPECT_EQ(restored.bindings.at("g++").at("15.1.0"), "g++-15");
}

TEST(XvmJsonTest, VersionDBRoundTrip) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/usr/bin", "program", "gcc");
    xlings::xvm::add_version(db, "gcc", "14.2.0", "/opt/gcc14/bin", "program", "gcc");
    xlings::xvm::add_version(db, "node", "22.0.0", "/opt/node22/bin", "program", "node");

    auto j = xlings::xvm::versions_to_json(db);
    auto restored = xlings::xvm::versions_from_json(j);

    ASSERT_EQ(restored.size(), 2u);
    EXPECT_TRUE(restored.contains("gcc"));
    EXPECT_TRUE(restored.contains("node"));
    EXPECT_EQ(restored.at("gcc").versions.size(), 2u);
    EXPECT_EQ(restored.at("node").versions.size(), 1u);
    EXPECT_EQ(restored.at("gcc").type, "program");
}

TEST(XvmJsonTest, WorkspaceRoundTrip) {
    xlings::xvm::Workspace ws;
    ws["gcc"] = "15.1.0";
    ws["node"] = "22.0.0";

    auto j = xlings::xvm::workspace_to_json(ws);
    auto restored = xlings::xvm::workspace_from_json(j);

    ASSERT_EQ(restored.size(), 2u);
    EXPECT_EQ(restored.at("gcc"), "15.1.0");
    EXPECT_EQ(restored.at("node"), "22.0.0");
}

TEST(XvmJsonTest, FromJsonEmptyObject) {
    auto j = nlohmann::json::object();
    auto db = xlings::xvm::versions_from_json(j);
    EXPECT_TRUE(db.empty());

    auto ws = xlings::xvm::workspace_from_json(j);
    EXPECT_TRUE(ws.empty());
}

TEST(XvmJsonTest, FromJsonNonObject) {
    auto j = nlohmann::json::array();
    auto db = xlings::xvm::versions_from_json(j);
    EXPECT_TRUE(db.empty());

    auto ws = xlings::xvm::workspace_from_json(j);
    EXPECT_TRUE(ws.empty());
}

TEST(XvmJsonTest, WorkspacePlatformAwareManifestParsing) {
    auto j = nlohmann::json::parse(R"({
        "node": {
            "default": "22.17.1",
            "linux": "20.19.0",
            "windows": "22.18.0"
        },
        "python": {
            "default": "3.12.9"
        },
        "rust": {
            "windows": "1.86.0"
        }
    })");

    auto ws = xlings::xvm::workspace_from_json(j);

#if defined(__linux__)
    EXPECT_EQ(ws.at("node"), "20.19.0");
#elif defined(_WIN32)
    EXPECT_EQ(ws.at("node"), "22.18.0");
#else
    EXPECT_EQ(ws.at("node"), "22.17.1");
#endif

    EXPECT_EQ(ws.at("python"), "3.12.9");
#if defined(_WIN32)
    EXPECT_EQ(ws.at("rust"), "1.86.0");
#else
    EXPECT_TRUE(ws.find("rust") == ws.end());
#endif
}

TEST(XvmJsonTest, FullConfigJsonRoundTrip) {
    // Simulate a complete .xlings.json
    std::string configJson = R"({
        "lang": "en",
        "mirror": "GLOBAL",
        "activeSubos": "default",
        "versions": {
            "gcc": {
                "type": "program",
                "filename": "gcc",
                "versions": {
                    "15.1.0": { "path": "/usr/bin", "alias": ["15"] },
                    "14.2.0": { "path": "/opt/gcc14/bin" }
                },
                "bindings": {
                    "g++": { "15.1.0": "g++-15", "14.2.0": "g++-14" }
                }
            },
            "node": {
                "type": "program",
                "filename": "node",
                "versions": {
                    "22.0.0": { "path": "/opt/node22/bin", "envs": {"NODE_HOME": "/opt/node22"} }
                }
            }
        }
    })";

    auto json = nlohmann::json::parse(configJson);
    auto db = xlings::xvm::versions_from_json(json["versions"]);

    ASSERT_EQ(db.size(), 2u);

    // Check gcc
    auto* gcc = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc, nullptr);
    EXPECT_EQ(gcc->type, "program");
    EXPECT_EQ(gcc->filename, "gcc");
    ASSERT_EQ(gcc->versions.size(), 2u);
    EXPECT_EQ(gcc->versions.at("15.1.0").path, "/usr/bin");
    ASSERT_EQ(gcc->versions.at("15.1.0").alias.size(), 1u);
    EXPECT_EQ(gcc->versions.at("15.1.0").alias[0], "15");
    EXPECT_EQ(gcc->versions.at("14.2.0").path, "/opt/gcc14/bin");
    EXPECT_EQ(gcc->bindings.at("g++").at("15.1.0"), "g++-15");

    // Check node
    auto* node_vdata = xlings::xvm::get_vdata(db, "node", "22.0.0");
    ASSERT_NE(node_vdata, nullptr);
    EXPECT_EQ(node_vdata->path, "/opt/node22/bin");
    EXPECT_EQ(node_vdata->envs.at("NODE_HOME"), "/opt/node22");

    // Fuzzy match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "15"), "15.1.0");
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "14"), "14.2.0");

    // Serialize back and verify
    auto j2 = xlings::xvm::versions_to_json(db);
    auto db2 = xlings::xvm::versions_from_json(j2);
    EXPECT_EQ(db2.size(), db.size());
}

// ============================================================
// xvm VData new fields (includedir/libdir) tests
// ============================================================

TEST(XvmVDataFieldsTest, IncludedirLibdirConstruction) {
    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    vdata.includedir = "/opt/glibc/2.39/include";
    vdata.libdir = "/opt/glibc/2.39/lib64";

    EXPECT_EQ(vdata.includedir, "/opt/glibc/2.39/include");
    EXPECT_EQ(vdata.libdir, "/opt/glibc/2.39/lib64");
}

TEST(XvmVDataFieldsTest, IncludedirLibdirJsonRoundTrip) {
    xlings::xvm::VData original;
    original.path = "/opt/openssl/3.1.5";
    original.includedir = "/opt/openssl/3.1.5/include";
    original.libdir = "/opt/openssl/3.1.5/lib64";
    original.alias = {"3.1"};

    auto j = xlings::xvm::vdata_to_json(original);
    EXPECT_EQ(j["includedir"].get<std::string>(), "/opt/openssl/3.1.5/include");
    EXPECT_EQ(j["libdir"].get<std::string>(), "/opt/openssl/3.1.5/lib64");

    auto restored = xlings::xvm::vdata_from_json(j);
    EXPECT_EQ(restored.path, original.path);
    EXPECT_EQ(restored.includedir, original.includedir);
    EXPECT_EQ(restored.libdir, original.libdir);
    EXPECT_EQ(restored.alias, original.alias);
}

TEST(XvmVDataFieldsTest, EmptyIncludedirLibdirNotSerialized) {
    xlings::xvm::VData vdata;
    vdata.path = "/usr/bin";
    // includedir and libdir are empty

    auto j = xlings::xvm::vdata_to_json(vdata);
    EXPECT_FALSE(j.contains("includedir"));
    EXPECT_FALSE(j.contains("libdir"));

    auto restored = xlings::xvm::vdata_from_json(j);
    EXPECT_TRUE(restored.includedir.empty());
    EXPECT_TRUE(restored.libdir.empty());
}

TEST(XvmVDataFieldsTest, FullConfigWithNewFields) {
    std::string configJson = R"({
        "versions": {
            "glibc": {
                "type": "program",
                "versions": {
                    "2.39": {
                        "path": "/opt/glibc/2.39",
                        "includedir": "/opt/glibc/2.39/include",
                        "libdir": "/opt/glibc/2.39/lib64"
                    }
                }
            }
        }
    })";

    auto json = nlohmann::json::parse(configJson);
    auto db = xlings::xvm::versions_from_json(json["versions"]);

    auto* vdata = xlings::xvm::get_vdata(db, "glibc", "2.39");
    ASSERT_NE(vdata, nullptr);
    EXPECT_EQ(vdata->path, "/opt/glibc/2.39");
    EXPECT_EQ(vdata->includedir, "/opt/glibc/2.39/include");
    EXPECT_EQ(vdata->libdir, "/opt/glibc/2.39/lib64");

    // Round-trip
    auto j2 = xlings::xvm::versions_to_json(db);
    auto db2 = xlings::xvm::versions_from_json(j2);
    auto* vdata2 = xlings::xvm::get_vdata(db2, "glibc", "2.39");
    ASSERT_NE(vdata2, nullptr);
    EXPECT_EQ(vdata2->includedir, "/opt/glibc/2.39/include");
    EXPECT_EQ(vdata2->libdir, "/opt/glibc/2.39/lib64");
}

// ============================================================
// xvm "latest" version resolution tests
// ============================================================

TEST(XvmDbTest, MatchLatestPicksHighest) {
    // "latest" isn't handled by match_version — it's handled in cmd_use.
    // But we can verify the underlying sort logic by checking match_version
    // with empty prefix returns nothing (since "" doesn't prefix-match digits),
    // confirming that "latest" needs special handling.
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "tool", "0.1.3", "/a");
    xlings::xvm::add_version(db, "tool", "0.1.4", "/b");
    xlings::xvm::add_version(db, "tool", "1.0.0", "/c");

    // "latest" should not match any version via fuzzy match
    EXPECT_EQ(xlings::xvm::match_version(db, "tool", "latest"), "");

    // Verify get_all_versions returns all, so cmd_use can sort and pick highest
    auto all = xlings::xvm::get_all_versions(db, "tool");
    EXPECT_EQ(all.size(), 3u);
}

TEST(XvmDbTest, NamespacedVersionMatch) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "gcc", "xim:15.1.0", "/a");
    xlings::xvm::add_version(db, "gcc", "xim:14.2.0", "/b");
    xlings::xvm::add_version(db, "gcc", "13.3.0", "/c");

    // Namespace-qualified match
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "xim:15"), "xim:15.1.0");
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "xim:14"), "xim:14.2.0");

    // Bare prefix prefers bare versions
    EXPECT_EQ(xlings::xvm::match_version(db, "gcc", "13"), "13.3.0");
}

// ============================================================
// xvm binding tree tests
// ============================================================

namespace {

xlings::xvm::BindingGroupRef make_binding_group_ref(
    std::string provider,
    std::string providerVersion,
    std::string group,
    std::string rootTarget,
    std::string rootVersion) {
    return {
        .provider = std::move(provider),
        .providerVersion = std::move(providerVersion),
        .group = std::move(group),
        .rootTarget = std::move(rootTarget),
        .rootVersion = std::move(rootVersion),
    };
}

xlings::xvm::VData& add_provider_group_member(
    xlings::xvm::VersionDB& db,
    const std::string& target,
    const std::string& version,
    const xlings::xvm::BindingGroupRef& group,
    const std::string& kind,
    const std::string& sourceName = "",
    const std::string& destinationName = "") {
    auto& data = db[target].versions[version];
    data.path = "/pkg/" + group.providerVersion;
    data.kind = kind;
    data.sourceName = sourceName;
    data.destinationName = destinationName;
    data.bindingGroup = group;
    return data;
}

void expect_binding_error(
    const std::expected<xlings::xvm::BindingSelection,
                        xlings::xvm::BindingError>& result,
    xlings::xvm::BindingErrorKind kind,
    std::string_view target,
    std::string_view version) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, kind);
    EXPECT_EQ(result.error().target, target);
    EXPECT_EQ(result.error().version, version);
    EXPECT_FALSE(result.error().message.empty());
}

struct BindingGroupIdentityFieldCase {
    std::string_view path;
    std::string xlings::xvm::BindingGroupRef::* member;
};

const std::array<BindingGroupIdentityFieldCase, 5>
    binding_group_identity_fields{
        BindingGroupIdentityFieldCase{
            "/bindingGroup/provider",
            &xlings::xvm::BindingGroupRef::provider,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/version",
            &xlings::xvm::BindingGroupRef::providerVersion,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/group",
            &xlings::xvm::BindingGroupRef::group,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/rootTarget",
            &xlings::xvm::BindingGroupRef::rootTarget,
        },
        BindingGroupIdentityFieldCase{
            "/bindingGroup/rootVersion",
            &xlings::xvm::BindingGroupRef::rootVersion,
        },
    };

void expect_binding_metadata_error(
    const std::expected<xlings::xvm::BindingSelection,
                        xlings::xvm::BindingError>& result,
    std::string_view target,
    std::string_view version,
    std::string_view code,
    std::string_view path) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind,
              xlings::xvm::BindingErrorKind::MetadataIntegrityIssue);
    EXPECT_EQ(result.error().target, target);
    EXPECT_EQ(result.error().version, version);
    EXPECT_NE(result.error().message.find(code), std::string::npos);
    EXPECT_NE(result.error().message.find(path), std::string::npos);
}

}  // namespace

TEST(XvmRegistrationTest,
     RegistersChildBeforeRootWithNamespacedTransformedVersions) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const xlings::xvm::RegistrationBatch batch{
        .provider = "xim:gcc",
        .providerVersion = "15.1.0",
        .nodes = {
            {
                .target = "gcc-ar",
                .version = "xim:gcc-15.1.0",
                .path = "/pkg/gcc/15.1.0/bin",
                .kind = "program",
                .sourceName = "gcc-ar-15",
                .destinationName = "gcc-ar",
                .binding = xlings::xvm::RegistrationBinding{
                    .rootTarget = "xim-gnu-gcc",
                    .rootVersion = "xim:15.1.0",
                },
            },
            {
                .target = "xim-gnu-gcc",
                .version = "xim:15.1.0",
                .path = "/pkg/gcc/15.1.0",
                .kind = "group",
            },
        },
    };

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    const auto& root =
        db.at("xim-gnu-gcc").versions.at("xim:15.1.0");
    const auto& child =
        db.at("gcc-ar").versions.at("xim:gcc-15.1.0");
    ASSERT_TRUE(root.bindingGroup.has_value());
    ASSERT_TRUE(child.bindingGroup.has_value());
    EXPECT_EQ(root.bindingGroup->group, "xim-gnu-gcc");
    EXPECT_EQ(root.bindingGroup->rootTarget, "xim-gnu-gcc");
    EXPECT_EQ(root.bindingGroup->rootVersion, "xim:15.1.0");
    EXPECT_EQ(child.bindingGroup->provider, root.bindingGroup->provider);
    EXPECT_EQ(
        child.bindingGroup->providerVersion,
        root.bindingGroup->providerVersion);
    EXPECT_EQ(child.bindingGroup->group, root.bindingGroup->group);
    EXPECT_EQ(
        child.bindingGroup->rootTarget,
        root.bindingGroup->rootTarget);
    EXPECT_EQ(
        child.bindingGroup->rootVersion,
        root.bindingGroup->rootVersion);
    EXPECT_EQ(
        root.bindingMembers,
        (std::map<std::string, std::string>{
            {"gcc-ar", "xim:gcc-15.1.0"},
            {"xim-gnu-gcc", "xim:15.1.0"},
        }));
    EXPECT_EQ(
        db.at("xim-gnu-gcc")
            .bindings.at("gcc-ar").at("xim:15.1.0"),
        "xim:gcc-15.1.0");
    EXPECT_EQ(
        db.at("gcc-ar")
            .bindings.at("xim-gnu-gcc").at("xim:gcc-15.1.0"),
        "xim:15.1.0");
    EXPECT_EQ(workspace.at("gcc-ar"), "xim:gcc-15.1.0");
    EXPECT_EQ(workspace.at("xim-gnu-gcc"), "xim:15.1.0");
    EXPECT_EQ(
        installed.at("gcc-ar"),
        (std::vector<std::string>{"xim:gcc-15.1.0"}));
    EXPECT_EQ(
        installed.at("xim-gnu-gcc"),
        (std::vector<std::string>{"xim:15.1.0"}));
}

namespace {

xlings::xvm::RegistrationNode make_registration_node(
    std::string target,
    std::string version,
    std::string kind = "program") {
    return {
        .target = std::move(target),
        .version = std::move(version),
        .path = "/pkg/provider/1.0.0",
        .kind = std::move(kind),
        .sourceName = "payload",
        .destinationName = "tool",
    };
}

xlings::xvm::RegistrationBinding make_registration_binding(
    std::string rootTarget,
    std::string rootVersion,
    std::string group = {}) {
    return {
        .rootTarget = std::move(rootTarget),
        .rootVersion = std::move(rootVersion),
        .group = std::move(group),
    };
}

xlings::xvm::RegistrationBatch make_registration_batch(
    std::vector<xlings::xvm::RegistrationNode> nodes) {
    return {
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .nodes = std::move(nodes),
    };
}

void expect_registration_error(
    const std::expected<
        std::vector<xlings::xvm::RegisteredMember>,
        xlings::xvm::RegistrationError>& result,
    xlings::xvm::RegistrationErrorKind kind,
    std::string_view path,
    std::string_view target = {},
    std::string_view version = {}) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, kind);
    EXPECT_EQ(result.error().path, path);
    EXPECT_EQ(result.error().target, target);
    EXPECT_EQ(result.error().version, version);
    EXPECT_FALSE(result.error().message.empty());
}

void expect_registration_state_unchanged(
    const xlings::xvm::VersionDB& db,
    const xlings::xvm::Workspace& workspace,
    const xlings::xvm::WorkspaceInstalled& installed,
    const nlohmann::json& dbBefore,
    const xlings::xvm::Workspace& workspaceBefore,
    const xlings::xvm::WorkspaceInstalled& installedBefore) {
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

}  // namespace

TEST(XvmRegistrationTest, RootFirstAndRootLastSerializeIdentically) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto child = make_registration_node("tool", "repo:tool-1.0.0");
    child.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");

    xlings::xvm::VersionDB rootFirstDb;
    xlings::xvm::Workspace rootFirstWorkspace;
    xlings::xvm::WorkspaceInstalled rootFirstInstalled;
    auto rootFirst = xlings::xvm::apply_registration_batch(
        rootFirstDb, rootFirstWorkspace, rootFirstInstalled,
        make_registration_batch({root, child}));
    ASSERT_TRUE(rootFirst.has_value()) << rootFirst.error().message;

    xlings::xvm::VersionDB rootLastDb;
    xlings::xvm::Workspace rootLastWorkspace;
    xlings::xvm::WorkspaceInstalled rootLastInstalled;
    auto rootLast = xlings::xvm::apply_registration_batch(
        rootLastDb, rootLastWorkspace, rootLastInstalled,
        make_registration_batch({child, root}));
    ASSERT_TRUE(rootLast.has_value()) << rootLast.error().message;

    EXPECT_EQ(
        xlings::xvm::versions_to_json(rootFirstDb),
        xlings::xvm::versions_to_json(rootLastDb));
    EXPECT_EQ(rootFirstWorkspace, rootLastWorkspace);
    EXPECT_EQ(rootFirstInstalled, rootLastInstalled);
}

TEST(XvmRegistrationErrorTest, RejectsPhantomRootWithoutMutation) {
    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    auto child = make_registration_node("tool", "repo:1.0.0");
    child.binding =
        make_registration_binding("missing-root", "repo:1.0.0");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, make_registration_batch({child}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::RootNotInBatch,
        "/nodes/0/binding", "missing-root", "repo:1.0.0");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationErrorTest, RejectsSelfBindingWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto node = make_registration_node("root", "repo:1.0.0", "group");
    node.sourceName.clear();
    node.destinationName.clear();
    node.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, make_registration_batch({node}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::SelfBinding,
        "/nodes/0/binding", "root", "repo:1.0.0");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsDuplicateExactNodeWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto node = make_registration_node("tool", "repo:1.0.0");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({node, node}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::DuplicateNode,
        "/nodes/1", "tool", "repo:1.0.0");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest,
     RejectsTwoVersionsOfOneTargetInOneGroupWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto first = make_registration_node("tool", "repo:tool-1.0.0");
    first.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    auto second = make_registration_node("tool", "repo:tool-alt-1.0.0");
    second.binding = first.binding;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, first, second}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::TargetVersionConflict,
        "/nodes/2", "tool", "repo:tool-alt-1.0.0");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsOneGroupLabelWithTwoRoots) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto childA = make_registration_node("tool-a", "repo:a");
    childA.binding =
        make_registration_binding("root-a", "repo:a", "shared");
    auto childB = make_registration_node("tool-b", "repo:b");
    childB.binding =
        make_registration_binding("root-b", "repo:b", "shared");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({rootA, childA, rootB, childB}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/nodes/3/binding/group", "tool-b", "repo:b");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsOneRootAssignedToTwoGroups) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto childA = make_registration_node("tool-a", "repo:a");
    childA.binding =
        make_registration_binding("root", "repo:1.0.0", "group-a");
    auto childB = make_registration_node("tool-b", "repo:b");
    childB.binding =
        make_registration_binding("root", "repo:1.0.0", "group-b");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, childA, childB}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/nodes/2/binding/group", "tool-b", "repo:b");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationErrorTest, RejectsInvalidBatchNodeAndBindingIdentity) {
    struct Case {
        xlings::xvm::RegistrationBatch batch;
        xlings::xvm::RegistrationErrorKind kind;
        std::string path;
        std::string target;
        std::string version;
    };

    auto validNode = make_registration_node("tool", "repo:1.0.0");
    auto emptyProvider = make_registration_batch({validNode});
    emptyProvider.provider.clear();
    auto emptyRelease = make_registration_batch({validNode});
    emptyRelease.providerVersion.clear();
    auto emptyTargetNode = validNode;
    emptyTargetNode.target.clear();
    auto emptyVersionNode = validNode;
    emptyVersionNode.version.clear();
    auto emptyRootNode = validNode;
    emptyRootNode.binding =
        make_registration_binding("", "repo:1.0.0");
    auto emptyRootVersionNode = validNode;
    emptyRootVersionNode.binding =
        make_registration_binding("root", "");

    const std::vector<Case> cases{
        {
            std::move(emptyProvider),
            xlings::xvm::RegistrationErrorKind::InvalidBatchIdentity,
            "/provider", "", "",
        },
        {
            std::move(emptyRelease),
            xlings::xvm::RegistrationErrorKind::InvalidBatchIdentity,
            "/providerVersion", "", "",
        },
        {
            make_registration_batch({emptyTargetNode}),
            xlings::xvm::RegistrationErrorKind::InvalidNodeIdentity,
            "/nodes/0/target", "", "repo:1.0.0",
        },
        {
            make_registration_batch({emptyVersionNode}),
            xlings::xvm::RegistrationErrorKind::InvalidNodeIdentity,
            "/nodes/0/version", "tool", "",
        },
        {
            make_registration_batch({emptyRootNode}),
            xlings::xvm::RegistrationErrorKind::InvalidBindingIdentity,
            "/nodes/0/binding/rootTarget", "tool", "repo:1.0.0",
        },
        {
            make_registration_batch({emptyRootVersionNode}),
            xlings::xvm::RegistrationErrorKind::InvalidBindingIdentity,
            "/nodes/0/binding/rootVersion", "tool", "repo:1.0.0",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        xlings::xvm::Workspace workspace;
        xlings::xvm::WorkspaceInstalled installed;

        auto result = xlings::xvm::apply_registration_batch(
            db, workspace, installed, testCase.batch);

        expect_registration_error(
            result, testCase.kind, testCase.path,
            testCase.target, testCase.version);
        EXPECT_TRUE(db.empty());
        EXPECT_TRUE(workspace.empty());
        EXPECT_TRUE(installed.empty());
    }
}

TEST(XvmRegistrationErrorTest,
     RejectsInvalidNodePayloadAtIndexedPathWithoutMutation) {
    struct Case {
        xlings::xvm::RegistrationNode node;
        std::string path;
    };

    auto unsupported =
        make_registration_node("archive", "repo:archive", "archive");
    auto emptyProgramSource =
        make_registration_node("program", "repo:program", "program");
    emptyProgramSource.sourceName.clear();
    auto emptyProgramDestination =
        make_registration_node("program", "repo:program", "program");
    emptyProgramDestination.destinationName.clear();
    auto emptyLibrarySource =
        make_registration_node("library", "repo:library", "lib");
    emptyLibrarySource.sourceName.clear();
    auto emptyLibraryDestination =
        make_registration_node("library", "repo:library", "lib");
    emptyLibraryDestination.destinationName.clear();

    const std::vector<Case> cases{
        {
            std::move(unsupported),
            "/nodes/0/kind",
        },
        {
            std::move(emptyProgramSource),
            "/nodes/0/sourceName",
        },
        {
            std::move(emptyProgramDestination),
            "/nodes/0/destinationName",
        },
        {
            std::move(emptyLibrarySource),
            "/nodes/0/sourceName",
        },
        {
            std::move(emptyLibraryDestination),
            "/nodes/0/destinationName",
        },
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        db["sentinel"].versions["0"].path = "/sentinel";
        xlings::xvm::Workspace workspace{{"sentinel", "0"}};
        xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
        const auto dbBefore = xlings::xvm::versions_to_json(db);
        const auto workspaceBefore = workspace;
        const auto installedBefore = installed;

        auto result = xlings::xvm::apply_registration_batch(
            db, workspace, installed,
            make_registration_batch({testCase.node}));

        expect_registration_error(
            result,
            xlings::xvm::RegistrationErrorKind::InvalidNodePayload,
            testCase.path, testCase.node.target, testCase.node.version);
        expect_registration_state_unchanged(
            db, workspace, installed,
            dbBefore, workspaceBefore, installedBefore);
    }
}

TEST(XvmRegistrationOwnershipTest,
     RejectsPersistedGroupLabelWithDifferentRootWithoutMutation) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef existingGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "old-root",
        .rootVersion = "repo:old-root",
    };
    auto& oldRoot = db["old-root"].versions["repo:old-root"];
    oldRoot.path = "/pkg/provider/old";
    oldRoot.kind = "group";
    oldRoot.bindingGroup = existingGroup;
    oldRoot.bindingMembers = {
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;

    auto newRoot =
        make_registration_node("new-root", "repo:new-root", "group");
    auto newTool =
        make_registration_node("new-tool", "repo:new-tool");
    newTool.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({newRoot, newTool}));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/nodes/1/binding/group", "new-tool", "repo:new-tool");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsPersistedGroupLabelWithDuplicateRootsWithoutMutation) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef oldGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "old-root",
        .rootVersion = "repo:old-root",
    };
    auto& oldRoot = db["old-root"].versions["repo:old-root"];
    oldRoot.path = "/pkg/provider/old";
    oldRoot.kind = "group";
    oldRoot.bindingGroup = oldGroup;
    oldRoot.bindingMembers = {
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;

    const xlings::xvm::BindingGroupRef otherGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "other-root",
        .rootVersion = "repo:other-root",
    };
    auto& otherRoot = db["other-root"].versions["repo:other-root"];
    otherRoot.path = "/pkg/provider/other";
    otherRoot.kind = "group";
    otherRoot.bindingGroup = otherGroup;
    otherRoot.bindingMembers = {
        {"other-root", "repo:other-root"},
    };
    otherRoot.bindingMembersDeclared = true;

    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("fresh", "repo:fresh"),
        }));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::GroupConflict,
        "/bindingGroups/shared", "other-root", "repo:other-root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     MissingPersistedRootStillRequiresEveryOwnedMemberWithoutMutation) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef existingGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "shared",
        .rootTarget = "root",
        .rootVersion = "repo:root",
    };
    auto& oldMember = db["old-member"].versions["repo:old-member"];
    oldMember.path = "/pkg/provider/old";
    oldMember.kind = "program";
    oldMember.sourceName = "old-member";
    oldMember.destinationName = "old-member";
    oldMember.bindingGroup = existingGroup;

    auto root = make_registration_node("root", "repo:root", "group");
    auto newMember =
        make_registration_node("new-member", "repo:new-member");
    newMember.binding =
        make_registration_binding("root", "repo:root", "shared");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, newMember}));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteOwnedGroup,
        "/nodes/1/binding/group",
        "old-member", "repo:old-member");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     MissingPersistedRootRejectsDifferentIncomingGroupWithoutMutation) {
    xlings::xvm::VersionDB db;
    const auto oldGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "old-group",
        "shared-root", "repo:root");
    add_provider_group_member(
        db, "old-member", "repo:old-member", oldGroup,
        "program", "old-member", "old-member");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto root =
        make_registration_node("shared-root", "repo:root", "group");
    auto newMember =
        make_registration_node("new-member", "repo:new-member");
    newMember.binding = make_registration_binding(
        "shared-root", "repo:root", "new-group");

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root, newMember}));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/bindingGroups/old-group", "shared-root", "repo:root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsPersistedTwoLabelsForOneRootWithoutMutation) {
    xlings::xvm::VersionDB db;
    const auto alphaGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "alpha",
        "shared-root", "repo:root");
    auto& root = add_provider_group_member(
        db, "shared-root", "repo:root", alphaGroup, "group");
    root.bindingMembers = {
        {"shared-root", "repo:root"},
    };
    root.bindingMembersDeclared = true;
    const auto betaGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "beta",
        "shared-root", "repo:root");
    add_provider_group_member(
        db, "beta-member", "repo:beta", betaGroup,
        "program", "beta-member", "beta-member");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("fresh", "repo:fresh"),
        }));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/bindingGroups/beta", "shared-root", "repo:root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     CompleteSameOwnerRootMigrationIsAtomicAndOrderIndependent) {
    const auto oldGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "shared",
        "old-root", "repo:old-root");
    xlings::xvm::VersionDB initialDb;
    auto& oldRoot = add_provider_group_member(
        initialDb, "old-root", "repo:old-root", oldGroup, "group");
    oldRoot.bindingMembers = {
        {"old-a", "repo:old-a"},
        {"old-b", "repo:old-b"},
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;
    oldRoot.bindingHeaders = {
        {
            .sourceDir = "/pkg/old/include",
            .destinationPrefix = "old",
        },
    };
    oldRoot.bindingHeadersDeclared = true;
    add_provider_group_member(
        initialDb, "old-a", "repo:old-a", oldGroup,
        "program", "old-a", "old-a");
    add_provider_group_member(
        initialDb, "old-b", "repo:old-b", oldGroup,
        "program", "old-b", "old-b");
    initialDb["old-root"].bindings["old-a"]["repo:old-root"] =
        "repo:old-a";
    initialDb["old-a"].bindings["old-root"]["repo:old-a"] =
        "repo:old-root";
    initialDb["old-root"].bindings["old-b"]["repo:old-root"] =
        "repo:old-b";
    initialDb["old-b"].bindings["old-root"]["repo:old-b"] =
        "repo:old-root";
    const xlings::xvm::Workspace initialWorkspace{
        {"old-a", "repo:old-a"},
        {"old-b", "repo:old-b"},
        {"old-root", "repo:old-root"},
    };
    const xlings::xvm::WorkspaceInstalled initialInstalled{
        {"old-a", {"repo:old-a"}},
        {"old-b", {"repo:old-b"}},
        {"old-root", {"repo:old-root"}},
    };

    auto newRoot =
        make_registration_node("new-root", "repo:new-root", "group");
    auto migratedOldRoot =
        make_registration_node("old-root", "repo:old-root", "group");
    migratedOldRoot.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto migratedA =
        make_registration_node("old-a", "repo:old-a");
    migratedA.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto migratedB =
        make_registration_node("old-b", "repo:old-b");
    migratedB.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto forwardBatch = make_registration_batch({
        migratedA,
        newRoot,
        migratedOldRoot,
        migratedB,
    });
    auto reverseBatch = forwardBatch;
    std::ranges::reverse(reverseBatch.nodes);

    auto forwardDb = initialDb;
    auto forwardWorkspace = initialWorkspace;
    auto forwardInstalled = initialInstalled;
    auto forward = xlings::xvm::apply_registration_batch(
        forwardDb, forwardWorkspace, forwardInstalled, forwardBatch);
    auto reverseDb = initialDb;
    auto reverseWorkspace = initialWorkspace;
    auto reverseInstalled = initialInstalled;
    auto reverse = xlings::xvm::apply_registration_batch(
        reverseDb, reverseWorkspace, reverseInstalled, reverseBatch);

    ASSERT_TRUE(forward.has_value()) << forward.error().message;
    ASSERT_TRUE(reverse.has_value()) << reverse.error().message;
    EXPECT_EQ(
        xlings::xvm::versions_to_json(forwardDb),
        xlings::xvm::versions_to_json(reverseDb));
    EXPECT_EQ(forwardWorkspace, reverseWorkspace);
    EXPECT_EQ(forwardInstalled, reverseInstalled);

    const std::map<std::string, std::string> expectedMembers{
        {"new-root", "repo:new-root"},
        {"old-a", "repo:old-a"},
        {"old-b", "repo:old-b"},
        {"old-root", "repo:old-root"},
    };
    const auto& migratedRoot =
        forwardDb.at("new-root").versions.at("repo:new-root");
    EXPECT_EQ(migratedRoot.bindingMembers, expectedMembers);
    EXPECT_TRUE(migratedRoot.bindingHeaders.empty());

    for (const auto& [target, version] : expectedMembers) {
        const auto& member = forwardDb.at(target).versions.at(version);
        ASSERT_TRUE(member.bindingGroup.has_value());
        EXPECT_EQ(member.bindingGroup->group, "shared");
        EXPECT_EQ(member.bindingGroup->rootTarget, "new-root");
        EXPECT_EQ(member.bindingGroup->rootVersion, "repo:new-root");
    }
    std::size_t sharedRefCount = 0;
    std::set<std::pair<std::string, std::string>> sharedRoots;
    for (const auto& [_, info] : forwardDb) {
        for (const auto& versionEntry : info.versions) {
            const auto& data = versionEntry.second;
            if (!data.bindingGroup
                || data.bindingGroup->provider != "repo:provider"
                || data.bindingGroup->providerVersion != "1.0.0"
                || data.bindingGroup->group != "shared") {
                continue;
            }
            ++sharedRefCount;
            sharedRoots.emplace(
                data.bindingGroup->rootTarget,
                data.bindingGroup->rootVersion);
        }
    }
    EXPECT_EQ(sharedRefCount, expectedMembers.size());
    EXPECT_EQ(
        sharedRoots,
        (std::set<std::pair<std::string, std::string>>{
            {"new-root", "repo:new-root"},
        }));

    const auto& migratedOldRootData =
        forwardDb.at("old-root").versions.at("repo:old-root");
    EXPECT_TRUE(migratedOldRootData.bindingMembers.empty());
    EXPECT_FALSE(migratedOldRootData.bindingMembersDeclared);
    EXPECT_TRUE(migratedOldRootData.bindingHeaders.empty());
    EXPECT_FALSE(migratedOldRootData.bindingHeadersDeclared);
    EXPECT_FALSE(forwardDb.at("old-root").bindings.contains("old-a"));
    EXPECT_FALSE(forwardDb.at("old-root").bindings.contains("old-b"));
    EXPECT_FALSE(forwardDb.at("old-a").bindings.contains("old-root"));
    EXPECT_FALSE(forwardDb.at("old-b").bindings.contains("old-root"));
    EXPECT_EQ(
        forwardDb.at("old-root")
            .bindings.at("new-root").at("repo:old-root"),
        "repo:new-root");
}

TEST(XvmRegistrationOwnershipTest,
     RootMigrationRejectsOmittedManifestOnlyMemberWithoutMutation) {
    const auto oldGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "shared",
        "old-root", "repo:old-root");
    xlings::xvm::VersionDB db;
    auto& oldRoot = add_provider_group_member(
        db, "old-root", "repo:old-root", oldGroup, "group");
    oldRoot.bindingMembers = {
        {"manifest-only", "repo:manifest-only"},
        {"old-member", "repo:old-member"},
        {"old-root", "repo:old-root"},
    };
    oldRoot.bindingMembersDeclared = true;
    add_provider_group_member(
        db, "old-member", "repo:old-member", oldGroup,
        "program", "old-member", "old-member");
    auto& manifestOnly =
        db["manifest-only"].versions["repo:manifest-only"];
    manifestOnly.path = "/pkg/provider/1.0.0";
    manifestOnly.kind = "program";
    manifestOnly.sourceName = "manifest-only";
    manifestOnly.destinationName = "manifest-only";

    auto newRoot =
        make_registration_node("new-root", "repo:new-root", "group");
    auto migratedOldRoot =
        make_registration_node("old-root", "repo:old-root", "group");
    migratedOldRoot.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    auto migratedMember =
        make_registration_node("old-member", "repo:old-member");
    migratedMember.binding = make_registration_binding(
        "new-root", "repo:new-root", "shared");
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            migratedOldRoot,
            migratedMember,
            newRoot,
        }));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteOwnedGroup,
        "/nodes/0", "manifest-only", "repo:manifest-only");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsOtherProviderExactCollisionWithoutMutation) {
    xlings::xvm::VersionDB db;
    auto& existing = db["tool"].versions["repo:1.0.0"];
    existing.path = "/other/tool";
    existing.kind = "program";
    existing.sourceName = "other-tool";
    existing.destinationName = "tool";
    existing.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "other:provider",
        .providerVersion = "1.0.0",
        .group = "tool",
        .rootTarget = "tool",
        .rootVersion = "repo:1.0.0",
    };
    existing.bindingMembers = {{"tool", "repo:1.0.0"}};
    existing.bindingMembersDeclared = true;
    xlings::xvm::Workspace workspace{{"tool", "repo:1.0.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"tool", {"repo:1.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("tool", "repo:1.0.0"),
        }));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::OwnershipConflict,
        "/nodes/0", "tool", "repo:1.0.0");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsSameProviderDifferentReleaseCollisionWithoutMutation) {
    xlings::xvm::VersionDB db;
    auto& existing = db["tool"].versions["repo:1.0.0"];
    existing.path = "/provider/old";
    existing.kind = "program";
    existing.sourceName = "old-tool";
    existing.destinationName = "tool";
    existing.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "repo:provider",
        .providerVersion = "0.9.0",
        .group = "tool",
        .rootTarget = "tool",
        .rootVersion = "repo:1.0.0",
    };
    existing.bindingMembers = {{"tool", "repo:1.0.0"}};
    existing.bindingMembersDeclared = true;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const auto dbBefore = xlings::xvm::versions_to_json(db);

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("tool", "repo:1.0.0"),
        }));

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::OwnershipConflict,
        "/nodes/0", "tool", "repo:1.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationOwnershipTest, SameOwnerReregistrationIsIdempotent) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto child = make_registration_node("tool", "repo:tool-1.0.0");
    child.sourceName = "tool-real";
    child.destinationName = "tool";
    child.alias = {"--driver-mode=gcc"};
    child.envs = {{"TOOLCHAIN_ROOT", "/pkg/provider/1.0.0"}};
    child.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    const auto batch = make_registration_batch({child, root});

    auto first = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    const auto dbAfterFirst = xlings::xvm::versions_to_json(db);
    const auto workspaceAfterFirst = workspace;
    const auto installedAfterFirst = installed;

    auto second = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbAfterFirst);
    EXPECT_EQ(workspace, workspaceAfterFirst);
    EXPECT_EQ(installed, installedAfterFirst);
    EXPECT_EQ(
        db.at("root").bindings.at("tool").size(),
        1u);
    EXPECT_EQ(
        db.at("tool").bindings.at("root").size(),
        1u);
}

namespace {

void seed_complete_legacy_registration_group(
    xlings::xvm::VersionDB& db) {
    auto& rootInfo = db["legacy-root"];
    rootInfo.type = "program";
    rootInfo.filename = "legacy-root-real";
    auto& root = rootInfo.versions["repo:1.0.0"];
    root.path = "/pkg/provider/1.0.0";
    root.alias = {"root-alias"};
    root.envs = {{"ROOT_ENV", "root"}};

    auto& childInfo = db["tool"];
    childInfo.type = "program";
    childInfo.filename = "tool-real";
    auto& child = childInfo.versions["repo:tool-1.0.0"];
    child.path = "/pkg/provider/1.0.0";
    child.alias = {"tool-alias"};
    child.envs = {{"TOOL_ENV", "tool"}};

    rootInfo.bindings["tool"]["repo:1.0.0"] =
        "repo:tool-1.0.0";
    childInfo.bindings["legacy-root"]["repo:tool-1.0.0"] =
        "repo:1.0.0";
}

xlings::xvm::RegistrationBatch complete_legacy_adoption_batch() {
    auto root =
        make_registration_node("legacy-root", "repo:1.0.0");
    root.sourceName = "legacy-root-real";
    root.destinationName = "legacy-root";
    root.alias = {"root-alias"};
    root.envs = {{"ROOT_ENV", "root"}};
    auto child =
        make_registration_node("tool", "repo:tool-1.0.0");
    child.sourceName = "tool-real";
    child.destinationName = "tool";
    child.alias = {"tool-alias"};
    child.envs = {{"TOOL_ENV", "tool"}};
    child.binding = make_registration_binding(
        "legacy-root", "repo:1.0.0");
    return make_registration_batch({child, root});
}

}  // namespace

TEST(XvmRegistrationOwnershipTest, AdoptsCompatibleCompleteLegacyGroup) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, complete_legacy_adoption_batch());

    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto selection = xlings::xvm::resolve_binding_selection(
        db, "tool", "repo:tool-1.0.0");
    ASSERT_TRUE(selection.has_value()) << selection.error().message;
    EXPECT_EQ(
        selection->members,
        (std::map<std::string, std::string>{
            {"legacy-root", "repo:1.0.0"},
            {"tool", "repo:tool-1.0.0"},
        }));
    EXPECT_EQ(
        db.at("tool")
            .versions.at("repo:tool-1.0.0")
            .bindingGroup->provider,
        "repo:provider");
}

TEST(XvmRegistrationOwnershipTest,
     AdoptsOwnerlessLegacyGroupWithNormalizedVirtualPayload) {
    xlings::xvm::VersionDB db;
    auto& info = db["virtual"];
    info.type = "group";
    auto& legacy = info.versions["repo:virtual"];
    legacy.path = "/pkg/provider/1.0.0";
    legacy.kind = "group";
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto group =
        make_registration_node("virtual", "repo:virtual", "group");
    group.sourceName = "ignored-payload";
    group.destinationName = "ignored-shim";

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({group}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& adopted = db.at("virtual").versions.at("repo:virtual");
    EXPECT_TRUE(adopted.sourceName.empty());
    EXPECT_TRUE(adopted.destinationName.empty());
    ASSERT_TRUE(adopted.bindingGroup.has_value());
    EXPECT_EQ(adopted.bindingGroup->group, "virtual");
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncompatibleLegacyPayloadWithoutMutation) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace{{"tool", "repo:tool-1.0.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"tool", {"repo:tool-1.0.0"}},
    };
    auto batch = complete_legacy_adoption_batch();
    batch.nodes[0].path = "/different/payload";
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::LegacyPayloadMismatch,
        "/nodes/0/path", "tool", "repo:tool-1.0.0");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncompleteLegacyComponentWithoutMutation) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto batch = complete_legacy_adoption_batch();
    batch.nodes.erase(batch.nodes.begin());
    const auto dbBefore = xlings::xvm::versions_to_json(db);

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteLegacyComponent,
        "/nodes/0", "tool", "repo:tool-1.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncomingOnlyLegacyEdgeWithoutMutation) {
    xlings::xvm::VersionDB db;
    auto& toolInfo = db["tool"];
    toolInfo.type = "program";
    toolInfo.filename = "payload";
    auto& tool = toolInfo.versions["repo:tool"];
    tool.path = "/pkg/provider/1.0.0";

    auto& peerInfo = db["peer"];
    peerInfo.type = "program";
    peerInfo.filename = "peer";
    peerInfo.versions["repo:peer"].path = "/pkg/peer";
    peerInfo.bindings["tool"]["repo:peer"] = "repo:tool";

    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({
            make_registration_node("tool", "repo:tool"),
        }));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/nodes/0", "peer", "repo:peer");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationOwnershipTest,
     RejectsIncompleteSameOwnerGroupReconfiguration) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef group{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "toolchain",
        .rootTarget = "root",
        .rootVersion = "repo:1.0.0",
    };
    auto& root = db["root"].versions["repo:1.0.0"];
    root.path = "/pkg/provider/1.0.0";
    root.kind = "group";
    root.bindingGroup = group;
    root.bindingMembers = {
        {"root", "repo:1.0.0"},
        {"tool", "repo:tool-1.0.0"},
    };
    root.bindingMembersDeclared = true;
    auto& child = db["tool"].versions["repo:tool-1.0.0"];
    child.path = "/pkg/provider/1.0.0";
    child.kind = "program";
    child.sourceName = "tool-real";
    child.destinationName = "tool";
    child.bindingGroup = group;
    db["root"].bindings["tool"]["repo:1.0.0"] =
        "repo:tool-1.0.0";
    db["tool"].bindings["root"]["repo:tool-1.0.0"] =
        "repo:1.0.0";
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    auto replacement =
        make_registration_node("root", "repo:1.0.0", "group");
    replacement.sourceName.clear();
    replacement.destinationName.clear();

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({replacement}));

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::IncompleteOwnedGroup,
        "/nodes/0", "tool", "repo:tool-1.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationReconfigureTest,
     ReplacesCurrentExactEdgesAndPreservesAnotherVersion) {
    xlings::xvm::VersionDB db;
    const xlings::xvm::BindingGroupRef currentGroup{
        .provider = "repo:provider",
        .providerVersion = "1.0.0",
        .group = "toolchain",
        .rootTarget = "root",
        .rootVersion = "repo:1.0.0",
    };
    auto& currentRoot = db["root"].versions["repo:1.0.0"];
    currentRoot.path = "/pkg/provider/1.0.0";
    currentRoot.kind = "group";
    currentRoot.bindingGroup = currentGroup;
    currentRoot.bindingMembers = {
        {"root", "repo:1.0.0"},
        {"tool", "repo:tool-1.0.0"},
    };
    currentRoot.bindingMembersDeclared = true;
    auto& currentTool = db["tool"].versions["repo:tool-1.0.0"];
    currentTool.path = "/pkg/provider/1.0.0";
    currentTool.kind = "program";
    currentTool.sourceName = "tool-real";
    currentTool.destinationName = "tool";
    currentTool.bindingGroup = currentGroup;
    db["root"].bindings["tool"]["repo:1.0.0"] =
        "repo:tool-1.0.0";
    db["tool"].bindings["root"]["repo:tool-1.0.0"] =
        "repo:1.0.0";

    auto& stale = db["stale"].versions["repo:stale-1.0.0"];
    stale.path = "/pkg/stale";
    stale.kind = "program";
    stale.sourceName = "stale";
    stale.destinationName = "stale";
    db["root"].bindings["stale"]["repo:1.0.0"] =
        "repo:stale-1.0.0";
    db["stale"].bindings["root"]["repo:stale-1.0.0"] =
        "repo:1.0.0";

    const xlings::xvm::BindingGroupRef oldGroup{
        .provider = "repo:provider",
        .providerVersion = "0.9.0",
        .group = "toolchain",
        .rootTarget = "root",
        .rootVersion = "repo:0.9.0",
    };
    auto& oldRoot = db["root"].versions["repo:0.9.0"];
    oldRoot.path = "/pkg/provider/0.9.0";
    oldRoot.kind = "group";
    oldRoot.bindingGroup = oldGroup;
    oldRoot.bindingMembers = {
        {"root", "repo:0.9.0"},
        {"tool", "repo:tool-0.9.0"},
    };
    oldRoot.bindingMembersDeclared = true;
    auto& oldTool = db["tool"].versions["repo:tool-0.9.0"];
    oldTool.path = "/pkg/provider/0.9.0";
    oldTool.kind = "program";
    oldTool.sourceName = "tool-old";
    oldTool.destinationName = "tool";
    oldTool.alias = {"--old"};
    oldTool.envs = {{"OLD", "1"}};
    oldTool.bindingGroup = oldGroup;
    db["root"].bindings["tool"]["repo:0.9.0"] =
        "repo:tool-0.9.0";
    db["tool"].bindings["root"]["repo:tool-0.9.0"] =
        "repo:0.9.0";
    const auto oldRootBefore = xlings::xvm::vdata_to_json(oldRoot);
    const auto oldToolBefore = xlings::xvm::vdata_to_json(oldTool);
    const auto staleBefore = xlings::xvm::vdata_to_json(stale);

    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto tool =
        make_registration_node("tool", "repo:tool-1.0.0");
    tool.sourceName = "tool-real";
    tool.destinationName = "tool";
    tool.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({tool, root}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(db.at("root").bindings.contains("stale"));
    EXPECT_FALSE(db.at("stale").bindings.contains("root"));
    EXPECT_EQ(
        xlings::xvm::vdata_to_json(
            db.at("root").versions.at("repo:0.9.0")),
        oldRootBefore);
    EXPECT_EQ(
        xlings::xvm::vdata_to_json(
            db.at("tool").versions.at("repo:tool-0.9.0")),
        oldToolBefore);
    EXPECT_EQ(
        xlings::xvm::vdata_to_json(
            db.at("stale").versions.at("repo:stale-1.0.0")),
        staleBefore);
    EXPECT_EQ(
        db.at("root")
            .bindings.at("tool").at("repo:0.9.0"),
        "repo:tool-0.9.0");
    EXPECT_EQ(
        db.at("tool")
            .bindings.at("root").at("repo:tool-0.9.0"),
        "repo:0.9.0");
}

TEST(XvmRegistrationTest, KeepsProviderReleaseGroupsIndependent) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto toolA = make_registration_node("tool", "repo:tool-a");
    toolA.sourceName = "tool-a";
    toolA.binding =
        make_registration_binding("root-a", "repo:a", "group-a");
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolB = make_registration_node("tool", "repo:tool-b");
    toolB.sourceName = "tool-b";
    toolB.binding =
        make_registration_binding("root-b", "repo:b", "group-b");
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({toolB, rootA, toolA, rootB}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    auto selectedA =
        xlings::xvm::resolve_binding_selection(db, "root-a", "repo:a");
    auto selectedB =
        xlings::xvm::resolve_binding_selection(db, "root-b", "repo:b");
    ASSERT_TRUE(selectedA.has_value()) << selectedA.error().message;
    ASSERT_TRUE(selectedB.has_value()) << selectedB.error().message;
    EXPECT_EQ(
        selectedA->members,
        (std::map<std::string, std::string>{
            {"root-a", "repo:a"},
            {"tool", "repo:tool-a"},
        }));
    EXPECT_EQ(
        selectedB->members,
        (std::map<std::string, std::string>{
            {"root-b", "repo:b"},
            {"tool", "repo:tool-b"},
        }));
}

TEST(XvmRegistrationTest,
     SameTargetIndependentGroupsIgnoreBatchNodeOrder) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto toolA =
        make_registration_node("tool", "repo:tool-a", "program");
    toolA.sourceName = "tool-a";
    toolA.destinationName = "tool";
    toolA.binding =
        make_registration_binding("root-a", "repo:a", "group-a");
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolB = make_registration_node("tool", "repo:tool-b", "lib");
    toolB.sourceName = "libtool-b.so.1";
    toolB.destinationName = "libtool.so.1";
    toolB.binding =
        make_registration_binding("root-b", "repo:b", "group-b");

    xlings::xvm::VersionDB forwardDb;
    xlings::xvm::Workspace forwardWorkspace;
    xlings::xvm::WorkspaceInstalled forwardInstalled;
    auto forward = xlings::xvm::apply_registration_batch(
        forwardDb, forwardWorkspace, forwardInstalled,
        make_registration_batch({rootA, toolA, rootB, toolB}));
    ASSERT_TRUE(forward.has_value()) << forward.error().message;

    xlings::xvm::VersionDB reverseDb;
    xlings::xvm::Workspace reverseWorkspace;
    xlings::xvm::WorkspaceInstalled reverseInstalled;
    auto reverse = xlings::xvm::apply_registration_batch(
        reverseDb, reverseWorkspace, reverseInstalled,
        make_registration_batch({toolB, rootB, toolA, rootA}));
    ASSERT_TRUE(reverse.has_value()) << reverse.error().message;

    EXPECT_EQ(
        xlings::xvm::versions_to_json(forwardDb),
        xlings::xvm::versions_to_json(reverseDb));
    EXPECT_EQ(forwardWorkspace, reverseWorkspace);
    EXPECT_EQ(forwardInstalled, reverseInstalled);
}

TEST(XvmRegistrationHeaderTest, RoutesExplicitHeaderToNamedGroupRoot) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto toolA = make_registration_node("tool-a", "repo:a");
    toolA.binding =
        make_registration_binding("root-a", "repo:a", "group-a");
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolB = make_registration_node("tool-b", "repo:b");
    toolB.binding =
        make_registration_binding("root-b", "repo:b", "group-b");
    auto batch =
        make_registration_batch({rootA, toolA, rootB, toolB});
    batch.headers = {
        {
            .sourceDir = "include/compiler",
            .destinationPrefix = "compiler",
            .group = "group-b",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(
        db.at("root-a").versions.at("repo:a").bindingHeaders.empty());
    const auto& routed =
        db.at("root-b").versions.at("repo:b").bindingHeaders;
    ASSERT_EQ(routed.size(), 1u);
    EXPECT_EQ(routed[0].sourceDir, "include/compiler");
    EXPECT_EQ(routed[0].destinationPrefix, "compiler");
}

TEST(XvmRegistrationHeaderTest, AcceptsUngroupedHeaderForSingleGroup) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& data =
        db.at("root").versions.at("repo:1.0.0");
    ASSERT_EQ(data.bindingHeaders.size(), 1u);
    EXPECT_EQ(data.bindingHeaders[0].sourceDir, "include");
    EXPECT_TRUE(data.bindingHeaders[0].destinationPrefix.empty());
    EXPECT_TRUE(data.bindingHeadersDeclared);
}

// A recipe that registers several independent targets and then declares
// headers is ordinary — a package exposing both a program and a library, for
// instance. Every ungrouped node becomes its own singleton group, so the
// "exactly one candidate group" rule alone rejects those recipes outright.
// The package's own target breaks the tie: headers shipped by package `p`
// belong with `p`.
// Installing a release must not split the workspace across two of them.
// The activation decision has to be made once per release: if any member is
// already active, install leaves the whole release alone; a member that is
// new in this release must not be activated on its own.
TEST(XvmRegistrationTest, InstallDoesNotSplitTheWorkspaceAcrossReleases) {
    auto oldGcc = make_registration_node("gcc", "15.1.0");
    auto oldCxx = make_registration_node("g++", "15.1.0");
    oldCxx.binding = xlings::xvm::RegistrationBinding{
        .rootTarget = "gcc", .rootVersion = "15.1.0"};
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto first = xlings::xvm::apply_registration_batch(
        db, workspace, installed, make_registration_batch({oldGcc, oldCxx}));
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_EQ(workspace.at("gcc"), "15.1.0");
    ASSERT_EQ(workspace.at("g++"), "15.1.0");

    // The next release adds a member the previous one did not have.
    auto newGcc = make_registration_node("gcc", "16.1.0");
    auto newCxx = make_registration_node("g++", "16.1.0");
    newCxx.binding = xlings::xvm::RegistrationBinding{
        .rootTarget = "gcc", .rootVersion = "16.1.0"};
    auto newAr = make_registration_node("gcc-ar", "16.1.0");
    newAr.binding = xlings::xvm::RegistrationBinding{
        .rootTarget = "gcc", .rootVersion = "16.1.0"};
    auto batch = make_registration_batch({newGcc, newCxx, newAr});
    batch.providerVersion = "16.1.0";

    auto second = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);
    ASSERT_TRUE(second.has_value()) << second.error().message;

    // Install without an explicit use must not change what is active.
    EXPECT_EQ(workspace.at("gcc"), "15.1.0");
    EXPECT_EQ(workspace.at("g++"), "15.1.0");
    EXPECT_FALSE(workspace.contains("gcc-ar"))
        << "a member new in 16.1.0 was activated while the rest stayed on "
           "15.1.0 — the workspace now spans two releases";
}

TEST(XvmRegistrationHeaderTest, UngroupedHeaderFallsBackToThePrimaryTarget) {
    auto program = make_registration_node("openssl", "repo:3.1.5");
    auto library = make_registration_node("libssl", "repo:3.1.5");
    library.kind = "lib";
    auto batch = make_registration_batch({program, library});
    batch.primaryTarget = "openssl";
    batch.headers = {{.sourceDir = "include"}};
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& owner = db.at("openssl").versions.at("repo:3.1.5");
    ASSERT_EQ(owner.bindingHeaders.size(), 1u);
    EXPECT_EQ(owner.bindingHeaders[0].sourceDir, "include");
    EXPECT_TRUE(db.at("libssl").versions.at("repo:3.1.5").bindingHeaders.empty())
        << "headers leaked onto a group that does not own them";
}

// The tie-break is the package's own target, not "pick the first group".
// When the package registers nothing under its own name there is genuinely
// no owner to infer, and guessing would attach headers to an arbitrary group.
TEST(XvmRegistrationHeaderTest,
     UngroupedHeaderStaysAmbiguousWhenPrimaryTargetIsNotRegistered) {
    auto first = make_registration_node("first", "repo:first");
    auto second = make_registration_node("second", "repo:second");
    auto batch = make_registration_batch({first, second});
    batch.primaryTarget = "not-registered";
    batch.headers = {{.sourceDir = "include"}};
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderAmbiguous,
        "/headers/0/group", "not-registered");
    EXPECT_NE(result.error().message.find("primaryTarget"), std::string::npos)
        << "the error must say how to resolve it, got: " << result.error().message;
}

// The same target bound into two different groups: naming it no longer
// identifies one owner, so the header is still ambiguous. (Registering one
// target at two versions *without* bindings is rejected earlier as a
// GroupConflict, so two roots are what actually reaches this branch.)
TEST(XvmRegistrationHeaderTest,
     UngroupedHeaderStaysAmbiguousWhenPrimaryTargetSpansGroups) {
    auto rootA = make_registration_node("root-a", "repo:a", "group");
    rootA.sourceName.clear();
    rootA.destinationName.clear();
    auto rootB = make_registration_node("root-b", "repo:b", "group");
    rootB.sourceName.clear();
    rootB.destinationName.clear();
    auto toolA = make_registration_node("tool", "repo:1.0.0");
    toolA.binding = xlings::xvm::RegistrationBinding{
        .rootTarget = "root-a", .rootVersion = "repo:a"};
    auto toolB = make_registration_node("tool", "repo:2.0.0");
    toolB.binding = xlings::xvm::RegistrationBinding{
        .rootTarget = "root-b", .rootVersion = "repo:b"};
    auto batch = make_registration_batch({rootA, rootB, toolA, toolB});
    batch.primaryTarget = "tool";
    batch.headers = {{.sourceDir = "include"}};
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderAmbiguous,
        "/headers/0/group", "tool");
    EXPECT_NE(result.error().message.find("spans"), std::string::npos)
        << result.error().message;
}

TEST(XvmRegistrationHeaderErrorTest,
     RejectsAmbiguousUngroupedHeaderWithoutMutation) {
    auto first = make_registration_node("first", "repo:first");
    auto second = make_registration_node("second", "repo:second");
    auto batch = make_registration_batch({first, second});
    batch.headers = {
        {
            .sourceDir = "include",
        },
    };
    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{{"sentinel", {"0"}}};
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderAmbiguous,
        "/headers/0/group");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmRegistrationHeaderErrorTest,
     RejectsMissingExplicitHeaderGroupWithoutMutation) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "include",
            .group = "missing-group",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::HeaderGroupNotFound,
        "/headers/0/group", "missing-group");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationHeaderErrorTest,
     RejectsEmptyHeaderSourceWithoutMutation) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result, xlings::xvm::RegistrationErrorKind::InvalidHeader,
        "/headers/0/sourceDir");
    EXPECT_TRUE(db.empty());
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XvmRegistrationHeaderTest, HeaderNeverCreatesTargetOrVersion) {
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName.clear();
    root.destinationName.clear();
    auto batch = make_registration_batch({root});
    batch.headers = {
        {
            .sourceDir = "phantom-header-target",
            .destinationPrefix = "sdk",
        },
    };
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(db.size(), 1u);
    EXPECT_FALSE(db.contains("phantom-header-target"));
    EXPECT_EQ(
        db.at("root")
            .versions.at("repo:1.0.0")
            .bindingHeaders.size(),
        1u);
}

TEST(XvmRegistrationStateTest,
     RegistersEveryMemberOnceAndHonorsActivationPolicy) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace{
        {"root", "repo:0.9.0"},
        {"tool", "repo:tool-0.9.0"},
        {"runtime", "repo:runtime-0.9.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"root", {"repo:0.9.0"}},
        {
            "tool",
            {
                "repo:tool-0.9.0",
                "repo:tool-1.0.0",
                "repo:tool-1.0.0",
            },
        },
        {"runtime", {"repo:runtime-0.9.0"}},
    };
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName = "must-not-be-a-shim";
    root.destinationName = "must-not-be-a-shim";
    auto tool =
        make_registration_node("tool", "repo:tool-1.0.0");
    tool.sourceName = "tool-real";
    tool.destinationName = "tool";
    tool.binding = make_registration_binding(
        "root", "repo:1.0.0", "toolchain");
    auto runtime = make_registration_node(
        "runtime", "repo:runtime-1.0.0", "lib");
    runtime.sourceName = "libruntime.so.1.0";
    runtime.destinationName = "libruntime.so.1";
    runtime.binding = tool.binding;
    auto batch = make_registration_batch({runtime, root, tool});

    auto first = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(first.has_value()) << first.error().message;
    EXPECT_EQ(first->size(), 3u);
    EXPECT_EQ(workspace.at("root"), "repo:0.9.0");
    EXPECT_EQ(workspace.at("tool"), "repo:tool-0.9.0");
    EXPECT_EQ(
        workspace.at("runtime"), "repo:runtime-0.9.0");
    EXPECT_EQ(
        std::ranges::count(
            installed.at("root"), "repo:1.0.0"),
        1);
    EXPECT_EQ(
        std::ranges::count(
            installed.at("tool"), "repo:tool-1.0.0"),
        1);
    EXPECT_EQ(
        std::ranges::count(
            installed.at("runtime"), "repo:runtime-1.0.0"),
        1);

    batch.useAfterInstall = true;
    auto second = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    ASSERT_TRUE(second.has_value()) << second.error().message;
    EXPECT_EQ(workspace.at("root"), "repo:1.0.0");
    EXPECT_EQ(workspace.at("tool"), "repo:tool-1.0.0");
    EXPECT_EQ(
        workspace.at("runtime"), "repo:runtime-1.0.0");
    EXPECT_EQ(
        std::ranges::count(
            installed.at("tool"), "repo:tool-1.0.0"),
        1);
}

TEST(XvmRegistrationStateTest,
     VirtualGroupRetainsEmptyPayloadAndNoSelfEdge) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto root = make_registration_node("root", "repo:1.0.0", "group");
    root.sourceName = "bogus-executable";
    root.destinationName = "bogus-shim";

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed,
        make_registration_batch({root}));

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& data =
        db.at("root").versions.at("repo:1.0.0");
    EXPECT_EQ(data.kind, "group");
    EXPECT_TRUE(data.sourceName.empty());
    EXPECT_TRUE(data.destinationName.empty());
    EXPECT_FALSE(db.at("root").bindings.contains("root"));
    EXPECT_EQ(workspace.at("root"), "repo:1.0.0");
    EXPECT_EQ(
        installed.at("root"),
        (std::vector<std::string>{"repo:1.0.0"}));
}

TEST(XvmRegistrationStateTest,
     FinalComponentValidationFailurePreservesEveryStateObject) {
    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    const auto staleGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "stale-group",
        "future-root", "repo:root");
    add_provider_group_member(
        db, "stale-member", "repo:stale-member", staleGroup,
        "program", "stale-member", "stale-member");
    xlings::xvm::Workspace workspace{
        {"future-root", "repo:previous"},
        {"sentinel", "0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"future-root", {"repo:previous"}},
        {"sentinel", {"0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto root =
        make_registration_node("future-root", "repo:root", "group");
    auto freshMember =
        make_registration_node("fresh-member", "repo:fresh-member");
    freshMember.binding = make_registration_binding(
        "future-root", "repo:root", "replacement-group");
    auto batch = make_registration_batch({root, freshMember});
    batch.useAfterInstall = true;

    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, batch);

    expect_registration_error(
        result,
        xlings::xvm::RegistrationErrorKind::BindingValidationFailed,
        "/bindingGroups/stale-group", "future-root", "repo:root");
    expect_registration_state_unchanged(
        db, workspace, installed,
        dbBefore, workspaceBefore, installedBefore);
}

TEST(XvmBindingSelectionTest, ProviderGroupResolvesRootAndLeafExactly) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "xim-gnu-gcc",
        "xim-gnu-gcc", "xim:15.1.0");
    const std::map<std::string, std::string> expected{
        {"g++", "xim:15.1.0"},
        {"gcc", "xim:15.1.0"},
        {"gcc-ar", "xim:gcc-15.1.0"},
        {"libstdc++.so.6", "xim:gcc-15.1.0"},
        {"xim-gnu-gcc", "xim:15.1.0"},
    };

    auto& root = add_provider_group_member(
        db, "xim-gnu-gcc", "xim:15.1.0", group, "group");
    root.bindingMembers = expected;
    root.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };
    add_provider_group_member(
        db, "gcc", "xim:15.1.0", group, "program", "gcc-15", "gcc");
    add_provider_group_member(
        db, "g++", "xim:15.1.0", group, "program", "g++-15", "g++");
    add_provider_group_member(
        db, "gcc-ar", "xim:gcc-15.1.0", group,
        "program", "gcc-ar-15", "gcc-ar");
    add_provider_group_member(
        db, "libstdc++.so.6", "xim:gcc-15.1.0", group,
        "lib", "libstdc++.so.6.0.34", "libstdc++.so.6");

    auto fromLeaf = xlings::xvm::resolve_binding_selection(
        db, "gcc", "xim:15.1.0");
    ASSERT_TRUE(fromLeaf.has_value()) << fromLeaf.error().message;
    EXPECT_EQ(fromLeaf->source, xlings::xvm::BindingSource::ProviderGroup);
    EXPECT_EQ(fromLeaf->members, expected);

    auto fromRoot = xlings::xvm::resolve_binding_selection(
        db, "xim-gnu-gcc", "xim:15.1.0");
    ASSERT_TRUE(fromRoot.has_value()) << fromRoot.error().message;
    EXPECT_EQ(fromRoot->source, xlings::xvm::BindingSource::ProviderGroup);
    EXPECT_EQ(fromRoot->members, expected);
}

TEST(XvmBindingSelectionTest, ProviderScopeSeparatesGroupsForSameTarget) {
    xlings::xvm::VersionDB db;
    db["cc"].type = "program";
    db["cc"].filename = "first-writer-cc";

    const auto groupA = make_binding_group_ref(
        "repo-a:toolchain", "1.0.0", "toolchain-a",
        "repo-a:root", "repo-a:1.0.0");
    auto& rootA = add_provider_group_member(
        db, "repo-a:root", "repo-a:1.0.0", groupA, "group");
    rootA.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
        {"repo-a:root", "repo-a:1.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:1.0.0", groupA,
        "program", "cc-a", "cc");

    const auto groupB = make_binding_group_ref(
        "repo-b:toolchain", "2.0.0", "toolchain-b",
        "repo-b:root", "repo-b:2.0.0");
    auto& rootB = add_provider_group_member(
        db, "repo-b:root", "repo-b:2.0.0", groupB, "group");
    rootB.bindingMembers = {
        {"cc", "repo-b:2.0.0"},
        {"repo-b:root", "repo-b:2.0.0"},
    };
    auto& ccB = add_provider_group_member(
        db, "cc", "repo-b:2.0.0", groupB,
        "lib", "libcc-b.so.2", "libcc.so");

    ASSERT_EQ(ccB.kind, "lib");
    EXPECT_EQ(ccB.sourceName, "libcc-b.so.2");
    EXPECT_EQ(ccB.destinationName, "libcc.so");

    auto selectedA = xlings::xvm::resolve_binding_selection(
        db, "cc", "repo-a:1.0.0");
    ASSERT_TRUE(selectedA.has_value()) << selectedA.error().message;
    EXPECT_EQ(selectedA->members,
              (std::map<std::string, std::string>{
                  {"cc", "repo-a:1.0.0"},
                  {"repo-a:root", "repo-a:1.0.0"},
              }));

    auto selectedB = xlings::xvm::resolve_binding_selection(
        db, "cc", "repo-b:2.0.0");
    ASSERT_TRUE(selectedB.has_value()) << selectedB.error().message;
    EXPECT_EQ(selectedB->members,
              (std::map<std::string, std::string>{
                  {"cc", "repo-b:2.0.0"},
                  {"repo-b:root", "repo-b:2.0.0"},
              }));
}

TEST(XvmBindingSelectionTest,
     SameProviderVersionSeparatesGroupRootsAndMembers) {
    xlings::xvm::VersionDB db;
    const auto groupA = make_binding_group_ref(
        "repo:toolchain", "1.0.0", "c-tools",
        "c-root", "repo:c-root");
    auto& rootA = add_provider_group_member(
        db, "c-root", "repo:c-root", groupA, "group");
    rootA.bindingMembers = {
        {"c-root", "repo:c-root"},
        {"cc", "repo:cc"},
    };
    add_provider_group_member(
        db, "cc", "repo:cc", groupA, "program", "cc", "cc");

    const auto groupB = make_binding_group_ref(
        "repo:toolchain", "1.0.0", "fortran-tools",
        "fortran-root", "repo:fortran-root");
    auto& rootB = add_provider_group_member(
        db, "fortran-root", "repo:fortran-root", groupB, "group");
    rootB.bindingMembers = {
        {"cc", "repo:fc"},
        {"fortran-root", "repo:fortran-root"},
    };
    add_provider_group_member(
        db, "cc", "repo:fc", groupB, "program", "fc", "fc");

    auto selectedA =
        xlings::xvm::resolve_binding_selection(db, "cc", "repo:cc");
    ASSERT_TRUE(selectedA.has_value()) << selectedA.error().message;
    EXPECT_EQ(selectedA->members,
              (std::map<std::string, std::string>{
                  {"c-root", "repo:c-root"},
                  {"cc", "repo:cc"},
              }));

    auto selectedB =
        xlings::xvm::resolve_binding_selection(db, "cc", "repo:fc");
    ASSERT_TRUE(selectedB.has_value()) << selectedB.error().message;
    EXPECT_EQ(selectedB->members,
              (std::map<std::string, std::string>{
                  {"cc", "repo:fc"},
                  {"fortran-root", "repo:fortran-root"},
              }));

    rootA.bindingMembers["cc"] = "repo:fc";
    auto crossGroup =
        xlings::xvm::resolve_binding_selection(db, "c-root", "repo:c-root");
    expect_binding_error(
        crossGroup, xlings::xvm::BindingErrorKind::MemberReferenceMismatch,
        "cc", "repo:fc");
}

TEST(XvmBindingSelectionErrorTest, RejectsBindingMembersWithoutGroup) {
    xlings::xvm::VersionDB db;
    auto& data = db["tool"].versions["1.0.0"];
    data.kind = "program";
    data.bindingMembers = {
        {"tool", "1.0.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::PartialProviderMetadata,
        "tool", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest, RejectsBindingHeadersWithoutGroup) {
    xlings::xvm::VersionDB db;
    auto& data = db["tool"].versions["1.0.0"];
    data.kind = "program";
    data.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::PartialProviderMetadata,
        "tool", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsBindingMembersOnNonRoot) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
        {"tool", "1.0.0"},
    };
    auto& member = add_provider_group_member(
        db, "tool", "1.0.0", group, "program");
    member.bindingMembers = {
        {"nested-tool", "1.0.0"},
    };

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "tool", "1.0.0",
        "binding-metadata-on-non-root", "/bindingMembers");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsBindingHeadersOnNonRoot) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
        {"tool", "1.0.0"},
    };
    auto& member = add_provider_group_member(
        db, "tool", "1.0.0", group, "program");
    member.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
    };

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "tool", "1.0.0",
        "binding-metadata-on-non-root", "/bindingHeaders");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsPersistedEmptyNonRootMetadataFields) {
    struct Case {
        std::string_view field;
        nlohmann::json emptyValue;
    };
    const std::array<Case, 2> cases{
        Case{"bindingMembers", nlohmann::json::object()},
        Case{"bindingHeaders", nlohmann::json::array()},
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.field);
        nlohmann::json memberJson{
            {"path", "/pkg/1.0.0"},
            {"kind", "program"},
            {"bindingGroup", valid_binding_group_json()},
            {std::string(testCase.field), testCase.emptyValue},
        };
        auto member = xlings::xvm::vdata_from_json(memberJson);
        auto persisted = xlings::xvm::vdata_to_json(member);
        ASSERT_TRUE(persisted.contains(testCase.field));
        member = reload_vdata(member);

        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        db["tool"].versions["1.0.0"] = std::move(member);

        auto result = xlings::xvm::resolve_binding_selection(
            db, "provider-root", "1.0.0");

        expect_binding_metadata_error(
            result, "tool", "1.0.0",
            "binding-metadata-on-non-root",
            "/" + std::string(testCase.field));
    }
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsEmptyRootManifestTargetBeforeLookup) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"", "existing-empty-target-version"},
        {"provider-root", "1.0.0"},
    };
    add_provider_group_member(
        db, "", "existing-empty-target-version", group, "program");

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "provider-root", "1.0.0",
        "binding-member-target-empty", "/bindingMembers/");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsEmptyRootManifestVersionBeforeLookup) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
        {"tool/member~alias", ""},
    };
    add_provider_group_member(
        db, "tool/member~alias", "", group, "program");

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "provider-root", "1.0.0",
        "binding-member-version-empty",
        "/bindingMembers/tool~1member~0alias");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderGroupRejectsEmptyRootHeaderSource) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider-root", "1.0.0");
    auto& root = add_provider_group_member(
        db, "provider-root", "1.0.0", group, "group");
    root.bindingMembers = {
        {"provider-root", "1.0.0"},
    };
    root.bindingHeaders = {
        {
            .sourceDir = "include",
            .destinationPrefix = "",
        },
        {
            .sourceDir = "",
            .destinationPrefix = "",
        },
    };

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    expect_binding_metadata_error(
        result, "provider-root", "1.0.0",
        "binding-header-source-dir-empty",
        "/bindingHeaders/1/sourceDir");
}

TEST(XvmBindingSelectionErrorTest,
     RejectsEveryEmptyStartingGroupIdentityField) {
    for (const auto& testCase : binding_group_identity_fields) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        auto& start = add_provider_group_member(
            db, "tool", "1.0.0", group, "program");
        ((*start.bindingGroup).*testCase.member).clear();

        auto result = xlings::xvm::resolve_binding_selection(
            db, "tool", "1.0.0");

        expect_binding_metadata_error(
            result, "tool", "1.0.0",
            "binding-group-field-invalid", testCase.path);
    }
}

TEST(XvmBindingSelectionErrorTest,
     RejectsEveryEmptyRootGroupIdentityField) {
    for (const auto& testCase : binding_group_identity_fields) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        ((*root.bindingGroup).*testCase.member).clear();
        add_provider_group_member(
            db, "tool", "1.0.0", group, "program");

        auto result = xlings::xvm::resolve_binding_selection(
            db, "tool", "1.0.0");

        expect_binding_metadata_error(
            result, "provider-root", "1.0.0",
            "binding-group-field-invalid", testCase.path);
    }
}

TEST(XvmBindingSelectionErrorTest,
     RejectsEveryEmptyMemberGroupIdentityField) {
    for (const auto& testCase : binding_group_identity_fields) {
        SCOPED_TRACE(testCase.path);
        xlings::xvm::VersionDB db;
        const auto group = make_binding_group_ref(
            "repo:provider", "1.0.0", "provider-group",
            "provider-root", "1.0.0");
        auto& root = add_provider_group_member(
            db, "provider-root", "1.0.0", group, "group");
        root.bindingMembers = {
            {"provider-root", "1.0.0"},
            {"tool", "1.0.0"},
        };
        auto& member = add_provider_group_member(
            db, "tool", "1.0.0", group, "program");
        ((*member.bindingGroup).*testCase.member).clear();

        auto result = xlings::xvm::resolve_binding_selection(
            db, "provider-root", "1.0.0");

        expect_binding_metadata_error(
            result, "tool", "1.0.0",
            "binding-group-field-invalid", testCase.path);
    }
}

TEST(XvmBindingSelectionErrorTest,
     LegacyRejectsProviderAwareNodeReachedByTraversal) {
    xlings::xvm::VersionDB db;
    db["legacy"].type = "program";
    db["legacy"].versions["1.0.0"].path = "/legacy";
    db["provider"].type = "program";
    auto& provider = db["provider"].versions["1.0.0"];
    provider.path = "/provider";
    provider.bindingGroup = make_binding_group_ref(
        "repo:provider", "1.0.0", "provider-group",
        "provider", "1.0.0");
    db["legacy"].bindings["provider"]["1.0.0"] = "1.0.0";
    db["provider"].bindings["legacy"]["1.0.0"] = "1.0.0";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "legacy", "1.0.0");

    expect_binding_error(
        result,
        xlings::xvm::BindingErrorKind::ProviderMetadataInLegacyGraph,
        "provider", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest,
     ProviderResolutionRejectsPersistedIntegrityIssue) {
    auto corruptRoot = nlohmann::json::parse(R"({
        "path": "/provider",
        "kind": "group",
        "bindingGroup": {
            "provider": "repo:provider",
            "version": "1.0.0",
            "group": "provider-group",
            "rootTarget": "provider-root",
            "rootVersion": "1.0.0"
        },
        "bindingMembers": {
            "provider-root": "1.0.0",
            "tool": false
        }
    })");
    auto persisted = xlings::xvm::vdata_to_json(
        xlings::xvm::vdata_from_json(corruptRoot));
    xlings::xvm::VersionDB db;
    db["provider-root"].versions["1.0.0"] =
        xlings::xvm::vdata_from_json(persisted);

    auto result = xlings::xvm::resolve_binding_selection(
        db, "provider-root", "1.0.0");

    ASSERT_FALSE(result.has_value());
    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::MetadataIntegrityIssue,
        "provider-root", "1.0.0");
    EXPECT_NE(result.error().message.find(
                  "binding-member-version-not-string"),
              std::string::npos);
    EXPECT_NE(result.error().message.find("/bindingMembers/tool"),
              std::string::npos);
}

TEST(XvmBindingSelectionErrorTest,
     LegacyResolutionRejectsPersistedIntegrityIssue) {
    auto corruptLegacy = nlohmann::json::parse(R"({
        "path": "/legacy",
        "bindingHeaders": [false]
    })");
    auto persisted = xlings::xvm::vdata_to_json(
        xlings::xvm::vdata_from_json(corruptLegacy));
    xlings::xvm::VersionDB db;
    db["legacy"].type = "program";
    db["legacy"].versions["1.0.0"] =
        xlings::xvm::vdata_from_json(persisted);

    auto result =
        xlings::xvm::resolve_binding_selection(db, "legacy", "1.0.0");

    ASSERT_FALSE(result.has_value());
    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::MetadataIntegrityIssue,
        "legacy", "1.0.0");
    EXPECT_NE(result.error().message.find("binding-header-not-object"),
              std::string::npos);
    EXPECT_NE(result.error().message.find("/bindingHeaders/0"),
              std::string::npos);
}

TEST(XvmBindingSelectionErrorTest, RejectsMissingStartingTarget) {
    xlings::xvm::VersionDB db;

    auto result =
        xlings::xvm::resolve_binding_selection(db, "missing", "1.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::TargetNotFound,
        "missing", "1.0.0");
}

TEST(XvmBindingSelectionErrorTest, RejectsMissingStartingVersion) {
    xlings::xvm::VersionDB db;
    db["tool"].versions["1.0.0"].path = "/tool";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "2.0.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound,
        "tool", "2.0.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMissingRootVersion) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    db["root"].versions["14.2.0"].kind = "group";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresRootSelfReference) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingGroup->rootTarget = "not-root";
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::RootReferenceMismatch,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresMatchingRootIdentity) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto otherGroup = group;
    otherGroup.provider = "other:gcc";
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", otherGroup, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::GroupIdentityMismatch,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresRootInManifest) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::RootMissingFromManifest,
        "root", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRequiresStartInManifest) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "program", "gcc", "gcc");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::StartMemberMissing,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMissingManifestTarget) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"missing", "15.1.0"},
        {"root", "15.1.0"},
    };

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::TargetNotFound,
        "missing", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMissingManifestVersion) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };
    add_provider_group_member(
        db, "gcc", "14.2.0", group, "program", "gcc", "gcc");

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupRejectsMemberBackReference) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };
    auto otherGroup = group;
    otherGroup.group = "other";
    add_provider_group_member(
        db, "gcc", "15.1.0", otherGroup, "program", "gcc", "gcc");

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::MemberReferenceMismatch,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, ProviderGroupUsesPerVersionKind) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "xim:gcc", "15.1.0", "gcc", "root", "15.1.0");
    auto& root =
        add_provider_group_member(db, "root", "15.1.0", group, "group");
    root.bindingMembers = {
        {"gcc", "15.1.0"},
        {"root", "15.1.0"},
    };
    add_provider_group_member(
        db, "gcc", "15.1.0", group, "archive", "gcc", "gcc");
    db["gcc"].type = "program";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "root", "15.1.0");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::UnsupportedKind,
        "gcc", "15.1.0");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsMissingDestinationVersion) {
    xlings::xvm::VersionDB db;
    db["a"].type = "program";
    db["a"].versions["1"].path = "/a";
    db["b"].type = "program";
    db["b"].versions["1"].path = "/b";
    db["a"].bindings["b"]["1"] = "2";
    db["b"].bindings["a"]["2"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound, "b", "2");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsAsymmetricEdge) {
    xlings::xvm::VersionDB db;
    db["a"].type = "program";
    db["a"].versions["1"].path = "/a";
    db["b"].type = "program";
    db["b"].versions["1"].path = "/b";
    db["a"].bindings["b"]["1"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::AsymmetricEdge, "b", "1");
}

TEST(XvmBindingSelectionErrorTest,
     LegacyRejectsIncomingOnlyEdgeFromStartingNode) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["peer"].type = "program";
    db["peer"].versions["1"].path = "/peer";
    db["peer"].bindings["tool"]["1"] = "1";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::AsymmetricEdge, "peer", "1");
}

TEST(XvmBindingSelectionErrorTest,
     LegacyIncomingEdgeRejectsMissingSourceVersion) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["peer"].type = "program";
    db["peer"].versions["1"].path = "/peer";
    db["peer"].bindings["tool"]["2"] = "1";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::VersionNotFound, "peer", "2");
}

TEST(XvmBindingSelectionErrorTest,
     LegacyIncomingEdgeReportsMismatchedReciprocalSourceExactly) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["peer"].type = "program";
    db["peer"].versions["1"].path = "/peer/1";
    db["peer"].versions["2"].path = "/peer/2";
    db["peer"].bindings["tool"]["1"] = "1";
    db["tool"].bindings["peer"]["1"] = "2";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::AsymmetricEdge, "peer", "1");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsSelfEdge) {
    xlings::xvm::VersionDB db;
    db["a"].type = "program";
    db["a"].versions["1"].path = "/a";
    db["a"].bindings["a"]["1"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::SelfEdge, "a", "1");
}

TEST(XvmBindingSelectionErrorTest, LegacyRejectsTargetVersionConflict) {
    xlings::xvm::VersionDB db;
    for (const auto& [target, version] :
         std::vector<std::pair<std::string, std::string>>{
             {"a", "1"}, {"a", "2"}, {"b", "1"}, {"c", "1"}}) {
        db[target].type = "program";
        db[target].versions[version].path = "/" + target + "/" + version;
    }
    db["a"].bindings["b"]["1"] = "1";
    db["b"].bindings["a"]["1"] = "1";
    db["b"].bindings["c"]["1"] = "1";
    db["c"].bindings["b"]["1"] = "1";
    db["c"].bindings["a"]["1"] = "2";
    db["a"].bindings["c"]["2"] = "1";

    auto result = xlings::xvm::resolve_binding_selection(db, "a", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::ConflictingTargetVersion,
        "a", "2");
}

TEST(XvmBindingSelectionErrorTest, LegacyFallsBackToVInfoKind) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "archive";
    db["tool"].versions["1"].path = "/tool";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::UnsupportedKind,
        "tool", "1");
}

TEST(XvmBindingSelectionErrorTest, LegacyPrefersVDataKindOverVInfo) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1"].path = "/tool";
    db["tool"].versions["1"].kind = "archive";

    auto result =
        xlings::xvm::resolve_binding_selection(db, "tool", "1");

    expect_binding_error(
        result, xlings::xvm::BindingErrorKind::UnsupportedKind,
        "tool", "1");
}

TEST(XvmDbTest, AddVersionWithBinding) {
    xlings::xvm::VersionDB db;

    // Simulate installing gcc package: xim-gnu-gcc is the parent package,
    // gcc, g++, gcc-ar are binding targets
    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "gcc-ar", "gcc-15.1.0", "/pkg/gcc-15", "program", "gcc-ar", "gcc-ar", "", "xim-gnu-gcc@15.1.0");

    // Verify bidirectional bindings exist
    // xim-gnu-gcc should know about gcc, g++, gcc-ar
    auto* parent = xlings::xvm::get_vinfo(db, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    ASSERT_TRUE(parent->bindings.contains("gcc"));
    ASSERT_TRUE(parent->bindings.contains("g++"));
    ASSERT_TRUE(parent->bindings.contains("gcc-ar"));
    EXPECT_EQ(parent->bindings.at("gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("g++").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("gcc-ar").at("15.1.0"), "gcc-15.1.0");

    // gcc should know about xim-gnu-gcc
    auto* gcc_info = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    ASSERT_TRUE(gcc_info->bindings.contains("xim-gnu-gcc"));
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("15.1.0"), "15.1.0");

    // gcc-ar should know about xim-gnu-gcc with correct version mapping
    auto* ar_info = xlings::xvm::get_vinfo(db, "gcc-ar");
    ASSERT_NE(ar_info, nullptr);
    ASSERT_TRUE(ar_info->bindings.contains("xim-gnu-gcc"));
    EXPECT_EQ(ar_info->bindings.at("xim-gnu-gcc").at("gcc-15.1.0"), "15.1.0");
}

TEST(XvmDbTest, AddVersionWithBindingMultipleVersions) {
    xlings::xvm::VersionDB db;

    // Install gcc 15.1.0
    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");

    // Install gcc 14.2.0
    xlings::xvm::add_version(db, "xim-gnu-gcc", "14.2.0", "/pkg/gcc-14");
    xlings::xvm::add_version(db, "gcc", "14.2.0", "/pkg/gcc-14", "program", "gcc", "gcc", "", "xim-gnu-gcc@14.2.0");
    xlings::xvm::add_version(db, "g++", "14.2.0", "/pkg/gcc-14", "program", "g++", "g++", "", "xim-gnu-gcc@14.2.0");

    // Parent should have version mappings for both versions
    auto* parent = xlings::xvm::get_vinfo(db, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->bindings.at("gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("gcc").at("14.2.0"), "14.2.0");
    EXPECT_EQ(parent->bindings.at("g++").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("g++").at("14.2.0"), "14.2.0");

    // Each child should map back to both parent versions
    auto* gcc_info = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("14.2.0"), "14.2.0");
}

TEST(XvmExactRemovalTest, RemovesOnlyExactVersionReciprocalEdges) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain-15");
    xlings::xvm::add_version(
        db, "cc", "15.1.0", "/pkg/toolchain-15",
        "program", "cc-15", "cc", "", "toolchain@15.1.0");
    xlings::xvm::add_version(
        db, "toolchain", "14.2.0", "/pkg/toolchain-14");
    xlings::xvm::add_version(
        db, "cc", "14.2.0", "/pkg/toolchain-14",
        "program", "cc-14", "cc", "", "toolchain@14.2.0");

    ASSERT_TRUE(
        xlings::xvm::remove_version(db, "cc", "15.1.0").has_value());

    ASSERT_TRUE(db.contains("cc"));
    EXPECT_FALSE(db.at("cc").versions.contains("15.1.0"));
    EXPECT_TRUE(db.at("cc").versions.contains("14.2.0"));
    ASSERT_TRUE(db.at("cc").bindings.contains("toolchain"));
    EXPECT_EQ(db.at("cc").bindings.at("toolchain").size(), 1u);
    EXPECT_EQ(
        db.at("cc").bindings.at("toolchain").at("14.2.0"),
        "14.2.0");

    ASSERT_TRUE(db.at("toolchain").bindings.contains("cc"));
    EXPECT_EQ(db.at("toolchain").bindings.at("cc").size(), 1u);
    EXPECT_EQ(
        db.at("toolchain").bindings.at("cc").at("14.2.0"),
        "14.2.0");
}

TEST(XvmExactRemovalTest, AmbiguousBareVersionFailsWithoutMutation) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "cc", "1.0.0", "/pkg/repo-a", "program",
        "cc-a", "cc", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "1.0.0", "/pkg/repo-b", "program",
        "cc-b", "cc", "repo-b");
    const auto before = xlings::xvm::versions_to_json(db);

    auto result = xlings::xvm::remove_version(db, "cc", "1.0.0");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AmbiguousVersion);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_EQ(result.error().version, "1.0.0");
    EXPECT_TRUE(result.error().peerTarget.empty());
    EXPECT_TRUE(result.error().peerVersion.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), before);
}

TEST(XvmRemovalBatchTest, LateAsymmetricEdgeFailsWithoutPartialMutation) {
    xlings::xvm::VersionDB db;
    db["safe"].versions["repo:safe-1"].path = "/pkg/safe";
    db["a"].versions["repo:a-1"].path = "/pkg/a";
    db["b"].versions["repo:b-1"].path = "/pkg/b";
    db["a"].bindings["b"]["repo:a-1"] = "repo:b-1";
    db["b"].bindings["a"]["repo:b-1"] = "repo:wrong-a";
    xlings::xvm::Workspace workspace{
        {"safe", "repo:safe-1"},
        {"a", "repo:a-1"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"safe", {"repo:safe-1"}},
        {"a", {"repo:a-1"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "safe",
            .version = "repo:safe-1",
        },
        {
            .op = "remove",
            .name = "a",
            .version = "repo:a-1",
        },
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AsymmetricEdge);
    EXPECT_EQ(result.error().target, "a");
    EXPECT_EQ(result.error().version, "repo:a-1");
    EXPECT_EQ(result.error().peerTarget, "b");
    EXPECT_EQ(result.error().peerVersion, "repo:b-1");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmExactRemovalTest, UnpairedIncomingEdgeFailsWithoutMutation) {
    xlings::xvm::VersionDB db;
    db["a"].versions["repo:a-1"].path = "/pkg/a";
    db["b"].versions["repo:b-1"].path = "/pkg/b";
    db["b"].bindings["a"]["repo:b-1"] = "repo:a-1";
    const auto before = xlings::xvm::versions_to_json(db);

    auto result =
        xlings::xvm::remove_version(db, "a", "repo:a-1");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AsymmetricEdge);
    EXPECT_EQ(result.error().target, "a");
    EXPECT_EQ(result.error().version, "repo:a-1");
    EXPECT_EQ(result.error().peerTarget, "b");
    EXPECT_EQ(result.error().peerVersion, "repo:b-1");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), before);
}

// ── Group-coherent reactivation after removal ────────────────────────
//
// Removing the active release has to leave the workspace coherent: every
// member of a toolchain either moves to the same surviving release together,
// or the whole group goes inactive. Picking a replacement per target is how
// `gcc` ends up on GCC 15 while `g++` lands on musl's 15 -- the mixed state
// the whole binding-group model exists to prevent, reintroduced at the moment
// of removal.

namespace {

// Register `members` as one provider release, rooted at the first entry.
void add_provider_group_(xlings::xvm::VersionDB& db,
                         std::string_view provider,
                         std::string_view providerVersion,
                         std::string_view group,
                         const std::vector<std::pair<std::string, std::string>>& members) {
    const auto& [rootTarget, rootVersion] = members.front();
    const xlings::xvm::BindingGroupRef ref{
        .provider = std::string(provider),
        .providerVersion = std::string(providerVersion),
        .group = std::string(group),
        .rootTarget = rootTarget,
        .rootVersion = rootVersion,
    };
    std::map<std::string, std::string> manifest;
    for (const auto& [target, version] : members) manifest[target] = version;

    for (const auto& [target, version] : members) {
        auto& info = db[target];
        if (info.type.empty()) info.type = "program";
        auto& data = info.versions[version];
        data.path = std::string("/pkg/") + std::string(provider);
        data.kind = "program";
        data.sourceName = target;
        data.destinationName = target;
        data.bindingGroup = ref;
    }
    auto& root = db[rootTarget].versions[rootVersion];
    root.bindingMembers = manifest;
    root.bindingMembersDeclared = true;
}

}  // namespace

TEST(XvmRemovalFallbackTest, IncoherentSurvivorDeactivatesTheWholeGroup) {
    xlings::xvm::VersionDB db;
    add_provider_group_(db, "pkgindex:gcc", "16.1.0", "gcc",
                        {{"gcc", "16.1.0"}, {"g++", "16.1.0"}});
    add_provider_group_(db, "pkgindex:gcc", "15.1.0", "gcc",
                        {{"gcc", "15.1.0"}, {"g++", "15.1.0"}});
    // musl also provides a `g++`. It is installed, but its own group is not
    // complete here -- only the g++ member of it is present.
    add_provider_group_(db, "pkgindex:musl", "1.2.5", "musl",
                        {{"g++", "musl:15.1.0"}});

    xlings::xvm::Workspace workspace{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};
    // g++ lists the musl build last on purpose: an insertion-order-driven
    // fallback picks it, which is exactly the bug.
    xlings::xvm::WorkspaceInstalled installed{
        {"gcc", {"15.1.0", "16.1.0"}},
        {"g++", {"15.1.0", "16.1.0", "musl:15.1.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "gcc", .version = "16.1.0"},
        {.op = "remove", .name = "g++", .version = "16.1.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    ASSERT_TRUE(workspace.contains("gcc"));
    ASSERT_TRUE(workspace.contains("g++"));
    EXPECT_EQ(workspace.at("gcc"), "15.1.0");
    EXPECT_EQ(workspace.at("g++"), "15.1.0")
        << "g++ fell back to a different provider than gcc";
}

TEST(XvmRemovalFallbackTest, CoherentSurvivingGroupIsActivatedWholesale) {
    xlings::xvm::VersionDB db;
    add_provider_group_(db, "pkgindex:gcc", "16.1.0", "gcc",
                        {{"gcc", "16.1.0"}, {"g++", "16.1.0"}, {"gcc-ar", "16.1.0"}});
    add_provider_group_(db, "pkgindex:gcc", "15.1.0", "gcc",
                        {{"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}});
    xlings::xvm::Workspace workspace{
        {"gcc", "16.1.0"}, {"g++", "16.1.0"}, {"gcc-ar", "16.1.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"gcc", {"15.1.0", "16.1.0"}},
        {"g++", {"15.1.0", "16.1.0"}},
        {"gcc-ar", {"15.1.0", "16.1.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "gcc", .version = "16.1.0"},
        {.op = "remove", .name = "g++", .version = "16.1.0"},
        {.op = "remove", .name = "gcc-ar", .version = "16.1.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(workspace.at("gcc"), "15.1.0");
    EXPECT_EQ(workspace.at("g++"), "15.1.0");
    EXPECT_EQ(workspace.at("gcc-ar"), "15.1.0");
}

TEST(XvmRemovalFallbackTest, NoCompleteSurvivorLeavesEveryMemberInactive) {
    xlings::xvm::VersionDB db;
    add_provider_group_(db, "pkgindex:gcc", "16.1.0", "gcc",
                        {{"gcc", "16.1.0"}, {"g++", "16.1.0"}});
    // 15 is only half installed: its manifest names gcc and g++, but only
    // g++ is registered. Rooted at g++ so the manifest survives the gap.
    add_provider_group_(db, "pkgindex:gcc", "15.1.0", "gcc",
                        {{"g++", "15.1.0"}, {"gcc", "15.1.0"}});
    db.at("gcc").versions.erase("15.1.0");
    xlings::xvm::Workspace workspace{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"gcc", {"16.1.0"}},
        {"g++", {"15.1.0", "16.1.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "gcc", .version = "16.1.0"},
        {.op = "remove", .name = "g++", .version = "16.1.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // g++ 15 survives on disk, but activating it alone would leave a `g++`
    // with no matching `gcc`. Better inactive than incoherent.
    EXPECT_FALSE(workspace.contains("gcc"));
    EXPECT_FALSE(workspace.contains("g++"))
        << "a lone member was activated without the rest of its group";
}

TEST(XvmRemovalFallbackTest, ResultDoesNotDependOnInstalledOrder) {
    const auto run = [](std::vector<std::string> gccOrder) {
        xlings::xvm::VersionDB db;
        for (const auto* v : {"14.1.0", "15.1.0", "16.1.0"}) {
            add_provider_group_(db, "pkgindex:gcc", v, "gcc",
                                {{"gcc", v}, {"g++", v}});
        }
        xlings::xvm::Workspace workspace{{"gcc", "16.1.0"}, {"g++", "16.1.0"}};
        xlings::xvm::WorkspaceInstalled installed{
            {"gcc", gccOrder},
            {"g++", {"14.1.0", "15.1.0", "16.1.0"}},
        };
        const std::vector<xlings::xvm::RemovalOperation> operations{
            {.op = "remove", .name = "gcc", .version = "16.1.0"},
            {.op = "remove", .name = "g++", .version = "16.1.0"},
        };
        auto result = xlings::xvm::apply_removal_batch(
            db, workspace, installed, operations, {});
        EXPECT_TRUE(result.has_value());
        return workspace;
    };

    // installed[] is append-ordered by whatever the user happened to install
    // first. The surviving release must not depend on it.
    const auto ascending = run({"14.1.0", "15.1.0", "16.1.0"});
    const auto descending = run({"16.1.0", "15.1.0", "14.1.0"});
    EXPECT_EQ(ascending, descending);
    EXPECT_EQ(ascending.at("gcc"), "15.1.0") << "expected the highest survivor";
}

TEST(XvmRemovalFallbackTest, UngroupedLegacyTargetStillFallsBack) {
    xlings::xvm::VersionDB db;
    db["tool"].type = "program";
    db["tool"].versions["1.0.0"].path = "/pkg/tool";
    db["tool"].versions["2.0.0"].path = "/pkg/tool";
    xlings::xvm::Workspace workspace{{"tool", "2.0.0"}};
    xlings::xvm::WorkspaceInstalled installed{{"tool", {"1.0.0", "2.0.0"}}};
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {.op = "remove", .name = "tool", .version = "2.0.0"},
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // A target with no group is a group of one, so it can always fall back.
    EXPECT_EQ(workspace.at("tool"), "1.0.0");
}

TEST(XvmRemovalBatchTest, RawEmptyOperationNeverMeansAllVersions) {
    xlings::xvm::VersionDB db;
    db["sibling"].versions["repo-a:1.0.0"].path = "/pkg/a";
    db["sibling"].versions["repo-b:1.0.0"].path = "/pkg/b";
    xlings::xvm::Workspace workspace{
        {"sibling", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"sibling", {"repo-a:1.0.0", "repo-b:1.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "sibling",
            .version = "",
        },
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::AmbiguousVersion);
    EXPECT_EQ(result.error().target, "sibling");
    EXPECT_TRUE(result.error().version.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);

    db["sibling"].versions.erase("repo-b:1.0.0");
    workspace = {{"sibling", "repo-a:1.0.0"}};
    installed = {{"sibling", {"repo-a:1.0.0"}}};
    const auto oneVersionBefore = xlings::xvm::versions_to_json(db);

    result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::VersionNotFound);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), oneVersionBefore);
    EXPECT_EQ(workspace.at("sibling"), "repo-a:1.0.0");
    EXPECT_EQ(
        installed.at("sibling"),
        (std::vector<std::string>{"repo-a:1.0.0"}));
}

TEST(XvmRemovalBatchTest,
     LegacySnapshotRemovesTransformedNamespacedMembersExactly) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain-15",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain-15",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::add_version(
        db, "toolchain", "14.2.0", "/pkg/toolchain-14",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-14.2.0", "/pkg/toolchain-14",
        "program", "cc-14", "cc", "repo-a",
        "toolchain@14.2.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {
            "toolchain",
            {"repo-a:14.2.0", "repo-a:15.1.0"},
        },
        {
            "cc",
            {"repo-a:gcc-14.2.0", "repo-a:gcc-15.1.0"},
        },
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "toolchain",
            .version = "",
        },
    };

    auto context = xlings::xvm::snapshot_removal_context(
        db, "toolchain", "repo-a:15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    ASSERT_TRUE(context->hasSelection);
    EXPECT_TRUE(context->provider.empty());

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "toolchain");
    EXPECT_EQ(result->removed[0].version, "repo-a:15.1.0");
    EXPECT_EQ(result->removed[1].target, "cc");
    EXPECT_EQ(result->removed[1].version, "repo-a:gcc-15.1.0");

    ASSERT_TRUE(db.contains("toolchain"));
    EXPECT_EQ(db.at("toolchain").versions.size(), 1u);
    EXPECT_TRUE(db.at("toolchain").versions.contains("repo-a:14.2.0"));
    ASSERT_TRUE(db.at("toolchain").bindings.contains("cc"));
    EXPECT_EQ(db.at("toolchain").bindings.at("cc").size(), 1u);
    EXPECT_EQ(
        db.at("toolchain").bindings.at("cc").at("repo-a:14.2.0"),
        "repo-a:gcc-14.2.0");

    ASSERT_TRUE(db.contains("cc"));
    EXPECT_EQ(db.at("cc").versions.size(), 1u);
    EXPECT_TRUE(db.at("cc").versions.contains("repo-a:gcc-14.2.0"));
    ASSERT_TRUE(db.at("cc").bindings.contains("toolchain"));
    EXPECT_EQ(db.at("cc").bindings.at("toolchain").size(), 1u);
    EXPECT_EQ(
        db.at("cc").bindings.at("toolchain").at(
            "repo-a:gcc-14.2.0"),
        "repo-a:14.2.0");

    EXPECT_EQ(workspace.at("toolchain"), "repo-a:14.2.0");
    EXPECT_EQ(workspace.at("cc"), "repo-a:gcc-14.2.0");
    EXPECT_EQ(
        installed.at("toolchain"),
        (std::vector<std::string>{"repo-a:14.2.0"}));
    EXPECT_EQ(
        installed.at("cc"),
        (std::vector<std::string>{"repo-a:gcc-14.2.0"}));
}

TEST(XvmRemovalBatchTest,
     ProviderRootOperationRemovesWholeOwnedReleaseOnly) {
    xlings::xvm::VersionDB db;
    auto addRelease = [&](const std::string& release,
                          const std::string& rootVersion,
                          const std::string& memberVersion) {
        const auto group = make_binding_group_ref(
            "repo-a:toolchain", release, "compiler",
            "toolchain", rootVersion);
        auto& root = add_provider_group_member(
            db, "toolchain", rootVersion, group, "group");
        root.bindingMembers = {
            {"cc", memberVersion},
            {"toolchain", rootVersion},
        };
        add_provider_group_member(
            db, "cc", memberVersion, group,
            "program", "cc-a", "cc");
    };
    addRelease("1.0.0", "repo-a:1.0.0", "repo-a:gcc-1.0.0");
    addRelease("2.0.0", "repo-a:2.0.0", "repo-a:gcc-2.0.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:1.0.0"},
        {"cc", "repo-a:gcc-1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:1.0.0", "repo-a:2.0.0"}},
        {"cc", {"repo-a:gcc-1.0.0", "repo-a:gcc-2.0.0"}},
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "toolchain",
        },
    };
    auto context = xlings::xvm::snapshot_removal_context(
        db, "toolchain", "repo-a:1.0.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->removed.size(), 2u);
    EXPECT_FALSE(
        db.at("toolchain").versions.contains("repo-a:1.0.0"));
    EXPECT_TRUE(
        db.at("toolchain").versions.contains("repo-a:2.0.0"));
    EXPECT_FALSE(
        db.at("cc").versions.contains("repo-a:gcc-1.0.0"));
    EXPECT_TRUE(
        db.at("cc").versions.contains("repo-a:gcc-2.0.0"));
    EXPECT_EQ(workspace.at("toolchain"), "repo-a:2.0.0");
    EXPECT_EQ(workspace.at("cc"), "repo-a:gcc-2.0.0");
}

TEST(XvmRemovalBatchTest,
     RemoveAllDeletesOnlyExecutingProviderAcrossReleases) {
    xlings::xvm::VersionDB db;
    auto& providerA1 = db["cc"].versions["repo-a:1.0.0"];
    providerA1.path = "/pkg/a/1";
    providerA1.kind = "program";
    providerA1.bindingGroup = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "cc-a-1",
        "root-a-1", "repo-a:1.0.0");
    auto& providerA2 = db["cc"].versions["repo-a:2.0.0"];
    providerA2.path = "/pkg/a/2";
    providerA2.kind = "program";
    providerA2.bindingGroup = make_binding_group_ref(
        "repo-a:provider", "2.0.0", "cc-a-2",
        "root-a-2", "repo-a:2.0.0");
    auto& providerB = db["cc"].versions["repo-b:9.0.0"];
    providerB.path = "/pkg/b/9";
    providerB.kind = "program";
    // Self-rooted, with a manifest. This entry is the one the workspace is
    // expected to fall back to, and reactivation now requires the candidate's
    // group to actually resolve -- a group whose root is not registered is
    // exactly the dangling state that used to get written into the active
    // workspace. The other entries stay minimal: they are only ever removal
    // subjects here, never fallback candidates.
    providerB.bindingGroup = make_binding_group_ref(
        "repo-b:provider", "9.0.0", "cc-b-9",
        "cc", "repo-b:9.0.0");
    providerB.bindingMembers = {{"cc", "repo-b:9.0.0"}};
    providerB.bindingMembersDeclared = true;
    db["cc"].versions["legacy:0.9.0"].path = "/pkg/legacy";

    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:2.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {
            "cc",
            {
                "legacy:0.9.0",
                "repo-b:9.0.0",
                "repo-a:1.0.0",
                "repo-a:2.0.0",
            },
        },
    };
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove_all",
            .name = "cc",
            .version = "",
        },
    };
    const xlings::xvm::RemovalContext context{
        .provider = "repo-a:provider",
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:1.0.0");
    EXPECT_EQ(result->removed[1].target, "cc");
    EXPECT_EQ(result->removed[1].version, "repo-a:2.0.0");

    ASSERT_TRUE(db.contains("cc"));
    EXPECT_EQ(db.at("cc").versions.size(), 2u);
    EXPECT_TRUE(db.at("cc").versions.contains("repo-b:9.0.0"));
    EXPECT_TRUE(db.at("cc").versions.contains("legacy:0.9.0"));
    EXPECT_EQ(workspace.at("cc"), "repo-b:9.0.0");
    EXPECT_EQ(
        installed.at("cc"),
        (std::vector<std::string>{
            "legacy:0.9.0",
            "repo-b:9.0.0",
        }));
}

TEST(XvmRemovalBatchTest, RemoveAllRejectsMissingProviderContext) {
    xlings::xvm::VersionDB db;
    auto& data = db["cc"].versions["repo-a:1.0.0"];
    data.path = "/pkg/a/1";
    data.kind = "program";
    data.bindingGroup = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "cc-a",
        "root-a", "repo-a:1.0.0");
    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-a:1.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove_all",
            .name = "cc",
            .version = "",
        },
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::ProviderRequired);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_TRUE(result.error().version.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmRemovalBatchTest, RemoveAllRejectsNoOwnedMatchingVersion) {
    xlings::xvm::VersionDB db;
    auto& providerB = db["cc"].versions["repo-b:9.0.0"];
    providerB.path = "/pkg/b/9";
    providerB.kind = "program";
    providerB.bindingGroup = make_binding_group_ref(
        "repo-b:provider", "9.0.0", "cc-b",
        "root-b", "repo-b:9.0.0");
    db["cc"].versions["legacy:0.9.0"].path = "/pkg/legacy";
    xlings::xvm::Workspace workspace{
        {"cc", "repo-b:9.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"legacy:0.9.0", "repo-b:9.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove_all",
            .name = "cc",
            .version = "",
        },
    };
    const xlings::xvm::RemovalContext context{
        .provider = "repo-a:provider",
    };

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, context);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::ProviderVersionNotFound);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_TRUE(result.error().version.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmRemovalBatchTest,
     ProviderSnapshotRejectsRecipeVersionOutsideOwnedSelection) {
    xlings::xvm::VersionDB db;
    const auto groupA = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "group-a",
        "root-a", "repo-a:1.0.0");
    auto& rootA = add_provider_group_member(
        db, "root-a", "repo-a:1.0.0", groupA, "group");
    rootA.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
        {"root-a", "repo-a:1.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:1.0.0", groupA,
        "program", "cc-a", "cc");

    const auto groupB = make_binding_group_ref(
        "repo-b:provider", "2.0.0", "group-b",
        "root-b", "repo-b:2.0.0");
    auto& rootB = add_provider_group_member(
        db, "root-b", "repo-b:2.0.0", groupB, "group");
    rootB.bindingMembers = {
        {"cc", "repo-b:2.0.0"},
        {"root-b", "repo-b:2.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-b:2.0.0", groupB,
        "program", "cc-b", "cc");

    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-a:1.0.0", "repo-b:2.0.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<xlings::xvm::RemovalOperation> operations{
        {
            .op = "remove",
            .name = "cc",
            .version = "repo-b:2.0.0",
        },
    };
    auto context = xlings::xvm::snapshot_removal_context(
        db, "root-a", "repo-a:1.0.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_EQ(context->provider, "repo-a:provider");

    auto result = xlings::xvm::apply_removal_batch(
        db, workspace, installed, operations, *context);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::VersionMismatch);
    EXPECT_EQ(result.error().target, "cc");
    EXPECT_EQ(result.error().version, "repo-b:2.0.0");
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XvmRemovalBatchTest,
     ProviderSnapshotIncludesEveryGroupOwnedByTheRelease) {
    xlings::xvm::VersionDB db;
    const auto compilerGroup = make_binding_group_ref(
        "repo-a:toolchain", "1.0.0", "compiler",
        "compiler-root", "repo-a:1.0.0");
    auto& compilerRoot = add_provider_group_member(
        db, "compiler-root", "repo-a:1.0.0",
        compilerGroup, "group");
    compilerRoot.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
        {"compiler-root", "repo-a:1.0.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:1.0.0",
        compilerGroup, "program", "cc-a", "cc");

    const auto toolsGroup = make_binding_group_ref(
        "repo-a:toolchain", "1.0.0", "tools",
        "tools-root", "repo-a:tools-1.0.0");
    auto& toolsRoot = add_provider_group_member(
        db, "tools-root", "repo-a:tools-1.0.0",
        toolsGroup, "group");
    toolsRoot.bindingMembers = {
        {"ar", "repo-a:tools-1.0.0"},
        {"tools-root", "repo-a:tools-1.0.0"},
    };
    add_provider_group_member(
        db, "ar", "repo-a:tools-1.0.0",
        toolsGroup, "program", "ar-a", "ar");

    auto context = xlings::xvm::snapshot_removal_context(
        db, "cc", "repo-a:1.0.0");

    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_EQ(context->provider, "repo-a:toolchain");
    EXPECT_EQ(
        context->members,
        (std::map<std::string, std::string>{
            {"ar", "repo-a:tools-1.0.0"},
            {"cc", "repo-a:1.0.0"},
            {"compiler-root", "repo-a:1.0.0"},
            {"tools-root", "repo-a:tools-1.0.0"},
        }));
}

TEST(XimXvmRegistrationAdapterTest,
     RootAfterChildProducesOneExactProviderGroup) {
    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "15.1.0";
    provider.namespaceName = "repo-a";
    provider.canonicalName = "repo-a:toolchain";
    provider.storeRoot = "/store";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "cc",
            .version = "gcc-15.1.0",
            .bindir = "/payload/bin",
            .type = "program",
            .filename = "cc-real",
            .binding = "toolchain@15.1.0",
        },
        {
            .op = "add",
            .name = "toolchain",
            .version = "15.1.0",
            .type = "group",
        },
    };

    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo-a", "/data", false);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(plan->batch.provider, "repo-a:toolchain");
    EXPECT_EQ(plan->batch.providerVersion, "15.1.0");
    ASSERT_EQ(plan->batch.nodes.size(), 2u);
    EXPECT_EQ(plan->batch.nodes[0].target, "cc");
    EXPECT_EQ(plan->batch.nodes[0].version, "repo-a:gcc-15.1.0");
    ASSERT_TRUE(plan->batch.nodes[0].binding.has_value());
    EXPECT_EQ(
        plan->batch.nodes[0].binding->rootTarget,
        "toolchain");
    EXPECT_EQ(
        plan->batch.nodes[0].binding->rootVersion,
        "repo-a:15.1.0");
    EXPECT_EQ(plan->batch.nodes[1].target, "toolchain");
    EXPECT_EQ(plan->batch.nodes[1].version, "repo-a:15.1.0");

    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& root =
        db.at("toolchain").versions.at("repo-a:15.1.0");
    const auto& child =
        db.at("cc").versions.at("repo-a:gcc-15.1.0");
    ASSERT_TRUE(root.bindingGroup.has_value());
    ASSERT_TRUE(child.bindingGroup.has_value());
    EXPECT_EQ(root.bindingGroup->provider, "repo-a:toolchain");
    EXPECT_EQ(root.bindingGroup->providerVersion, "15.1.0");
    EXPECT_EQ(root.bindingGroup->group, "toolchain");
    EXPECT_EQ(
        child.bindingGroup->provider,
        root.bindingGroup->provider);
    EXPECT_EQ(
        child.bindingGroup->providerVersion,
        root.bindingGroup->providerVersion);
    EXPECT_EQ(
        child.bindingGroup->group,
        root.bindingGroup->group);
    EXPECT_EQ(
        child.bindingGroup->rootTarget,
        root.bindingGroup->rootTarget);
    EXPECT_EQ(
        child.bindingGroup->rootVersion,
        root.bindingGroup->rootVersion);
    EXPECT_EQ(
        root.bindingMembers,
        (std::map<std::string, std::string>{
            {"cc", "repo-a:gcc-15.1.0"},
            {"toolchain", "repo-a:15.1.0"},
        }));
    EXPECT_EQ(
        installed.at("cc"),
        (std::vector<std::string>{"repo-a:gcc-15.1.0"}));
    EXPECT_EQ(
        installed.at("toolchain"),
        (std::vector<std::string>{"repo-a:15.1.0"}));
}

TEST(XimXvmRegistrationAdapterTest,
     NormalizesVersionNamespaceExactlyOnceAndRejectsConflicts) {
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo-a";
    const auto makeAdd = [](std::string version) {
        return std::vector<mcpplibs::xpkg::XvmOp>{
            {
                .op = "add",
                .name = "tool",
                .version = std::move(version),
            },
        };
    };

    auto matching = xlings::xim::normalize_xpkg_registration_plan(
        provider, makeAdd("repo-a:tool-1.0.0"),
        "repo-a", "/data", false);
    ASSERT_TRUE(matching.has_value()) << matching.error().message;
    ASSERT_EQ(matching->batch.nodes.size(), 1u);
    EXPECT_EQ(
        matching->batch.nodes[0].version,
        "repo-a:tool-1.0.0");

    struct InvalidCase {
        std::string expectedNamespace;
        std::string version;
    };
    const std::array<InvalidCase, 3> invalidCases{
        InvalidCase{"repo-a", "repo-b:tool-1.0.0"},
        InvalidCase{"", "repo-a:tool-1.0.0"},
        InvalidCase{"repo-a", "repo-a:repo-a:tool-1.0.0"},
    };
    for (const auto& testCase : invalidCases) {
        SCOPED_TRACE(testCase.version);
        auto result = xlings::xim::normalize_xpkg_registration_plan(
            provider, makeAdd(testCase.version),
            testCase.expectedNamespace, "/data", false);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(
            result.error().kind,
            xlings::xim::XpkgRegistrationErrorKind::InvalidVersion);
        EXPECT_EQ(result.error().operationIndex, 0u);
        EXPECT_EQ(result.error().target, "tool");
        EXPECT_EQ(result.error().version, testCase.version);
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST(XimXvmRegistrationAdapterTest,
     RejectsMalformedLegacyBindingBeforeRegistration) {
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo-a";
    const std::array<std::string, 4> invalidBindings{
        "root",
        "@1.0.0",
        "root@",
        "root@1.0.0@extra",
    };

    for (const auto& binding : invalidBindings) {
        SCOPED_TRACE(binding);
        const std::vector<mcpplibs::xpkg::XvmOp> operations{
            {
                .op = "add",
                .name = "child",
                .binding = binding,
            },
        };
        auto result = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo-a", "/data", false);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(
            result.error().kind,
            xlings::xim::XpkgRegistrationErrorKind::InvalidBinding);
        EXPECT_EQ(result.error().operationIndex, 0u);
        EXPECT_EQ(result.error().target, "child");
        EXPECT_EQ(result.error().version, binding);
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST(XimXvmRegistrationAdapterTest,
     DeduplicatesEqualEnvironmentAndRejectsConflictingDuplicate) {
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> equalOperations{
        {
            .op = "add",
            .name = "tool",
            .envs = {
                {"MODE", "release"},
                {"MODE", "release"},
            },
        },
    };
    auto equal = xlings::xim::normalize_xpkg_registration_plan(
        provider, equalOperations, "", "/data", false);

    ASSERT_TRUE(equal.has_value()) << equal.error().message;
    ASSERT_EQ(equal->batch.nodes.size(), 1u);
    EXPECT_EQ(
        equal->batch.nodes[0].envs,
        (std::map<std::string, std::string>{
            {"MODE", "release"},
        }));

    const std::vector<mcpplibs::xpkg::XvmOp> conflictOperations{
        {
            .op = "add",
            .name = "tool",
            .envs = {
                {"MODE", "release"},
                {"MODE", "debug"},
            },
        },
    };
    auto conflict = xlings::xim::normalize_xpkg_registration_plan(
        provider, conflictOperations, "", "/data", false);

    ASSERT_FALSE(conflict.has_value());
    EXPECT_EQ(
        conflict.error().kind,
        xlings::xim::XpkgRegistrationErrorKind::ConflictingEnvironment);
    EXPECT_EQ(conflict.error().operationIndex, 0u);
    EXPECT_EQ(conflict.error().target, "tool");
    EXPECT_EQ(conflict.error().version, "MODE");
    EXPECT_FALSE(conflict.error().message.empty());
}

TEST(XimXvmRegistrationAdapterTest,
     CanonicalProviderAndVersionPayloadDriveDeferredEffects) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-name";
    provider.version = "2.0.0";
    provider.namespaceName = "repo-a";
    provider.canonicalName = "canonical:provider";
    provider.storeRoot = "/store";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "cc",
            .alias = "--driver",
            .envs = {
                {"CC_MODE", "strict"},
            },
        },
        {
            .op = "add",
            .name = "compiler-runtime",
            .version = "runtime-2.0.0",
            .bindir = "/payload/lib",
            .type = "lib",
            .filename = "libcompiler.so.2",
        },
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
    };

    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo-a", "/data", true);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(plan->batch.provider, "canonical:provider");
    EXPECT_EQ(plan->batch.providerVersion, "2.0.0");
    EXPECT_TRUE(plan->batch.useAfterInstall);
    ASSERT_EQ(plan->batch.nodes.size(), 3u);

    const auto& program = plan->batch.nodes[0];
    EXPECT_EQ(program.target, "cc");
    EXPECT_EQ(program.version, "repo-a:2.0.0");
    // Built as a filesystem path, so compare as one: on Windows the
    // separators are backslashes and a POSIX literal never matches.
    EXPECT_EQ(
        program.path,
        (std::filesystem::path("/store") / "repo-a-x-recipe-name" / "2.0.0")
            .string());
    EXPECT_EQ(program.kind, "program");
    EXPECT_EQ(program.sourceName, "cc");
    EXPECT_EQ(program.destinationName, "cc");
    EXPECT_EQ(
        program.alias,
        (std::vector<std::string>{"--driver"}));
    EXPECT_EQ(
        program.envs,
        (std::map<std::string, std::string>{
            {"CC_MODE", "strict"},
        }));

    const auto& library = plan->batch.nodes[1];
    EXPECT_EQ(library.target, "compiler-runtime");
    EXPECT_EQ(library.version, "repo-a:runtime-2.0.0");
    EXPECT_EQ(library.path, "/payload/lib");
    EXPECT_EQ(library.kind, "lib");
    EXPECT_EQ(library.sourceName, "libcompiler.so.2");
    EXPECT_EQ(library.destinationName, "libcompiler.so.2");

    const auto& group = plan->batch.nodes[2];
    EXPECT_EQ(group.target, "toolchain");
    EXPECT_EQ(group.version, "repo-a:2.0.0");
    EXPECT_EQ(group.kind, "group");
    EXPECT_TRUE(group.sourceName.empty());
    EXPECT_TRUE(group.destinationName.empty());

    ASSERT_EQ(plan->effects.size(), 2u);
    EXPECT_EQ(
        plan->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::ProgramShim);
    EXPECT_EQ(plan->effects[0].target, "cc");
    EXPECT_EQ(plan->effects[0].version, "repo-a:2.0.0");
    EXPECT_EQ(
        plan->effects[1].kind,
        xlings::xim::XpkgFilesystemEffectKind::Library);
    EXPECT_EQ(plan->effects[1].target, "compiler-runtime");
    EXPECT_EQ(
        plan->effects[1].version,
        "repo-a:runtime-2.0.0");
}

TEST(XimXvmRegistrationAdapterTest,
     UngroupedHeaderRoutesToOnlyRootWithoutPhantomProviderTarget) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo-a";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
        {
            .op = "headers",
            .includedir = "/payload/include",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo-a", "/data", false);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    ASSERT_EQ(plan->batch.headers.size(), 1u);
    EXPECT_EQ(
        plan->batch.headers[0].sourceDir,
        "/payload/include");
    EXPECT_TRUE(plan->batch.headers[0].destinationPrefix.empty());
    EXPECT_TRUE(plan->batch.headers[0].group.empty());
    ASSERT_EQ(plan->effects.size(), 1u);
    EXPECT_EQ(
        plan->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::InstallHeaders);
    EXPECT_EQ(plan->effects[0].sourceDir, "/payload/include");

    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_TRUE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("recipe-provider"));
    const auto& root =
        db.at("toolchain").versions.at("repo-a:1.0.0");
    ASSERT_EQ(root.bindingHeaders.size(), 1u);
    EXPECT_EQ(root.bindingHeaders[0].sourceDir, "/payload/include");
    EXPECT_TRUE(root.bindingHeaders[0].destinationPrefix.empty());
}

// The shape this actually unblocks: a package that registers a program and a
// library under no binding, then declares headers. Both nodes become
// singleton groups, so before the primaryTarget tie-break this recipe failed
// to install outright. Goes through the normalizer, so it also covers the
// wiring -- that the batch carries the package's own name as the hint.
TEST(XimXvmRegistrationAdapterTest,
     UngroupedHeaderRoutesToThePackagesOwnTarget) {
    xlings::xim::PlanNode provider;
    provider.name = "openssl";
    provider.version = "3.1.5";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {.op = "add", .name = "openssl"},
        {.op = "add", .name = "libssl", .type = "lib", .filename = "libssl.so"},
        {.op = "headers", .includedir = "/payload/include"},
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_EQ(plan->batch.primaryTarget, "openssl");

    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto& owner = db.at("openssl").versions.at("3.1.5");
    ASSERT_EQ(owner.bindingHeaders.size(), 1u);
    EXPECT_EQ(owner.bindingHeaders[0].sourceDir, "/payload/include");
    EXPECT_TRUE(db.at("libssl").versions.at("3.1.5").bindingHeaders.empty());
}

TEST(XimXvmRegistrationAdapterTest,
     UngroupedHeaderRejectsMultipleGroupsWithoutMutation) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-provider";
    provider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "compiler",
            .type = "group",
        },
        {
            .op = "add",
            .name = "tools",
            .version = "tools-1.0.0",
            .type = "group",
        },
        {
            .op = "headers",
            .includedir = "/payload/include",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;

    xlings::xvm::VersionDB db;
    db["sentinel"].versions["0"].path = "/sentinel";
    xlings::xvm::Workspace workspace{{"sentinel", "0"}};
    xlings::xvm::WorkspaceInstalled installed{
        {"sentinel", {"0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    auto result = xlings::xvm::apply_registration_batch(
        db, workspace, installed, plan->batch);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RegistrationErrorKind::HeaderAmbiguous);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmRegistrationAdapterTest,
     RemoveHeadersProducesOnlyADeferredCompatibilityEffect) {
    xlings::xim::PlanNode provider;
    provider.name = "recipe-provider";
    provider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove_headers",
            .includedir = "/payload/include",
        },
    };

    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "", "/data", false);

    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    EXPECT_TRUE(plan->batch.nodes.empty());
    EXPECT_TRUE(plan->batch.headers.empty());
    ASSERT_EQ(plan->effects.size(), 1u);
    EXPECT_EQ(
        plan->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::RemoveHeaders);
    EXPECT_EQ(plan->effects[0].sourceDir, "/payload/include");
}

TEST(XimXvmMetadataBatchTest,
     LateRegistrationConflictPreservesCallerStateAndArtifact) {
    xlings::xvm::VersionDB db;
    auto& oldVersion = db["old-tool"].versions["repo:old-1.0.0"];
    oldVersion.path = "/old/tool";
    oldVersion.kind = "program";
    oldVersion.sourceName = "old-tool";
    oldVersion.destinationName = "old-tool";

    auto& conflict = db["new-tool"].versions["repo:new-1.0.0"];
    conflict.path = "/other/tool";
    conflict.kind = "program";
    conflict.sourceName = "other-tool";
    conflict.destinationName = "new-tool";
    conflict.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "other:provider",
        .providerVersion = "1.0.0",
        .group = "new-tool",
        .rootTarget = "new-tool",
        .rootVersion = "repo:new-1.0.0",
    };
    conflict.bindingMembers = {
        {"new-tool", "repo:new-1.0.0"},
    };
    conflict.bindingMembersDeclared = true;

    xlings::xvm::Workspace workspace{
        {"old-tool", "repo:old-1.0.0"},
        {"new-tool", "repo:new-1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"old-tool", {"repo:old-1.0.0"}},
        {"new-tool", {"repo:new-1.0.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = "old-tool",
            .version = "repo:old-1.0.0",
        },
        {
            .op = "add",
            .name = "new-tool",
            .version = "repo:new-1.0.0",
            .bindir = "/new/tool",
        },
    };
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:provider";
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        provider.canonicalName, provider.version);
    ASSERT_TRUE(context.has_value()) << context.error().message;

    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const auto artifactDir =
        std::filesystem::temp_directory_path()
        / std::format(
            "xlings-xvm-metadata-batch-sentinel-{}",
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
    std::filesystem::create_directories(artifactDir);
    const auto artifact = artifactDir / "keep";
    {
        std::ofstream output(artifact);
        output << "unchanged";
    }

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations, *context, *plan);

    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<xlings::xvm::RegistrationError>(
        result.error()));
    EXPECT_EQ(
        std::get<xlings::xvm::RegistrationError>(result.error()).kind,
        xlings::xvm::RegistrationErrorKind::OwnershipConflict);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
    ASSERT_TRUE(std::filesystem::exists(artifact));
    std::ifstream input(artifact);
    std::string artifactContents;
    input >> artifactContents;
    EXPECT_EQ(artifactContents, "unchanged");
    // Non-throwing: Windows refuses to delete a file that is still open,
    // and a cleanup failure here says nothing about what was tested.
    std::error_code cleanupEc;
    std::filesystem::remove_all(artifactDir, cleanupEc);
}

TEST(XimXvmMetadataBatchTest,
     RemoveOldAddNewCommitsAndPreservesAnotherProviderRelease) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const auto seedRelease = [&](
            const std::string& release,
            bool useAfterInstall) {
        xlings::xim::PlanNode provider;
        provider.name = "toolchain";
        provider.version = release;
        provider.namespaceName = "repo";
        provider.canonicalName = "repo:toolchain";
        const std::vector<mcpplibs::xpkg::XvmOp> operations{
            {
                .op = "add",
                .name = "cc",
                .version = "cc-" + release,
                .bindir = "/payload/" + release + "/bin",
                .binding = "toolchain@" + release,
            },
            {
                .op = "add",
                .name = "compiler-runtime",
                .version = "runtime-" + release,
                .bindir = "/payload/" + release + "/lib",
                .type = "lib",
                .filename = "libcompiler.so",
                .binding = "toolchain@" + release,
            },
            {
                .op = "add",
                .name = "toolchain",
                .type = "group",
            },
        };
        auto plan = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo", "/data", useAfterInstall);
        EXPECT_TRUE(plan.has_value());
        if (!plan) return;
        auto result = xlings::xvm::apply_registration_batch(
            db, workspace, installed, plan->batch);
        EXPECT_TRUE(result.has_value());
    };
    seedRelease("0.9.0", false);
    seedRelease("1.0.0", true);
    ASSERT_EQ(db.at("toolchain").versions.size(), 2u);

    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "2.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:toolchain";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = "toolchain",
            .version = "repo:1.0.0",
        },
        {
            .op = "add",
            .name = "cc",
            .version = "cc-2.0.0",
            .bindir = "/payload/2.0.0/bin",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "compiler-runtime",
            .version = "runtime-2.0.0",
            .bindir = "/payload/2.0.0/lib",
            .type = "lib",
            .filename = "libcompiler.so",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", true);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        provider.canonicalName, provider.version);
    ASSERT_TRUE(context.has_value()) << context.error().message;

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations, *context, *plan);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->removal.removed.size(), 3u);
    EXPECT_EQ(result->registered.size(), 3u);
    ASSERT_EQ(result->effects.size(), 2u);
    EXPECT_EQ(
        result->effects[0].kind,
        xlings::xim::XpkgFilesystemEffectKind::ProgramShim);
    EXPECT_EQ(
        result->effects[1].kind,
        xlings::xim::XpkgFilesystemEffectKind::Library);
    for (const auto& [target, oldVersion, otherVersion, newVersion] :
         std::array<std::array<std::string, 4>, 3>{
             std::array<std::string, 4>{
                 "toolchain", "repo:1.0.0",
                 "repo:0.9.0", "repo:2.0.0",
             },
             std::array<std::string, 4>{
                 "cc", "repo:cc-1.0.0",
                 "repo:cc-0.9.0", "repo:cc-2.0.0",
             },
             std::array<std::string, 4>{
                 "compiler-runtime", "repo:runtime-1.0.0",
                 "repo:runtime-0.9.0", "repo:runtime-2.0.0",
             },
         }) {
        SCOPED_TRACE(target);
        ASSERT_TRUE(db.contains(target));
        EXPECT_FALSE(db.at(target).versions.contains(oldVersion));
        EXPECT_TRUE(db.at(target).versions.contains(otherVersion));
        EXPECT_TRUE(db.at(target).versions.contains(newVersion));
        EXPECT_EQ(workspace.at(target), newVersion);
        EXPECT_EQ(
            installed.at(target),
            (std::vector<std::string>{otherVersion, newVersion}));
    }
    const auto& newRoot =
        db.at("toolchain").versions.at("repo:2.0.0");
    ASSERT_TRUE(newRoot.bindingGroup.has_value());
    EXPECT_EQ(newRoot.bindingGroup->provider, "repo:toolchain");
    EXPECT_EQ(newRoot.bindingGroup->providerVersion, "2.0.0");
    EXPECT_EQ(newRoot.bindingMembers.size(), 3u);
}

TEST(XimXvmMetadataBatchTest,
     EmptyMetadataOperationsDoNotPurgeDiscoveredProviderSelection) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:provider";
    const std::vector<mcpplibs::xpkg::XvmOp> seedOperations{
        {
            .op = "add",
            .name = "provider-root",
            .type = "group",
        },
    };
    auto seedPlan = xlings::xim::normalize_xpkg_registration_plan(
        provider, seedOperations, "repo", "/data", true);
    ASSERT_TRUE(seedPlan.has_value()) << seedPlan.error().message;
    auto seeded = xlings::xvm::apply_registration_batch(
        db, workspace, installed, seedPlan->batch);
    ASSERT_TRUE(seeded.has_value()) << seeded.error().message;

    const std::vector<mcpplibs::xpkg::XvmOp> operations;
    auto emptyPlan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", false);
    ASSERT_TRUE(emptyPlan.has_value()) << emptyPlan.error().message;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        provider.canonicalName, provider.version);
    ASSERT_TRUE(context.has_value()) << context.error().message;
    ASSERT_TRUE(context->hasSelection);
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed,
        operations, *context, *emptyPlan);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->removal.removed.empty());
    EXPECT_TRUE(result->registered.empty());
    EXPECT_TRUE(result->effects.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmMetadataBatchTest,
     RootOrderPermutationsSerializeIdenticallyThroughAdapter) {
    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:toolchain";
    const mcpplibs::xpkg::XvmOp root{
        .op = "add",
        .name = "toolchain",
        .type = "group",
    };
    const mcpplibs::xpkg::XvmOp child{
        .op = "add",
        .name = "cc",
        .version = "cc-1.0.0",
        .bindir = "/payload/bin",
        .filename = "cc-real",
        .binding = "toolchain@1.0.0",
    };
    const auto apply = [&](
            const std::vector<mcpplibs::xpkg::XvmOp>& operations) {
        xlings::xvm::VersionDB db;
        xlings::xvm::Workspace workspace;
        xlings::xvm::WorkspaceInstalled installed;
        auto plan = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo", "/data", false);
        EXPECT_TRUE(plan.has_value());
        if (!plan) {
            return std::tuple{
                nlohmann::json{},
                xlings::xvm::Workspace{},
                xlings::xvm::WorkspaceInstalled{},
            };
        }
        auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
            db, workspace, installed, operations,
            xlings::xvm::RemovalContext{}, *plan);
        EXPECT_TRUE(result.has_value());
        return std::tuple{
            xlings::xvm::versions_to_json(db),
            std::move(workspace),
            std::move(installed),
        };
    };

    const auto rootFirst = apply({root, child});
    const auto rootLast = apply({child, root});

    EXPECT_EQ(rootFirst, rootLast);
}

TEST(XimXvmMetadataBatchTest,
     PhantomAndSelfBindingsPropagateWithoutStateMutation) {
    struct InvalidCase {
        std::string binding;
        xlings::xvm::RegistrationErrorKind expected;
    };
    const std::array<InvalidCase, 2> invalidCases{
        InvalidCase{
            "missing-root@1.0.0",
            xlings::xvm::RegistrationErrorKind::RootNotInBatch,
        },
        InvalidCase{
            "child@1.0.0",
            xlings::xvm::RegistrationErrorKind::SelfBinding,
        },
    };
    for (const auto& testCase : invalidCases) {
        SCOPED_TRACE(testCase.binding);
        xlings::xim::PlanNode provider;
        provider.name = "provider";
        provider.version = "1.0.0";
        provider.namespaceName = "repo";
        provider.canonicalName = "repo:provider";
        const std::vector<mcpplibs::xpkg::XvmOp> operations{
            {
                .op = "add",
                .name = "child",
                .binding = testCase.binding,
            },
        };
        auto plan = xlings::xim::normalize_xpkg_registration_plan(
            provider, operations, "repo", "/data", false);
        ASSERT_TRUE(plan.has_value()) << plan.error().message;

        xlings::xvm::VersionDB db;
        db["sentinel"].versions["0"].path = "/sentinel";
        xlings::xvm::Workspace workspace{{"sentinel", "0"}};
        xlings::xvm::WorkspaceInstalled installed{
            {"sentinel", {"0"}},
        };
        const auto dbBefore = xlings::xvm::versions_to_json(db);
        const auto workspaceBefore = workspace;
        const auto installedBefore = installed;

        auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
            db, workspace, installed, operations,
            xlings::xvm::RemovalContext{}, *plan);

        ASSERT_FALSE(result.has_value());
        ASSERT_TRUE(std::holds_alternative<
            xlings::xvm::RegistrationError>(result.error()));
        EXPECT_EQ(
            std::get<xlings::xvm::RegistrationError>(
                result.error()).kind,
            testCase.expected);
        EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
        EXPECT_EQ(workspace, workspaceBefore);
        EXPECT_EQ(installed, installedBefore);
    }
}

TEST(XimXvmMetadataBatchTest,
     AdoptsCompatibleLegacyStateAndRerunsIdempotentlyThroughAdapter) {
    xlings::xvm::VersionDB db;
    seed_complete_legacy_registration_group(db);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    xlings::xim::PlanNode provider;
    provider.name = "provider";
    provider.version = "1.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:provider";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "tool",
            .version = "tool-1.0.0",
            .bindir = "/pkg/provider/1.0.0",
            .alias = "tool-alias",
            .filename = "tool-real",
            .binding = "legacy-root@1.0.0",
            .envs = {
                {"TOOL_ENV", "tool"},
            },
        },
        {
            .op = "add",
            .name = "legacy-root",
            .bindir = "/pkg/provider/1.0.0",
            .alias = "root-alias",
            .filename = "legacy-root-real",
            .envs = {
                {"ROOT_ENV", "root"},
            },
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", false);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;

    auto first = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations,
        xlings::xvm::RemovalContext{}, *plan);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(
        db.at("legacy-root")
            .versions.at("repo:1.0.0")
            .bindingGroup.has_value());
    EXPECT_EQ(
        db.at("legacy-root")
            .versions.at("repo:1.0.0")
            .bindingGroup->provider,
        "repo:provider");
    EXPECT_EQ(
        installed.at("legacy-root"),
        (std::vector<std::string>{"repo:1.0.0"}));
    EXPECT_EQ(
        installed.at("tool"),
        (std::vector<std::string>{"repo:tool-1.0.0"}));
    const auto dbAfterFirst = xlings::xvm::versions_to_json(db);
    const auto workspaceAfterFirst = workspace;
    const auto installedAfterFirst = installed;

    auto second = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations,
        xlings::xvm::RemovalContext{}, *plan);

    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbAfterFirst);
    EXPECT_EQ(workspace, workspaceAfterFirst);
    EXPECT_EQ(installed, installedAfterFirst);
}

TEST(XimXvmMetadataBatchTest,
     SameProviderDifferentReleaseCollisionPreservesState) {
    xlings::xvm::VersionDB db;
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    xlings::xim::PlanNode oldProvider;
    oldProvider.name = "provider";
    oldProvider.version = "0.9.0";
    oldProvider.namespaceName = "repo";
    oldProvider.canonicalName = "repo:provider";
    const std::vector<mcpplibs::xpkg::XvmOp> oldOperations{
        {
            .op = "add",
            .name = "tool",
            .version = "shared-1.0.0",
            .bindir = "/old/tool",
        },
    };
    auto oldPlan = xlings::xim::normalize_xpkg_registration_plan(
        oldProvider, oldOperations, "repo", "/data", false);
    ASSERT_TRUE(oldPlan.has_value()) << oldPlan.error().message;
    auto seeded = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, oldOperations,
        xlings::xvm::RemovalContext{}, *oldPlan);
    ASSERT_TRUE(seeded.has_value());

    xlings::xim::PlanNode newProvider = oldProvider;
    newProvider.version = "1.0.0";
    const std::vector<mcpplibs::xpkg::XvmOp> newOperations{
        {
            .op = "add",
            .name = "tool",
            .version = "shared-1.0.0",
            .bindir = "/new/tool",
        },
    };
    auto newPlan = xlings::xim::normalize_xpkg_registration_plan(
        newProvider, newOperations, "repo", "/data", false);
    ASSERT_TRUE(newPlan.has_value()) << newPlan.error().message;
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto result = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, newOperations,
        xlings::xvm::RemovalContext{}, *newPlan);

    ASSERT_FALSE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<
        xlings::xvm::RegistrationError>(result.error()));
    EXPECT_EQ(
        std::get<xlings::xvm::RegistrationError>(
            result.error()).kind,
        xlings::xvm::RegistrationErrorKind::OwnershipConflict);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmMetadataBatchTest,
     ResolvesDeferredEffectsFromFinalExactMetadata) {
    xlings::xvm::VersionDB db;
    auto& oldProgram = db["cc"].versions["repo:cc-1.0.0"];
    oldProgram.path = "/old/bin";
    oldProgram.kind = "program";
    oldProgram.sourceName = "old-cc";
    oldProgram.destinationName = "cc";
    auto& oldLibrary =
        db["compiler-runtime"].versions["repo:runtime-1.0.0"];
    oldLibrary.path = "/old/lib";
    oldLibrary.kind = "lib";
    oldLibrary.sourceName = "libold.so";
    oldLibrary.destinationName = "libold.so";
    xlings::xvm::Workspace workspace{
        {"cc", "repo:cc-1.0.0"},
        {"compiler-runtime", "repo:runtime-1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo:cc-1.0.0"}},
        {"compiler-runtime", {"repo:runtime-1.0.0"}},
    };
    xlings::xim::PlanNode provider;
    provider.name = "toolchain";
    provider.version = "2.0.0";
    provider.namespaceName = "repo";
    provider.canonicalName = "repo:toolchain";
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "add",
            .name = "cc",
            .version = "cc-2.0.0",
            .bindir = "/final/bin",
            .filename = "cc-real",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "compiler-runtime",
            .version = "runtime-2.0.0",
            .bindir = "/final/lib",
            .type = "lib",
            .filename = "libcompiler.so.2",
            .binding = "toolchain@2.0.0",
        },
        {
            .op = "add",
            .name = "toolchain",
            .type = "group",
        },
    };
    auto plan = xlings::xim::normalize_xpkg_registration_plan(
        provider, operations, "repo", "/data", true);
    ASSERT_TRUE(plan.has_value()) << plan.error().message;
    auto applied = xlings::xim::apply_xpkg_xvm_metadata_batch(
        db, workspace, installed, operations,
        xlings::xvm::RemovalContext{}, *plan);
    ASSERT_TRUE(applied.has_value());
    ASSERT_EQ(applied->effects.size(), 2u);

    auto program = xlings::xim::resolve_xpkg_filesystem_effect(
        db, workspace, applied->effects[0]);
    auto library = xlings::xim::resolve_xpkg_filesystem_effect(
        db, workspace, applied->effects[1]);

    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(
        program->kind,
        xlings::xim::XpkgFilesystemEffectKind::ProgramShim);
    EXPECT_EQ(program->target, "cc");
    EXPECT_EQ(program->version, "repo:cc-2.0.0");
    EXPECT_EQ(program->path, "/final/bin");
    EXPECT_EQ(program->sourceName, "cc-real");
    EXPECT_EQ(program->destinationName, "cc");
    EXPECT_TRUE(program->active);
    ASSERT_TRUE(library.has_value());
    EXPECT_EQ(
        library->kind,
        xlings::xim::XpkgFilesystemEffectKind::Library);
    EXPECT_EQ(library->target, "compiler-runtime");
    EXPECT_EQ(library->version, "repo:runtime-2.0.0");
    EXPECT_EQ(library->path, "/final/lib");
    EXPECT_EQ(library->sourceName, "libcompiler.so.2");
    EXPECT_EQ(library->destinationName, "libcompiler.so.2");
    EXPECT_TRUE(library->active);
    EXPECT_FALSE(std::ranges::any_of(
        applied->effects,
        [](const auto& effect) {
            return effect.target == "toolchain";
        }));
}

namespace {

int run_xvm_registration_production_child_(
        const std::filesystem::path& root) {
    namespace fs = std::filesystem;

    const auto fail = [](int code, std::string_view message) {
        std::cerr << "xvm registration production child: "
                  << message << '\n';
        return code;
    };
    const auto write_text = [&](const fs::path& path,
                                std::string_view contents) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path);
        output << contents;
        return output.good();
    };

    // Compare where the paths point, not how they are spelled. The test
    // builds its expectations from fs::temp_directory_path(), while Config
    // discovers the project root by walking up from the working directory.
    // On macOS the temp dir lives under /var, which is a symlink to
    // /private/var, so the two spellings differ while naming one directory.
    const auto same_dir = [](const fs::path& lhs, const fs::path& rhs) {
        std::error_code ec;
        return fs::weakly_canonical(lhs, ec) == fs::weakly_canonical(rhs, ec);
    };

    const auto home = root / "home" / ".xlings";
    const auto project = root / "project";
    const auto payload = root / "payload";
    const auto temp = root / "tmp";
    const auto projectSubos = project / ".xlings" / "subos" / "_";
    const auto globalSubos = home / "subos" / "env-scope";
    fs::create_directories(temp);
    fs::create_directories(project);
    fs::create_directories(payload / "bin");
    fs::create_directories(globalSubos);
    if (!write_text(
            home / ".xlings.json",
            R"({"activeSubos":"persisted-scope"})")
        || !write_text(
            globalSubos / ".xlings.json",
            R"({"workspace":{}})")
        || !write_text(
            project / ".xlings.json",
            R"({"workspace":{}})")) {
        return fail(2, "failed to create isolated Config fixtures");
    }

    xlings::platform::set_env_variable("HOME", (root / "home").string());
    xlings::platform::set_env_variable("XLINGS_HOME", home.string());
    xlings::platform::set_env_variable(
        "XLINGS_ACTIVE_SUBOS", "env-scope");
    xlings::platform::set_env_variable("XLINGS_PROJECT_DIR", "");
    xlings::platform::set_env_variable(
        "XDG_CONFIG_HOME", (root / "config").string());
    xlings::platform::set_env_variable(
        "XDG_CACHE_HOME", (root / "cache").string());
    xlings::platform::set_env_variable(
        "XDG_DATA_HOME", (root / "data").string());
    xlings::platform::set_env_variable("TMPDIR", temp.string());
    fs::current_path(project);

#ifdef _WIN32
    constexpr std::string_view executableExtension = ".exe";
#else
    constexpr std::string_view executableExtension = "";
#endif
    const auto bootstrap =
        home / "bin"
        / ("xlings" + std::string(executableExtension));
    if (!write_text(bootstrap, "old-bootstrap")) {
        return fail(3, "failed to seed isolated bootstrap");
    }
    fs::permissions(
        bootstrap,
        fs::perms::owner_all
            | fs::perms::group_read
            | fs::perms::group_exec
            | fs::perms::others_read
            | fs::perms::others_exec,
        fs::perm_options::replace);

    const auto write_recipe = [&](
            const fs::path& recipe,
            std::string_view target,
            std::string_view filename = {}) {
        std::ofstream output(recipe);
        output
            << "import(\"xim.libxpkg.pkginfo\")\n"
            << "import(\"xim.libxpkg.xvm\")\n"
            << "function config()\n"
            << "  xvm.add(\"" << target
            << "\", { bindir = path.join("
               "pkginfo.install_dir(), \"bin\")";
        if (!filename.empty()) {
            output << ", filename = \"" << filename << "\"";
        }
        output << " })\n  return true\nend\n";
        return output.good();
    };
    const auto run_config = [&](
            const fs::path& recipe,
            std::string name,
            std::string version,
            const fs::path& installDir,
            bool useAfterInstall) {
        auto executor = mcpplibs::xpkg::create_executor(recipe);
        if (!executor) {
            std::cerr << executor.error() << '\n';
            return false;
        }
        xlings::xim::PlanNode node;
        node.name = std::move(name);
        node.canonicalName = node.name;
        node.version = std::move(version);
        mcpplibs::xpkg::ExecutionContext context;
        context.pkg_name = node.name;
        context.version = node.version;
        context.platform = std::string(xlings::platform::OS_NAME);
        context.arch = "x86_64";
        context.install_file = recipe;
        context.install_dir = installDir;
        context.run_dir = installDir;
        context.xpkg_dir = root / "data";
        xlings::xim::detail_::
            configure_xpkg_execution_artifact_paths_(context);
        const auto expectedSubos =
            xlings::Config::xvm_artifact_subos_dir();
        if (!same_dir(context.bin_dir, expectedSubos / "bin")
            || !same_dir(context.subos_sysrootdir, expectedSubos)) {
            std::cerr
                << "execution context does not match write scope\n";
            return false;
        }
        return xlings::xim::detail_::run_config_hook_(
            node,
            root / "data",
            *executor,
            context,
            {},
            useAfterInstall);
    };

    if (!xlings::Config::has_project_config()) {
        return fail(4, "temporary project was not selected");
    }
    if (!same_dir(xlings::Config::global_subos_dir(), globalSubos)) {
        return fail(
            5,
            "global subos root ignored XLINGS_ACTIVE_SUBOS");
    }
    if (!same_dir(xlings::Config::xvm_artifact_subos_dir(), projectSubos)) {
        return fail(
            6,
            "project metadata and artifact roots disagree");
    }

    const auto projectRecipe = root / "project-provider.lua";
    if (!write_recipe(projectRecipe, "task3b-project-tool")) {
        return fail(7, "failed to write project recipe");
    }
    if (!run_config(
            projectRecipe,
            "task3b-project-provider",
            "1.0.0",
            payload / "project",
            true)) {
        return fail(8, "project-scope config hook failed");
    }
    if (!xlings::Config::project_versions().contains(
            "task3b-project-tool")
        || !xlings::Config::workspace_mut().contains(
            "task3b-project-tool")
        || !xlings::Config::workspace_installed_mut().contains(
            "task3b-project-tool")) {
        return fail(
            9,
            "project registration did not use project metadata");
    }
    // Program shims are named after the target plus the platform's
    // executable suffix, so a bare name only exists on POSIX.
    const auto shim_name = [](std::string_view target) {
#if defined(_WIN32)
        return std::string(target) + ".exe";
#else
        return std::string(target);
#endif
    };
    if (!fs::exists(
            projectSubos / "bin" / shim_name("task3b-project-tool"))) {
        return fail(
            10,
            "project registration did not use project artifact root");
    }

    xlings::Config::set_force_global_scope(true);
    if (!same_dir(xlings::Config::xvm_artifact_subos_dir(), globalSubos)) {
        return fail(
            11,
            "force-global metadata and artifact roots disagree");
    }
    const auto selfRecipe = root / "self-provider.lua";
    if (!write_recipe(
            selfRecipe, "xlings", "xlings-real")) {
        return fail(12, "failed to write self-replace recipe");
    }
    const auto selfPayload = payload / "self";
    const auto selfSource =
        selfPayload / "bin"
        / ("xlings-real"
           + std::string(executableExtension));
    if (!write_text(selfSource, "new-bootstrap")) {
        return fail(13, "failed to create custom self source");
    }
    fs::permissions(
        selfSource,
        fs::perms::owner_all
            | fs::perms::group_read
            | fs::perms::group_exec
            | fs::perms::others_read
            | fs::perms::others_exec,
        fs::perm_options::replace);
    if (!run_config(
            selfRecipe,
            "task3b-self-provider",
            "2.0.0",
            selfPayload,
            true)) {
        return fail(14, "force-global self config hook failed");
    }
    if (!xlings::Config::global_versions().contains("xlings")
        || xlings::Config::project_versions().contains("xlings")) {
        return fail(
            15,
            "force-global registration did not use global metadata");
    }
    if (!fs::exists(
            globalSubos / "bin"
            / ("xlings" + std::string(executableExtension)))
        || fs::exists(
            projectSubos / "bin"
            / ("xlings" + std::string(executableExtension)))) {
        return fail(
            16,
            "force-global shim was written to the wrong artifact root");
    }
    {
        std::ifstream input(bootstrap);
        std::string contents;
        input >> contents;
        if (contents != "new-bootstrap") {
            return fail(
                17,
                "self-replace ignored final exact sourceName");
        }
    }

    xlings::Config::set_force_global_scope(false);
    auto& db = xlings::Config::versions_mut();
    auto& workspace = xlings::Config::workspace_mut();
    auto& installed =
        xlings::Config::workspace_installed_mut();
    constexpr std::string_view conflictTarget =
        "task3b-conflict-tool";
    constexpr std::string_view conflictVersion = "3.0.0";
    constexpr std::string_view removedTarget =
        "task3b-removal-sentinel";
    constexpr std::string_view removedVersion = "0.1.0";
    auto& removed =
        db[std::string(removedTarget)]
            .versions[std::string(removedVersion)];
    removed.path = "/old/payload";
    removed.kind = "program";
    removed.sourceName = std::string(removedTarget);
    removed.destinationName = std::string(removedTarget);
    workspace[std::string(removedTarget)] =
        std::string(removedVersion);
    installed[std::string(removedTarget)] = {
        std::string(removedVersion),
    };
    auto& conflict =
        db[std::string(conflictTarget)]
            .versions[std::string(conflictVersion)];
    conflict.path = "/other/payload";
    conflict.kind = "program";
    conflict.sourceName = "other-tool";
    conflict.destinationName = std::string(conflictTarget);
    conflict.bindingGroup = xlings::xvm::BindingGroupRef{
        .provider = "other-provider",
        .providerVersion = "9.0.0",
        .group = std::string(conflictTarget),
        .rootTarget = std::string(conflictTarget),
        .rootVersion = std::string(conflictVersion),
    };
    conflict.bindingMembers = {
        {std::string(conflictTarget),
         std::string(conflictVersion)},
    };
    conflict.bindingMembersDeclared = true;
    workspace[std::string(conflictTarget)] =
        std::string(conflictVersion);
    installed[std::string(conflictTarget)] = {
        std::string(conflictVersion),
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const auto conflictShim =
        projectSubos / "bin"
        / (std::string(conflictTarget)
           + std::string(executableExtension));
    const auto removalShim =
        projectSubos / "bin"
        / (std::string(removedTarget)
           + std::string(executableExtension));
    if (fs::exists(conflictShim)) {
        return fail(18, "conflict shim unexpectedly pre-exists");
    }
    if (!write_text(removalShim, "keep-removal-shim")) {
        return fail(19, "failed to seed exact removal artifact");
    }
    const auto conflictRecipe = root / "conflict-provider.lua";
    {
        std::ofstream output(conflictRecipe);
        output
            << "import(\"xim.libxpkg.pkginfo\")\n"
            << "import(\"xim.libxpkg.xvm\")\n"
            << "function config()\n"
            << "  xvm.remove(\"" << removedTarget
            << "\", \"" << removedVersion << "\")\n"
            << "  xvm.add(\"" << conflictTarget
            << "\", { bindir = path.join("
               "pkginfo.install_dir(), \"bin\") })\n"
            << "  return true\nend\n";
        if (!output.good()) {
            return fail(20, "failed to write conflict recipe");
        }
    }
    if (run_config(
            conflictRecipe,
            "task3b-conflict-provider",
            std::string(conflictVersion),
            payload / "conflict",
            true)) {
        return fail(
            21,
            "production config hook swallowed registration failure");
    }
    if (xlings::xvm::versions_to_json(db) != dbBefore
        || workspace != workspaceBefore
        || installed != installedBefore) {
        return fail(
            22,
            "late registration conflict mutated scoped metadata");
    }
    if (fs::exists(conflictShim)) {
        return fail(
            23,
            "late registration conflict ran its exact shim effect");
    }
    {
        std::ifstream input(removalShim);
        std::string contents;
        input >> contents;
        if (contents != "keep-removal-shim") {
            return fail(
                24,
                "late conflict ran candidate removal cleanup");
        }
    }

    return 0;
}

}  // namespace

TEST(XimXvmProductionPathTest,
     UsesMatchingScopedArtifactsAndSuppressesFailedEffects) {
    namespace fs = std::filesystem;
    const auto root =
        fs::temp_directory_path()
        / std::format(
            "xlings-task3b-production-{}",
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
    fs::remove_all(root);
    fs::create_directories(root);
    const auto executable =
        xlings::platform::get_executable_path();
    ASSERT_FALSE(executable.empty());
    auto command = std::format(
        "{} --xvm-registration-production-child {}",
        xlings::platform::shell_quote(executable.string()),
        xlings::platform::shell_quote(root.string()));
#ifdef _WIN32
    command = "\"" + command + "\"";
#endif
    auto child = xlings::platform::spawn_command(command);
    ASSERT_GT(child.pid, 0);
    auto [status, output] = xlings::platform::wait_or_kill(
        child, nullptr, std::chrono::seconds{30});
    EXPECT_EQ(status, 0) << output;
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(XimXvmRemovalAdapterTest,
     PreSnapshotsUninstallSelectionForLaterVersionlessHookOps) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:15.1.0"}},
        {"cc", {"repo-a:gcc-15.1.0"}},
    };

    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, {}, "repo-a:toolchain", "15.1.0",
        "toolchain", "repo-a:15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    const std::vector<mcpplibs::xpkg::XvmOp> hookOperations{
        {
            .op = "remove",
            .name = "cc",
        },
    };

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, hookOperations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:gcc-15.1.0");
    EXPECT_EQ(result->removed[1].target, "toolchain");
    EXPECT_EQ(result->removed[1].version, "repo-a:15.1.0");
    EXPECT_FALSE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("cc"));
}

TEST(XimXvmRemovalAdapterTest,
     AuthoritativePurgeRemovesSnapshotWhenHookHasNoRemoveOps) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:15.1.0"}},
        {"cc", {"repo-a:gcc-15.1.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> hookOperations;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, hookOperations,
        "repo-a:toolchain", "15.1.0",
        "toolchain", "repo-a:15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, hookOperations, *context,
        xlings::xvm::RemovalBatchOptions{
            .purgeSelection = true,
        });

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->removed.size(), 2u);
    EXPECT_FALSE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("cc"));
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());
}

TEST(XimXvmRemovalAdapterTest,
     OperationsOnlyKeepsProviderSnapshotWhenConfigHookHasNoRemoveOps) {
    xlings::xvm::VersionDB db;
    const auto group = make_binding_group_ref(
        "repo-a:toolchain", "15.1.0", "compiler",
        "toolchain", "repo-a:15.1.0");
    auto& root = add_provider_group_member(
        db, "toolchain", "repo-a:15.1.0", group, "group");
    root.bindingMembers = {
        {"cc", "repo-a:gcc-15.1.0"},
        {"toolchain", "repo-a:15.1.0"},
    };
    add_provider_group_member(
        db, "cc", "repo-a:gcc-15.1.0", group,
        "program", "cc-15", "cc");
    xlings::xvm::Workspace workspace{
        {"toolchain", "repo-a:15.1.0"},
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"toolchain", {"repo-a:15.1.0"}},
        {"cc", {"repo-a:gcc-15.1.0"}},
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;
    const std::vector<mcpplibs::xpkg::XvmOp> configOperations;
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, configOperations,
        "repo-a:toolchain", "15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    ASSERT_TRUE(context->hasSelection);

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, configOperations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->removed.empty());
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmRemovalAdapterTest,
     PreSnapshotsConfigSelectionFromActiveRemovalTarget) {
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, "toolchain", "15.1.0", "/pkg/toolchain",
        "program", "", "", "repo-a");
    xlings::xvm::add_version(
        db, "cc", "gcc-15.1.0", "/pkg/toolchain",
        "program", "cc-15", "cc", "repo-a",
        "toolchain@15.1.0");
    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:gcc-15.1.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-a:gcc-15.1.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = "cc",
        },
    };

    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations, "repo-a:toolchain", "15.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 2u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:gcc-15.1.0");
    EXPECT_EQ(result->removed[1].target, "toolchain");
    EXPECT_EQ(result->removed[1].version, "repo-a:15.1.0");
    EXPECT_FALSE(db.contains("toolchain"));
    EXPECT_FALSE(db.contains("cc"));
    EXPECT_TRUE(workspace.empty());
    EXPECT_TRUE(installed.empty());

    xlings::xvm::VersionDB singletonDb;
    singletonDb["base"].versions["repo-a:1.0.0"].kind = "subos-base";
    xlings::xvm::Workspace singletonWorkspace{
        {"base", "repo-a:1.0.0"},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> singletonOperations{
        {
            .op = "remove",
            .name = "base",
        },
    };
    auto singletonContext = xlings::xim::snapshot_xpkg_removal_context(
        singletonDb, singletonWorkspace, singletonOperations,
        "repo-a:base", "1.0.0");

    ASSERT_TRUE(singletonContext.has_value())
        << singletonContext.error().message;
    EXPECT_EQ(
        singletonContext->members.at("base"),
        "repo-a:1.0.0");
}

TEST(XimXvmRemovalAdapterTest,
     LegacyPreferredSelectionCannotAuthorizeCanonicalRemoveAll) {
    xlings::xvm::VersionDB db;
    auto& legacy = db["cc"].versions["legacy:0.9.0"];
    legacy.path = "/pkg/legacy";
    legacy.kind = "program";
    legacy.sourceName = "cc-legacy";
    legacy.destinationName = "cc";

    const auto canonicalGroup = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "compiler",
        "cc", "repo-a:1.0.0");
    auto& canonical = add_provider_group_member(
        db, "cc", "repo-a:1.0.0", canonicalGroup, "group");
    canonical.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
    };

    xlings::xvm::Workspace workspace{
        {"cc", "legacy:0.9.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"legacy:0.9.0", "repo-a:1.0.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove_all",
            .name = "cc",
        },
    };
    const auto dbBefore = xlings::xvm::versions_to_json(db);
    const auto workspaceBefore = workspace;
    const auto installedBefore = installed;

    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        "repo-a:provider", "1.0.0",
        "cc", "legacy:0.9.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_TRUE(context->provider.empty());
    EXPECT_EQ(context->members.at("cc"), "legacy:0.9.0");

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, *context);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(
        result.error().kind,
        xlings::xvm::RemovalErrorKind::ProviderRequired);
    EXPECT_EQ(xlings::xvm::versions_to_json(db), dbBefore);
    EXPECT_EQ(workspace, workspaceBefore);
    EXPECT_EQ(installed, installedBefore);
}

TEST(XimXvmRemovalAdapterTest, TransportsRemoveAllAsDistinctOperation) {
    xlings::xvm::VersionDB db;
    const auto groupA = make_binding_group_ref(
        "repo-a:provider", "1.0.0", "group-a",
        "cc", "repo-a:1.0.0");
    auto& providerA = add_provider_group_member(
        db, "cc", "repo-a:1.0.0", groupA, "group");
    providerA.bindingMembers = {
        {"cc", "repo-a:1.0.0"},
    };
    const auto groupB = make_binding_group_ref(
        "repo-b:provider", "2.0.0", "group-b",
        "cc", "repo-b:2.0.0");
    auto& providerB = add_provider_group_member(
        db, "cc", "repo-b:2.0.0", groupB, "group");
    providerB.bindingMembers = {
        {"cc", "repo-b:2.0.0"},
    };
    xlings::xvm::Workspace workspace{
        {"cc", "repo-a:1.0.0"},
    };
    xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"repo-b:2.0.0", "repo-a:1.0.0"}},
    };
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove_all",
            .name = "cc",
        },
    };
    auto context = xlings::xim::snapshot_xpkg_removal_context(
        db, workspace, operations,
        "repo-a:provider", "1.0.0",
        "cc", "repo-a:1.0.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_EQ(context->provider, "repo-a:provider");

    auto result = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, *context);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->removed.size(), 1u);
    EXPECT_EQ(result->removed[0].target, "cc");
    EXPECT_EQ(result->removed[0].version, "repo-a:1.0.0");
    ASSERT_TRUE(db.contains("cc"));
    EXPECT_EQ(db.at("cc").versions.size(), 1u);
    EXPECT_TRUE(db.at("cc").versions.contains("repo-b:2.0.0"));
    EXPECT_EQ(workspace.at("cc"), "repo-b:2.0.0");
}

TEST(XimXvmRemovalArtifactTest,
     RemovesExactLibraryDestinationBeforeReplacement) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-artifact-{}", nonce);
    const auto binDir = testRoot / "bin";
    const auto libDir = testRoot / "lib";
    const auto oldPayload = testRoot / "old";
    const auto newPayload = testRoot / "new";
    const auto unrelatedPayload = testRoot / "unrelated";
    fs::create_directories(binDir);
    fs::create_directories(libDir);
    fs::create_directories(oldPayload);
    fs::create_directories(newPayload);
    fs::create_directories(unrelatedPayload);

    const auto target = std::string{"compiler-runtime"};
    const auto destinationName = std::string{"libcompiler.so"};
    const auto oldSource = oldPayload / destinationName;
    const auto newSource = newPayload / destinationName;
    const auto unrelatedSource = unrelatedPayload / target;
    xlings::platform::write_string_to_file(
        oldSource.string(), "OLD");
    xlings::platform::write_string_to_file(
        newSource.string(), "NEW");
    xlings::platform::write_string_to_file(
        unrelatedSource.string(), "SENTINEL");
    xlings::xvm::place_library(
        (oldPayload / destinationName).string(), destinationName, libDir);
    xlings::xvm::place_library(
        (unrelatedPayload / target).string(), target, libDir);

    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(
        db, target, "repo-a:1.0.0", oldPayload.string(),
        "lib", destinationName);
    const auto dbBefore = db;
    ASSERT_EQ(
        dbBefore.at(target)
            .versions.at("repo-a:1.0.0").destinationName,
        destinationName);
    xlings::xvm::Workspace workspace;
    xlings::xvm::WorkspaceInstalled installed;
    const std::vector<mcpplibs::xpkg::XvmOp> operations{
        {
            .op = "remove",
            .name = target,
            .version = "repo-a:1.0.0",
        },
    };
    auto removalResult = xlings::xim::apply_xpkg_removal_operations(
        db, workspace, installed, operations, {});
    ASSERT_TRUE(removalResult.has_value())
        << removalResult.error().message;

    xlings::xim::cleanup_removed_xvm_library_artifacts(
        libDir, dbBefore, db, *removalResult);

    EXPECT_FALSE(fs::exists(libDir / destinationName));
    ASSERT_TRUE(fs::exists(libDir / target));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / target).string()),
        "SENTINEL");

    xlings::xvm::add_version(
        db, target, "repo-a:2.0.0", newPayload.string(),
        "lib", destinationName);
    xlings::xvm::place_library(
        (newPayload / destinationName).string(), destinationName, libDir);
    ASSERT_TRUE(fs::exists(libDir / destinationName));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / destinationName).string()),
        "NEW");
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / target).string()),
        "SENTINEL");

    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     UsesLegacyLibraryFilenameWhenVersionDestinationIsMissing) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-legacy-lib-{}", nonce);
    const auto sourceDir = testRoot / "source";
    const auto libDir = testRoot / "lib";
    const auto filename = std::string{"liblegacy.so"};
    fs::create_directories(sourceDir);
    xlings::platform::write_string_to_file(
        (sourceDir / filename).string(), "LEGACY");
    xlings::xvm::place_library(
        (sourceDir / filename).string(), filename, libDir);

    xlings::xvm::VersionDB dbBefore;
    auto& info = dbBefore["legacy-runtime"];
    info.type = "lib";
    info.filename = filename;
    auto& version = info.versions["1.0.0"];
    version.path = sourceDir.string();
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = "legacy-runtime",
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_library_artifacts(
        libDir, dbBefore, {}, removalResult);

    EXPECT_FALSE(fs::exists(libDir / filename));
    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     KeepsDestinationOwnedBySurvivingLibraryVersion) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-surviving-lib-{}", nonce);
    const auto sourceDir = testRoot / "source";
    const auto libDir = testRoot / "lib";
    const auto target = std::string{"compiler-runtime"};
    const auto filename = std::string{"libcompiler.so"};
    fs::create_directories(sourceDir);
    xlings::platform::write_string_to_file(
        (sourceDir / filename).string(), "SURVIVOR");
    xlings::xvm::place_library(
        (sourceDir / filename).string(), filename, libDir);

    xlings::xvm::VersionDB dbBefore;
    xlings::xvm::add_version(
        dbBefore, target, "1.0.0", "/old",
        "lib", filename);
    xlings::xvm::add_version(
        dbBefore, target, "2.0.0", sourceDir.string(),
        "lib", filename);
    auto currentDb = dbBefore;
    currentDb.at(target).versions.erase("1.0.0");
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = target,
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_library_artifacts(
        libDir, dbBefore, currentDb, removalResult);

    ASSERT_TRUE(fs::exists(libDir / filename));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(
            (libDir / filename).string()),
        "SURVIVOR");
    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     KeepsProgramShimWhenUsableVersionSurvives) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-surviving-shim-{}", nonce);
    const auto binDir = testRoot / "bin";
    fs::create_directories(binDir);
#ifdef _WIN32
    const auto shim = binDir / "cc.exe";
#else
    const auto shim = binDir / "cc";
#endif
    xlings::platform::write_string_to_file(shim.string(), "SHIM");

    xlings::xvm::VersionDB dbBefore;
    xlings::xvm::add_version(
        dbBefore, "cc", "1.0.0", "/old", "program", "cc");
    xlings::xvm::add_version(
        dbBefore, "cc", "2.0.0", "/new", "program", "cc");
    auto currentDb = dbBefore;
    currentDb.at("cc").versions.erase("1.0.0");
    const xlings::xvm::WorkspaceInstalled installed{
        {"cc", {"2.0.0"}},
    };
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = "cc",
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_program_artifacts(
        binDir, dbBefore, currentDb, installed, removalResult);

    EXPECT_TRUE(fs::exists(shim));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(shim.string()),
        "SHIM");
    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XimXvmRemovalArtifactTest,
     VirtualGroupNeverOwnsSameNamedProgramShim) {
    namespace fs = std::filesystem;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto testRoot = fs::temp_directory_path()
        / std::format("xlings-xvm-removal-group-shim-{}", nonce);
    const auto binDir = testRoot / "bin";
    fs::create_directories(binDir);
#ifdef _WIN32
    const auto shim = binDir / "toolchain.exe";
#else
    const auto shim = binDir / "toolchain";
#endif
    xlings::platform::write_string_to_file(shim.string(), "UNRELATED");

    xlings::xvm::VersionDB dbBefore;
    auto& info = dbBefore["toolchain"];
    info.type = "program";
    auto& version = info.versions["1.0.0"];
    version.kind = "group";
    const xlings::xvm::RemovalBatchResult removalResult{
        .removed = {
            {
                .target = "toolchain",
                .version = "1.0.0",
            },
        },
    };

    xlings::xim::cleanup_removed_xvm_program_artifacts(
        binDir, dbBefore, {}, {}, removalResult);

    EXPECT_TRUE(fs::exists(shim));
    EXPECT_EQ(
        xlings::platform::read_file_to_string(shim.string()),
        "UNRELATED");
    std::error_code ec;
    fs::remove_all(testRoot, ec);
}

TEST(XvmDbTest, AddVersionWithBindingNamespaced) {
    xlings::xvm::VersionDB db;

    // Simulate a non-primary repo with namespace
    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15", "program", "", "", "xim");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "xim", "xim-gnu-gcc@15.1.0");

    // Verify namespaced version keys in bindings
    auto* parent = xlings::xvm::get_vinfo(db, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    ASSERT_TRUE(parent->bindings.contains("gcc"));
    EXPECT_EQ(parent->bindings.at("gcc").at("xim:15.1.0"), "xim:15.1.0");

    auto* gcc_info = xlings::xvm::get_vinfo(db, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("xim:15.1.0"), "xim:15.1.0");
}

TEST(XvmDbTest, BindingTreeTraversal) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "gcc-ar", "gcc-15.1.0", "/pkg/gcc-15", "program", "gcc-ar", "gcc-ar", "", "xim-gnu-gcc@15.1.0");

    auto selection =
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0");

    ASSERT_TRUE(selection.has_value()) << selection.error().message;
    EXPECT_EQ(selection->source, xlings::xvm::BindingSource::LegacyGraph);
    EXPECT_EQ(selection->members,
              (std::map<std::string, std::string>{
                  {"g++", "15.1.0"},
                  {"gcc", "15.1.0"},
                  {"gcc-ar", "gcc-15.1.0"},
                  {"xim-gnu-gcc", "15.1.0"},
              }));
}

TEST(XvmDbTest, BindingJsonRoundTrip) {
    xlings::xvm::VersionDB db;

    xlings::xvm::add_version(db, "xim-gnu-gcc", "15.1.0", "/pkg/gcc-15");
    xlings::xvm::add_version(db, "gcc", "15.1.0", "/pkg/gcc-15", "program", "gcc", "gcc", "", "xim-gnu-gcc@15.1.0");
    xlings::xvm::add_version(db, "g++", "15.1.0", "/pkg/gcc-15", "program", "g++", "g++", "", "xim-gnu-gcc@15.1.0");

    // Serialize and deserialize
    auto j = xlings::xvm::versions_to_json(db);
    auto restored = xlings::xvm::versions_from_json(j);

    // Verify bindings survived round-trip
    auto* parent = xlings::xvm::get_vinfo(restored, "xim-gnu-gcc");
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->bindings.at("gcc").at("15.1.0"), "15.1.0");
    EXPECT_EQ(parent->bindings.at("g++").at("15.1.0"), "15.1.0");

    auto* gcc_info = xlings::xvm::get_vinfo(restored, "gcc");
    ASSERT_NE(gcc_info, nullptr);
    EXPECT_EQ(gcc_info->bindings.at("xim-gnu-gcc").at("15.1.0"), "15.1.0");
}


// ============================================================
// A declared file asset must resolve as an effect
//
// resolve_xpkg_filesystem_effect compared the entry's kind against exactly
// two spellings -- "program" for a shim, "lib" for everything else -- so a
// FileAsset effect was matched against "lib" and never resolved. Every
// package declaring one logged "validated xvm effect target disappeared or
// changed kind" on a perfectly correct install, and the FileAsset branch of
// the caller, including its active gate, was unreachable.
//
// Only a real recipe declaring an asset surfaced it: the assets still land,
// because the activation pass places them by a different route.
// ============================================================

TEST(XimXvmFileAssetEffect, ResolvesInsteadOfWarningAboutAChangedKind) {
    xlings::xvm::VersionDB db;
    auto& info = db["demo.files.1"];
    info.type = "files";
    auto& data = info.versions["1.0.0"];
    data.kind = "files";
    data.path = "/pkg/demo/1.0.0";
    data.fileSrc = "include/demo.h";
    data.fileDst = "usr/include/demo.h";
    const xlings::xvm::Workspace workspace{{"demo.files.1", "1.0.0"}};

    const xlings::xim::XpkgFilesystemEffect effect{
        .kind = xlings::xim::XpkgFilesystemEffectKind::FileAsset,
        .target = "demo.files.1",
        .version = "1.0.0",
    };

    auto resolved =
        xlings::xim::resolve_xpkg_filesystem_effect(db, workspace, effect);
    ASSERT_TRUE(resolved.has_value())
        << "a declared file asset did not resolve -- this is the warning on "
           "an install that is actually fine";
    EXPECT_EQ(resolved->kind,
              xlings::xim::XpkgFilesystemEffectKind::FileAsset);
    EXPECT_EQ(resolved->target, "demo.files.1");
    // The active gate is what the caller reads to decide whether to place
    // the asset; it was unreachable while this resolved to nullopt.
    EXPECT_TRUE(resolved->active);
}

TEST(XimXvmFileAssetEffect, StillRefusesWhenTheEntryIsNotAFileAsset) {
    // The kind check has to keep rejecting a mismatch; widening it to three
    // spellings must not turn it into "anything goes".
    xlings::xvm::VersionDB db;
    db["demo.files.1"].type = "program";
    db["demo.files.1"].versions["1.0.0"].kind = "program";
    db["demo.files.1"].versions["1.0.0"].path = "/pkg/demo/1.0.0/bin";

    const xlings::xim::XpkgFilesystemEffect effect{
        .kind = xlings::xim::XpkgFilesystemEffectKind::FileAsset,
        .target = "demo.files.1",
        .version = "1.0.0",
    };
    EXPECT_FALSE(
        xlings::xim::resolve_xpkg_filesystem_effect(db, {}, effect)
            .has_value());
}

// ============================================================
// A dangling edge must not make a package unremovable
//
// Reported from the field: on a real installation `use gcc 15` refused with
// xvm-binding-version-missing, and `remove gcc 15` refused with the same
// error from the removal path -- so the user could neither switch nor take
// the package out. A dead end with no command that leads out of it.
//
// Removal does not need the release to resolve. Taking something out needs
// no understanding of what put it in, which is the same reason removal sits
// outside the xpackage spec gate.
// ============================================================

TEST(XvmRemovalDangling, RemovalFallsBackToTheNamedEntry) {
    // gcc@15.1.0 bound to an anchor registered only at 16.1.0 -- the shape
    // found on a real machine.
    xlings::xvm::VersionDB db;
    db["gcc"].type = "program";
    db["gcc"].versions["15.1.0"].path = "/pkg/gcc/15.1.0/bin";
    db["gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0/bin";
    db["xim-gnu-gcc"].type = "program";
    db["xim-gnu-gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0";
    db["gcc"].bindings["xim-gnu-gcc"]["15.1.0"] = "15.1.0";   // dangling

    // Precondition: the release genuinely does not resolve, so the fallback
    // is what is under test rather than a happy path.
    ASSERT_FALSE(
        xlings::xvm::resolve_binding_selection(db, "gcc", "15.1.0")
            .has_value());

    auto context =
        xlings::xvm::snapshot_removal_context(db, "gcc", "15.1.0");
    ASSERT_TRUE(context.has_value())
        << "removal refused, leaving the package unremovable: "
        << context.error().message;
    ASSERT_EQ(context->members.size(), 1u);
    EXPECT_EQ(context->members.at("gcc"), "15.1.0");
}

TEST(XvmRemovalDangling, AResolvableReleaseStillRemovesEveryMember) {
    // The fallback must not swallow the normal case: when the release does
    // resolve, removal still takes the whole thing.
    xlings::xvm::VersionDB db;
    db["gcc"].type = "program";
    db["gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0/bin";
    db["xim-gnu-gcc"].type = "program";
    db["xim-gnu-gcc"].versions["16.1.0"].path = "/pkg/gcc/16.1.0";
    db["gcc"].bindings["xim-gnu-gcc"]["16.1.0"] = "16.1.0";
    db["xim-gnu-gcc"].bindings["gcc"]["16.1.0"] = "16.1.0";

    auto context =
        xlings::xvm::snapshot_removal_context(db, "gcc", "16.1.0");
    ASSERT_TRUE(context.has_value()) << context.error().message;
    EXPECT_EQ(context->members.size(), 2u);
    EXPECT_EQ(context->members.at("xim-gnu-gcc"), "16.1.0");
}

// ============================================================

// The production-path test re-executes this binary; the child mode has to
// live in the same translation unit as the function it calls.
#ifndef XLINGS_USE_GTEST_MAIN
int main(int argc, char** argv) {
    if (argc == 3
        && std::string_view(argv[1])
            == "--xvm-registration-production-child") {
        return run_xvm_registration_production_child_(argv[2]);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
