#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.libs.json;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.materialize;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace m = xlings::xvm::materialize;

namespace {
void derived(const fs::path& source, const fs::path& destination) {
    if constexpr (xlings::platform::is_windows) {
        if (fs::is_directory(source)) ASSERT_TRUE(xlings::platform::create_directory_link(destination.string(), source.string()));
        else fs::create_hard_link(source, destination);
    } else fs::create_symlink(source, destination);
}
}

XTEST(XvmMaterialize, RecordedHeaderClaimsSurviveMissingSourcesButIncomingAssetsRequireThem,
      .area = "xvm", .covers = {"HOME-LAYER-RESOLVE"}) {
    auto home = tk::Home::isolated("asset-missing-source");
    const auto scope = home.dir() / "subos/default";
    const auto include = home.dir() / "data/xpkgs/demo/1/include";
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "tool", "1", include.parent_path().string());
    db["tool"].versions["1"].includedir = include.string();
    const xlings::xvm::WorkspaceInstalled installed{{"tool", {"1"}}};
    const auto recorded = m::collect_claims(db, installed, scope, scope / "lib", home.dir().string());
    ASSERT_TRUE(recorded) << recorded.error();
    ASSERT_EQ(recorded->size(), 1);
    EXPECT_EQ(recorded->front().source, include);
    EXPECT_TRUE(recorded->front().descendants);
    EXPECT_FALSE(m::collect_claims(db, installed, scope, scope / "lib", home.dir().string(),
        m::ClaimSource::Present));
}

XTEST(XvmMaterialize, ReplacedPayloadRepairsProvenDanglingHeadersAndPreservesUnknownSiblings,
      .area = "xvm", .covers = {"HOME-LAYER-RESOLVE"}) {
    if constexpr (xlings::platform::is_windows) GTEST_SKIP() << "deleted sources cannot prove Windows hardlink ownership";
    auto home = tk::Home::isolated("asset-dangling-header");
    const auto scope = home.dir() / "subos/default";
    const auto old = home.dir() / "data/xpkgs/demo/1/gen1/include";
    const auto fresh = home.dir() / "data/xpkgs/demo/1/gen2/include";
    const auto destination = scope / "usr/include";
    tk::write_file(old / "owned.h", "old");
    tk::write_file(old / "obsolete.h", "obsolete");
    tk::write_file(fresh / "owned.h", "fresh");
    fs::create_directories(destination);
    derived(old / "owned.h", destination / "owned.h");
    derived(old / "obsolete.h", destination / "obsolete.h");
    tk::write_file(destination / "user.h", "user header");
    fs::remove_all(old);
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "tool", "1", old.parent_path().string());
    db["tool"].versions["1"].includedir = old.string();
    auto recorded = m::collect_claims(db, {{"tool", {"1"}}}, scope, scope / "lib", home.dir().string());
    ASSERT_TRUE(recorded) << recorded.error();
    const auto& claims = *recorded;
    const std::vector<m::AssetClaim> desired{{fresh / "owned.h", destination / "owned.h"}};
    auto obsolete = m::obsolete_assets(claims, desired);
    ASSERT_TRUE(obsolete) << obsolete.error();
    ASSERT_EQ(obsolete->size(), 1);
    EXPECT_EQ(obsolete->front().destination, destination / "obsolete.h");
    auto changes = std::move(*obsolete);
    changes.push_back({desired.front().source, desired.front().destination});
    auto prepared = m::preflight_materialization(changes, claims, scope);
    ASSERT_TRUE(prepared) << prepared.error();
    auto applied = prepared->execute();
    ASSERT_TRUE(applied) << applied.error();
    ASSERT_TRUE(applied->commit());
    EXPECT_EQ(tk::read_file(destination / "owned.h"), "fresh");
    EXPECT_EQ(tk::read_file(destination / "user.h"), "user header");
    EXPECT_FALSE(fs::is_symlink(destination / "obsolete.h"));
}

XTEST(XvmMaterialize, PreflightPreservesUnknownFilesDirectoriesAndLinks, .area = "xvm",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto home = tk::Home::isolated("asset-user-data");
    const auto scope = home.dir() / "subos/default";
    const auto source = home.dir() / "data/xpkgs/demo/1/include/owned.h";
    tk::write_file(source, "incoming");
    const auto destination = scope / "usr/include/owned.h";
    tk::write_file(destination, "user file");
    const std::vector<m::AssetChange> changes{{source, destination}};
    EXPECT_FALSE(m::preflight_materialization(changes, {}, scope));
    EXPECT_EQ(tk::read_file(destination), "user file");
    fs::remove(destination);
    tk::write_file(destination / "keep", "user directory");
    EXPECT_FALSE(m::preflight_materialization(changes, {}, scope));
    EXPECT_EQ(tk::read_file(destination / "keep"), "user directory");
    fs::remove(destination / "keep");
    fs::remove(destination);
    derived(source, destination);
    const auto other = home.dir() / "data/xpkgs/demo/2/include/owned.h";
    tk::write_file(other, "replacement");
    EXPECT_FALSE(m::preflight_materialization(std::vector<m::AssetChange>{{other, destination}}, {}, scope));
    EXPECT_EQ(tk::read_file(destination), "incoming");
}

XTEST(XvmMaterialize, ProvenReplacementCanRollbackAndCommitWithoutFixedStaging, .area = "xvm",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto home = tk::Home::isolated("asset-replace");
    const auto scope = home.dir() / "subos/default";
    const auto old = home.dir() / "data/xpkgs/demo/1/file";
    const auto fresh = home.dir() / "data/xpkgs/demo/2/file";
    const auto destination = scope / "usr/share/demo/file";
    tk::write_file(old, "old");
    tk::write_file(fresh, "fresh");
    fs::create_directories(destination.parent_path());
    derived(old, destination);
    tk::write_file(destination.string() + ".xlings-new/sentinel", "user staging");
    const std::vector<m::AssetClaim> claims{{old, destination}};
    const std::vector<m::AssetChange> changes{{fresh, destination}};
    auto prepared = m::preflight_materialization(changes, claims, scope);
    ASSERT_TRUE(prepared) << prepared.error();
    auto applied = prepared->execute();
    ASSERT_TRUE(applied) << applied.error();
    EXPECT_EQ(tk::read_file(destination), "fresh");
    ASSERT_EQ(applied->proofs().size(), 1);
    ASSERT_TRUE(applied->rollback());
    EXPECT_EQ(tk::read_file(destination), "old");
    prepared = m::preflight_materialization(changes, claims, scope);
    ASSERT_TRUE(prepared);
    applied = prepared->execute();
    ASSERT_TRUE(applied) << applied.error();
    ASSERT_TRUE(applied->commit());
    EXPECT_EQ(tk::read_file(destination), "fresh");
    EXPECT_EQ(tk::read_file(destination.string() + ".xlings-new/sentinel"), "user staging");
    EXPECT_EQ(tk::read_file(old), "old");
}

XTEST(XvmMaterialize, PublicationConflictRestoresEarlierChanges, .area = "xvm",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto home = tk::Home::isolated("asset-race");
    const auto scope = home.dir() / "subos/default";
    const auto source = home.dir() / "data/xpkgs/demo/1/file";
    tk::write_file(source, "payload");
    const auto first = scope / "usr/share/a";
    const auto second = scope / "usr/share/b";
    auto prepared = m::preflight_materialization(std::vector<m::AssetChange>{{source, first}, {source, second}}, {}, scope);
    ASSERT_TRUE(prepared) << prepared.error();
    tk::write_file(second, "user appeared after preflight");
    auto applied = prepared->execute();
    ASSERT_FALSE(applied);
    EXPECT_FALSE(fs::exists(first));
    EXPECT_EQ(tk::read_file(second), "user appeared after preflight");
    EXPECT_EQ(tk::read_file(source), "payload");
}

XTEST(XvmMaterialize, DirectoryMergeAndLaterUpdateRequireTheLedger, .area = "xvm",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto home = tk::Home::isolated("asset-directory");
    const auto scope = home.dir() / "subos/default";
    const auto old = home.dir() / "data/xpkgs/demo/1/include/scsi";
    const auto fresh = home.dir() / "data/xpkgs/demo/2/include/scsi";
    const auto arrival = home.dir() / "data/xpkgs/other/1/arrival.h";
    const auto destination = scope / "usr/include/scsi";
    tk::write_file(old / "deep/owned.h", "old");
    tk::write_file(old / "obsolete.h", "obsolete");
    tk::write_file(fresh / "deep/owned.h", "fresh");
    tk::write_file(arrival, "arrival");
    fs::create_directories(destination.parent_path());
    derived(old, destination);
    const std::vector<m::AssetChange> merge{{old, destination}, {arrival, destination / "arrival.h"}};
    EXPECT_FALSE(m::preflight_materialization(merge, {}, scope));
    std::vector<m::AssetClaim> claims{{old, destination, true}};
    auto prepared = m::preflight_materialization(merge, claims, scope);
    ASSERT_TRUE(prepared) << prepared.error();
    auto applied = prepared->execute();
    ASSERT_TRUE(applied) << applied.error();
    ASSERT_TRUE(applied->commit());
    EXPECT_FALSE(fs::is_symlink(destination));
    EXPECT_EQ(tk::read_file(destination / "deep/owned.h"), "old");
    EXPECT_EQ(tk::read_file(destination / "arrival.h"), "arrival");
    EXPECT_FALSE(fs::exists(old / "arrival.h"));
    claims.push_back({arrival, destination / "arrival.h"});
    const std::vector<m::AssetChange> update{{fresh, destination}, {arrival, destination / "arrival.h"}};
    prepared = m::preflight_materialization(update, claims, scope);
    ASSERT_TRUE(prepared) << prepared.error();
    applied = prepared->execute();
    ASSERT_TRUE(applied) << applied.error();
    ASSERT_TRUE(applied->commit());
    EXPECT_EQ(tk::read_file(destination / "deep/owned.h"), "fresh");
    EXPECT_FALSE(fs::exists(destination / "obsolete.h"));
    EXPECT_EQ(tk::read_file(destination / "arrival.h"), "arrival");
    EXPECT_EQ(tk::read_file(old / "obsolete.h"), "obsolete");
    tk::write_file(destination / "user.h", "user");
    claims.front().source = fresh;
    EXPECT_FALSE(m::preflight_materialization(update, claims, scope));
    EXPECT_EQ(tk::read_file(destination / "user.h"), "user");
}

XTEST(XvmMaterialize, AbandonedTransactionRestoresThePreviousFileObject, .area = "xvm",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto home = tk::Home::isolated("asset-raii");
    const auto scope = home.dir() / "subos/default";
    const auto old = home.dir() / "data/xpkgs/demo/1/file";
    const auto fresh = home.dir() / "data/xpkgs/demo/2/file";
    const auto destination = scope / "usr/share/demo";
    tk::write_file(old, "same bytes");
    tk::write_file(fresh, "same bytes");
    fs::create_directories(destination.parent_path());
    derived(old, destination);
    {
        auto prepared = m::preflight_materialization(std::vector<m::AssetChange>{{fresh, destination}},
            std::vector<m::AssetClaim>{{old, destination}}, scope);
        ASSERT_TRUE(prepared);
        auto applied = prepared->execute();
        ASSERT_TRUE(applied) << applied.error();
        EXPECT_TRUE(fs::equivalent(destination, fresh));
    }
    EXPECT_TRUE(fs::equivalent(destination, old));
    fs::remove(destination);
    tk::write_file(destination, "same bytes");
    EXPECT_FALSE(m::preflight_materialization(std::vector<m::AssetChange>{{fresh, destination}},
        std::vector<m::AssetClaim>{{old, destination}}, scope));
    EXPECT_EQ(tk::read_file(destination), "same bytes");
}

XTEST(XvmMaterialize, UseConflictDoesNotPublishMetadataOrDeleteUserHeaders, .area = "xvm",
      .covers = {"HOME-LAYER-RESOLVE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("asset-use-conflict");
    auto init = home.xlings({"self", "init"});
    ASSERT_EQ(init.exit_code, 0) << init.transcript();
    const auto payload = home.dir() / "data/xpkgs/demo-x-tool/1/bin";
    const auto include = payload.parent_path() / "include";
    tk::write_file(payload / "tool", "tool");
    tk::write_file(include / "shared.h", "payload header");
    xlings::xvm::VersionDB db;
    xlings::xvm::add_version(db, "tool", "1", payload.string());
    db["tool"].versions["1"].includedir = include.string();
    auto config = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json"));
    config["versions"] = xlings::xvm::versions_to_json(db);
    tk::write_file(home.dir() / ".xlings.json", config.dump(2));
    const auto scope = home.dir() / "subos/default";
    auto scopeConfig = nlohmann::json::parse(tk::read_file(scope / ".xlings.json"));
    xlings::xvm::SubosWorkspace workspace;
    workspace.installed["tool"] = {"1"};
    scopeConfig["workspace"] = xlings::xvm::subos_workspace_to_json(workspace);
    scopeConfig["future"] = "preserve";
    tk::write_file(scope / ".xlings.json", scopeConfig.dump(2));
    tk::write_file(scope / "usr/include/shared.h", "user header");
    tk::write_file(scope / "usr/include/shared.h.xlings-new/sentinel", "user staging");
    auto used = home.xlings({"use", "tool@1"});
    EXPECT_NE(used.exit_code, 0) << used.transcript();
    EXPECT_EQ(tk::read_file(scope / "usr/include/shared.h"), "user header");
    EXPECT_EQ(tk::read_file(scope / "usr/include/shared.h.xlings-new/sentinel"), "user staging");
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(scope / ".xlings.json")), scopeConfig);
}
