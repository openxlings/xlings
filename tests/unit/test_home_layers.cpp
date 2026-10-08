#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.libs.json;
import xlings.core.home;
import xlings.core.config;
import xlings.core.home.layers;
import xlings.core.profile;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.payload;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace h = xlings::home;
namespace layers = xlings::home::layers;
namespace xvm = xlings::xvm;
using Json = nlohmann::json;

namespace {
struct Env {
    std::string key, previous;
    Env(std::string name, std::string value) : key(std::move(name)) {
        if (auto* value = std::getenv(key.c_str())) previous = value;
        xlings::platform::set_env_variable(key, value);
    }
    ~Env() { xlings::platform::set_env_variable(key, previous); }
};

struct Layer {
    fs::path home;
    fs::path tool;
    fs::path dependency;
    explicit Layer(const fs::path& path) : home(path), tool(path / "data/xpkgs/fixture-x-layer-tool/1.0.0"),
        dependency(path / "data/xpkgs/fixture-x-layer-dep/2.0.0") {
        tk::write_file(home / ".xlings-home", R"({"schema":1,"id":"layer","mode":"root","layout":"multi"})");
        tk::write_file(tool / "bin/layer-tool", "tool sentinel");
        tk::write_file(dependency / "include/layer.h", "header sentinel");
        tk::write_file(dependency / "lib/liblayer.a", "library sentinel");
        Json versions = {
            {"layer-tool", {{"type", "program"}, {"versions", {{"fixture:1.0.0", {
                {"path", "${XLINGS_HOME}/data/xpkgs/fixture-x-layer-tool/1.0.0/bin"},
                {"alias", {"${XLINGS_HOME}/data/xpkgs/fixture-x-layer-tool/1.0.0/bin/layer-tool"}}}}}}}},
            {"layer-dep", {{"type", "lib"}, {"versions", {{"fixture:2.0.0", {
                {"path", "${XLINGS_HOME}/data/xpkgs/fixture-x-layer-dep/2.0.0/lib"},
                {"sourceName", "liblayer.a"}, {"destinationName", "liblayer.a"},
                {"includedir", "${XLINGS_HOME}/data/xpkgs/fixture-x-layer-dep/2.0.0/include"}}}}}}}
        };
        tk::write_file(home / ".xlings.json", Json{{"versions", versions}, {"future", true}}.dump());
        Json workspace = {
            {"layer-tool", {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}}},
            {"layer-dep", {{"active", "fixture:2.0.0"}, {"installed", {"fixture:2.0.0"}}}}
        };
        tk::write_file(home / "subos/default/.xlings.json", Json{
            {"workspace", workspace}, {"configured", {{"fixture:layer-tool@1.0.0", 9}}}}.dump());
        tk::write_file(tool / ".xlings-resolution.json", Json{
            {"package", "fixture:layer-tool@1.0.0"}, {"deps", Json::array({{
                {"spec", "layer-dep@2.0.0"}, {"name", "fixture:layer-dep"}, {"version", "2.0.0"},
                {"install_dir", dependency.string()}, {"source", "fixture"}, {"libdirs", {"lib"}}}})}}.dump());
        tk::write_file(dependency / ".xlings-resolution.json", Json{
            {"package", "fixture:layer-dep@2.0.0"}, {"deps", Json::array()}}.dump());
    }
};
}

XTEST(HomeLayers, GarbageCollectionDoesNotFollowAStoreIntoAnotherHome,
      .area = "home", .covers = {"HOME-LAYER-RESOLVE"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "requires directory symlinks";
    auto isolated = tk::Home::isolated("layer-gc-ownership");
    const auto user = isolated.root() / "user";
    Layer system(isolated.root() / "system");
    tk::write_file(user / ".xlings.json", R"({"versions":{}})");
    fs::create_directories(user / "data");
    fs::create_directory_symlink(system.home / "data/xpkgs", user / "data/xpkgs");
    EXPECT_EQ(xlings::profile::gc(user, false), 1);
    EXPECT_EQ(tk::read_file(system.tool / "bin/layer-tool"), "tool sentinel");
    EXPECT_EQ(tk::read_file(system.dependency / "include/layer.h"), "header sentinel");
}

XTEST(HomeLayers, RootMultiMarkerAndStrictUnreadableState, .area = "home",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto isolated = tk::Home::isolated("strict-layer");
    Layer layer(isolated.root() / "system");
    Env selected("XLINGS_SYSTEM_LAYER", layer.home.string());
    auto declared = h::read_system_layer();
    ASSERT_TRUE(declared);
    ASSERT_TRUE(*declared);
    EXPECT_EQ(**declared, fs::canonical(layer.home));
    const auto context = h::describe(layer.home, h::Source::Env);
    EXPECT_EQ(context.mode, h::Mode::Root);
    EXPECT_EQ(context.rootLayout, "multi");
    ASSERT_TRUE(h::declare(layer.home, std::nullopt, 2));
    EXPECT_EQ(Json::parse(tk::read_file(layer.home / ".xlings-home"))["layout"], "multi");
    tk::write_file(layer.home / ".xlings-home", "{corrupt");
    EXPECT_FALSE(h::read_system_layer());
    tk::write_file(layer.home / ".xlings-home", R"({"mode":"root","layout":"single"})");
    declared = h::read_system_layer();
    ASSERT_TRUE(declared);
    EXPECT_FALSE(*declared);
    tk::write_file(layer.home / ".xlings.json", "{corrupt");
    EXPECT_FALSE(layers::read_snapshot(layer.home));
    fs::remove(layer.home / ".xlings.json");
    tk::write_file(layer.home / ".xlings.json", R"({"versions":{"tool":{"versions":[]}}})");
    EXPECT_FALSE(layers::read_snapshot(layer.home));
    if (!tk::is_windows) {
        fs::remove(layer.home / ".xlings.json");
        fs::create_symlink(layer.home / "does-not-exist", layer.home / ".xlings.json");
        EXPECT_FALSE(h::read_json_for_update(layer.home / ".xlings.json"));
        EXPECT_FALSE(layers::read_snapshot(layer.home));
    }
    EXPECT_TRUE(h::read_json_for_update(layer.home / "actually-absent.json"));
}

XTEST(HomeLayers, SharedStoreCollectionRetainsPayloadsWithoutLocalReferences,
      .area = "home", .covers = {"HOME-LAYER-RESOLVE"}) {
    auto isolated = tk::Home::isolated("shared-layer-gc");
    Layer layer(isolated.root() / "system");
    tk::write_file(layer.home / ".xlings.json", R"({"versions":{}})");
    fs::remove(layer.home / "subos/default/.xlings.json");
    EXPECT_EQ(xlings::profile::gc(layer.home, false), 0);
    EXPECT_EQ(tk::read_file(layer.tool / "bin/layer-tool"), "tool sentinel");
    EXPECT_EQ(tk::read_file(layer.dependency / "include/layer.h"), "header sentinel");
    tk::write_file(layer.home / ".xlings-home", R"({"mode":"root","layout":"future"})");
    EXPECT_EQ(xlings::profile::gc(layer.home, false), 1);
    EXPECT_EQ(tk::read_file(layer.tool / "bin/layer-tool"), "tool sentinel");
}

XTEST(HomeLayers, BorrowIncludesRuntimeDependenciesAndPinsForeignHome, .area = "home",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto isolated = tk::Home::isolated("closure-layer");
    Layer layer(isolated.root() / "system");
    auto snapshot = layers::read_snapshot(layer.home);
    ASSERT_TRUE(snapshot) << snapshot.error();
    auto& root = snapshot->versions.at("layer-tool").versions.at("fixture:1.0.0");
    root.kind = "program";
    root.bindingGroup = xvm::BindingGroupRef{
        .provider = "fixture:layer-tool", .providerVersion = "1.0.0", .group = "release",
        .rootTarget = "layer-tool", .rootVersion = "fixture:1.0.0"};
    root.bindingMembers = {{"layer-tool", "fixture:1.0.0"}, {"layer-helper", "fixture:1.0.0"}};
    root.bindingMembersDeclared = true;
    auto helper = root;
    helper.bindingMembers.clear();
    helper.bindingMembersDeclared = false;
    snapshot->versions["layer-helper"].type = "program";
    snapshot->versions["layer-helper"].versions["fixture:1.0.0"] = helper;
    snapshot->workspace.installed["layer-helper"] = {"fixture:1.0.0"};
    auto plan = layers::plan_borrow(*snapshot, {}, "layer-helper", "fixture:1.0.0");
    ASSERT_TRUE(plan) << plan.error();
    EXPECT_EQ(plan->members.size(), 3);
    EXPECT_EQ(plan->members.at("layer-tool"), "fixture:1.0.0");
    EXPECT_EQ(plan->members.at("layer-dep"), "fixture:2.0.0");
    EXPECT_EQ(plan->payloads.size(), 2);
    const auto& data = plan->registrations.at("layer-tool").versions.at("fixture:1.0.0");
    EXPECT_EQ(data.sourceHome, fs::canonical(layer.home).string());
    EXPECT_EQ(data.sourceScope, "default");
    EXPECT_EQ(data.path, (layer.tool / "bin").string());
    EXPECT_EQ(data.alias.front(), (layer.tool / "bin/layer-tool").string());
    const auto roundtrip = xvm::vdata_from_json(xvm::vdata_to_json(data));
    EXPECT_EQ(roundtrip.sourceHome, data.sourceHome);
    EXPECT_EQ(roundtrip.sourceScope, data.sourceScope);
    auto future = xvm::vdata_to_json(data);
    future["layer"]["future"] = {{"flag", true}};
    EXPECT_EQ(xvm::vdata_to_json(xvm::vdata_from_json(future))["layer"]["future"], future["layer"]["future"]);
    EXPECT_TRUE(layers::owns_payload(layer.home, layer.tool));
    EXPECT_FALSE(layers::owns_payload(isolated.dir(), layer.tool));
    EXPECT_FALSE(fs::exists(isolated.dir() / "data/xpkgs"));
}

XTEST(HomeLayers, KnownIncompleteAndForeignPayloadsCannotBeBorrowed, .area = "home",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto isolated = tk::Home::isolated("failed-layer");
    Layer layer(isolated.root() / "system");
    auto snapshot = layers::read_snapshot(layer.home);
    ASSERT_TRUE(snapshot);
    xlings::xim::write_payload_failure_marker(layer.dependency, "2.0.0", "failed config");
    auto failed = layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0");
    ASSERT_FALSE(failed);
    EXPECT_NE(failed.error().find("incomplete"), std::string::npos);
    tk::write_file(layer.dependency / ".xpkg-install.json", R"({"os":"foreign-test-platform"})");
    auto foreign = layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0");
    ASSERT_FALSE(foreign);
    EXPECT_NE(foreign.error().find("another platform"), std::string::npos);
}

XTEST(HomeLayers, PackageLookupUsesRecordedProviderInsteadOfTargetSpelling, .area = "home",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto isolated = tk::Home::isolated("provider-layer");
    Layer layer(isolated.root() / "system");
    auto snapshot = layers::read_snapshot(layer.home);
    ASSERT_TRUE(snapshot);
    snapshot->versions["different-tool-name"] = snapshot->versions.at("layer-tool");
    snapshot->versions.erase("layer-tool");
    snapshot->workspace.installed["different-tool-name"] = {"fixture:1.0.0"};
    snapshot->workspace.installed.erase("layer-tool");
    auto selected = layers::plan_borrow_package(*snapshot, {}, "fixture:layer-tool", "1.0.0");
    ASSERT_TRUE(selected) << selected.error();
    ASSERT_TRUE(*selected);
    EXPECT_EQ((**selected).requestedPayload, fs::canonical(layer.tool));
    EXPECT_TRUE((**selected).requestedMembers.contains("different-tool-name"));
    EXPECT_FALSE((**selected).requestedMembers.contains("layer-dep"));
    EXPECT_EQ((**selected).payloadCoordinates.at("fixture:layer-dep@2.0.0"), fs::canonical(layer.dependency));
    auto absent = layers::plan_borrow_package(*snapshot, {}, "other:layer-tool", "1.0.0");
    ASSERT_TRUE(absent);
    EXPECT_FALSE(*absent);
    fs::remove(layer.tool / ".xlings-resolution.json");
    EXPECT_FALSE(layers::plan_borrow_package(*snapshot, {}, "fixture:layer-tool", "1.0.0"));
}

XTEST(HomeLayers, MissingCorruptAndContradictoryEvidenceCannotInventEmptyClosure, .area = "home",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto isolated = tk::Home::isolated("missing-layer");
    Layer layer(isolated.root() / "system");
    auto snapshot = layers::read_snapshot(layer.home);
    ASSERT_TRUE(snapshot);
    fs::remove(layer.dependency / ".xlings-resolution.json");
    auto rejected = layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0");
    ASSERT_FALSE(rejected);
    EXPECT_NE(rejected.error().find("--reconfig"), std::string::npos);
    tk::write_file(layer.dependency / ".xlings-resolution.json", "{bad");
    EXPECT_FALSE(layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0"));
    tk::write_file(layer.dependency / ".xlings-resolution.json", R"({"package":"fixture:other@2.0.0","deps":[]})");
    EXPECT_FALSE(layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0"));
    tk::write_file(layer.dependency / ".xlings-resolution.json", R"({"package":"layer-dep@2.0.0","deps":[]})");
    EXPECT_TRUE(layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0"));
    snapshot->workspace.installed.erase("layer-dep");
    EXPECT_FALSE(layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0"));
}

XTEST(HomeLayers, UserOwnershipAndOutsideStoreAssetsRefuseUnsafeBorrowing, .area = "home",
      .covers = {"HOME-LAYER-RESOLVE"}) {
    auto isolated = tk::Home::isolated("priority-layer");
    Layer layer(isolated.root() / "system");
    auto snapshot = layers::read_snapshot(layer.home);
    ASSERT_TRUE(snapshot);
    auto own = snapshot->versions;
    EXPECT_FALSE(layers::plan_borrow(*snapshot, own, "layer-tool", "fixture:1.0.0"));
    tk::write_file(isolated.root() / "foreign/header", "private");
    snapshot->versions.at("layer-dep").versions.at("fixture:2.0.0").includedir = (isolated.root() / "foreign").string();
    EXPECT_FALSE(layers::plan_borrow(*snapshot, {}, "layer-tool", "fixture:1.0.0"));
    if (!tk::is_windows) {
        fs::create_directory_symlink(isolated.root() / "foreign", layer.home / "data/xpkgs/foreign");
        EXPECT_FALSE(layers::owns_payload(layer.home, layer.home / "data/xpkgs/foreign/header"));
    }
    auto invalid = xvm::vdata_from_json(Json{{"path", "x"}, {"layer", {{"home", 7}, {"scope", "default"}}}});
    EXPECT_FALSE(invalid.bindingIntegrityIssues.empty());
    EXPECT_EQ(xvm::vdata_to_json(invalid)["layer"]["home"], 7);
}

XTEST(HomeLayers, UseMaterializesTheFullBorrowedClosureWithoutCopyingConfiguredState,
      .area = "home", .covers = {"HOME-LAYER-RESOLVE"}, .requires_ = {"xlings-bin"}) {
    auto isolated = tk::Home::isolated("use-layer");
    Layer layer(isolated.root() / "system");
    const auto sourceConfig = tk::read_file(layer.home / ".xlings.json");
    fs::create_directories(isolated.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), isolated.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    const auto primary = isolated.root() / "empty-index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    auto config = Json::parse(tk::read_file(isolated.dir() / ".xlings.json"));
    config["xim"]["index-repo"] = primary.generic_string();
    config["index_repos"] = Json::array({{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                                         {"url", primary.generic_string()}, {"source", "git"}}});
    tk::write_file(isolated.dir() / ".xlings.json", config.dump());
    const auto initialized = isolated.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();
    auto result = isolated.xlings({"use", "layer-tool", "1.0.0"}, {{"XLINGS_SYSTEM_LAYER", layer.home.string()}});
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    const auto state = Json::parse(tk::read_file(isolated.dir() / "subos/default/.xlings.json"));
    EXPECT_EQ(state["workspace"]["layer-dep"]["active"], "fixture:2.0.0");
    EXPECT_EQ(state["workspace"]["layer-tool"]["active"], "fixture:1.0.0");
    EXPECT_FALSE(state.contains("configured") && state["configured"].contains("fixture:layer-tool@1.0.0"));
    EXPECT_EQ(tk::read_file(isolated.dir() / "subos/default/usr/include/layer.h"), "header sentinel");
    EXPECT_EQ(tk::read_file(isolated.dir() / "subos/default/lib/liblayer.a"), "library sentinel");
    EXPECT_FALSE(fs::exists(isolated.dir() / "data/xpkgs/fixture-x-layer-tool"));
    EXPECT_EQ(tk::read_file(layer.home / ".xlings.json"), sourceConfig);
    const auto registration = Json::parse(tk::read_file(isolated.dir() / ".xlings.json"));
    EXPECT_EQ(registration["versions"]["layer-tool"]["versions"]["fixture:1.0.0"]["layer"]["home"], fs::canonical(layer.home).string());
    fs::remove(layer.dependency / ".xlings-resolution.json");
    const auto before = tk::read_file(isolated.dir() / "subos/default/.xlings.json");
    result = isolated.xlings({"use", "layer-tool", "1.0.0"}, {{"XLINGS_SYSTEM_LAYER", layer.home.string()}});
    EXPECT_NE(result.exit_code, 0) << result.transcript();
    EXPECT_EQ(tk::read_file(isolated.dir() / "subos/default/.xlings.json"), before);
    // A higher layer owns its own candidate even when the borrowed layer has
    // a numerically newer version of the same target.
    auto local = registration;
    auto ownData = local["versions"]["layer-tool"]["versions"]["fixture:1.0.0"];
    ownData.erase("layer");
    const auto ownedPayload = isolated.dir() / "data/xpkgs/fixture-x-user-tool/0.1.0/bin";
    tk::write_file(ownedPayload / "layer-tool", "user tool");
    ownData["path"] = ownedPayload.string();
    ownData["alias"] = Json::array({(ownedPayload / "layer-tool").string()});
    local["versions"]["layer-tool"]["versions"]["fixture:0.1.0"] = ownData;
    tk::write_file(isolated.dir() / ".xlings.json", local.dump());
    auto ownedState = Json::parse(before);
    ownedState["workspace"]["layer-tool"]["installed"].push_back("fixture:0.1.0");
    tk::write_file(isolated.dir() / "subos/default/.xlings.json", ownedState.dump());
    result = isolated.xlings({"use", "layer-tool", "latest"}, {{"XLINGS_SYSTEM_LAYER", layer.home.string()}});
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    EXPECT_EQ(Json::parse(tk::read_file(isolated.dir() / "subos/default/.xlings.json"))
                  ["workspace"]["layer-tool"]["active"], "fixture:0.1.0");

}
