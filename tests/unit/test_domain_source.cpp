#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer_source;
import xlings.libs.json;
import xlings.platform;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace domain = xlings::home::prefix_domain;
namespace source = xlings::home::domain_producer_source;
using Json = nlohmann::json;
namespace {
struct Fixture {
    fs::path home, root;
    explicit Fixture(const fs::path& directory) {
        fs::create_directories(directory);
        home = fs::canonical(directory);
        root = home / "data/xpkgs/fixture-x-tool/1.0.0";
        tk::write_file(home / ".xlings-home", R"({"mode":"root","layout":"multi","future":42})");
        tk::write_file(root / "bin/tool", "immutable package bytes");
        tk::write_file(root / ".xlings-resolution.json", Json{{"package", "fixture:tool@1.0.0"},
            {"deps", Json::array()}, {"future", "retain record"}}.dump());
        Json versions{{"tool", {{"type", "program"}, {"future", true}, {"versions", {
            {"fixture:1.0.0", {{"path", (root / "bin").generic_string()},
             {"alias", Json::array({"/xlings/data/xpkgs/fixture-x-tool/1.0.0/bin/tool"})},
             {"future", "keep literal /xlings/data"}}}}}}}};
        tk::write_file(home / ".xlings.json", Json{{"versions", versions}, {"future", 77}}.dump());
        tk::write_file(home / "subos/default/.xlings.json", Json{{"workspace", {
            {"tool", {{"active", "fixture:1.0.0"}, {"installed", Json::array({"fixture:1.0.0"})}}}}}}.dump());
    }
};
}

XTEST(DomainSource, FacadeUsesCheckedPathsAndPreservesPackageBytesAndUnknownFields,
      .area = "home", .covers = {"DOM-PREFIX", "HOME-LAYER-RESOLVE"}, .requires_ = {"posix"}) {
    auto owner = tk::Home::isolated("domain-source-facade");
    Fixture original(owner.root() / "source");
    const auto originalConfig = tk::read_file(original.home / ".xlings.json");
    const auto originalRecord = tk::read_file(original.root / ".xlings-resolution.json");
    tk::write_file(original.home / "data/versions.json", "{}");
    auto selected = domain::resolve(owner.dir(), "/xlings", original.home);
    ASSERT_TRUE(selected) << selected.error();
    ASSERT_TRUE(domain::prepare_private(*selected));
    const auto stage = owner.root() / "facade";
    fs::create_directory(stage);
    auto facade = source::prepare(*selected, stage);
    ASSERT_TRUE(facade) << facade.error();
    ASSERT_EQ(facade->payloads.size(), 1);
    EXPECT_EQ(facade->payloads[0].source, original.root);
    EXPECT_EQ(facade->payloads[0].destination, fs::path("/xlings/data/xpkgs/fixture-x-tool/1.0.0"));
    const auto mapped = Json::parse(tk::read_file(stage / "primary.json"));
    const auto& version = mapped["versions"]["tool"]["versions"]["fixture:1.0.0"];
    EXPECT_EQ(version["path"], "/run/xlings-system-source/data/xpkgs/fixture-x-tool/1.0.0/bin");
    EXPECT_EQ(version["alias"][0], "/xlings/data/xpkgs/fixture-x-tool/1.0.0/bin/tool");
    EXPECT_EQ(version["future"], "keep literal /xlings/data");
    EXPECT_EQ(mapped["future"], 77);
    EXPECT_EQ(tk::read_file(original.home / ".xlings.json"), originalConfig);
    EXPECT_EQ(tk::read_file(original.root / ".xlings-resolution.json"), originalRecord);
    EXPECT_EQ(tk::read_file(original.root / "bin/tool"), "immutable package bytes");
    EXPECT_EQ(Json::parse(tk::read_file(stage / "versions-cache.json")), Json::object());
    EXPECT_TRUE(fs::is_empty(selected->physicalHome / "data/xpkgs/fixture-x-tool/1.0.0"));
    const auto nextStage = owner.root() / "facade-again";
    fs::create_directory(nextStage);
    auto next = source::prepare(*selected, nextStage);
    ASSERT_TRUE(next) << next.error();
    EXPECT_EQ(next->payloads.size(), 1) << "only inode-proved empty source slots are reusable";
}

XTEST(DomainSource, UserOwnedPrivatePayloadIsNeverHiddenByAReadOnlySourceMount,
      .area = "home", .covers = {"DOM-LAYOUTS", "HOME-LAYER-RESOLVE"}, .requires_ = {"posix"}) {
    auto owner = tk::Home::isolated("domain-source-conflict");
    Fixture original(owner.root() / "source");
    auto selected = domain::resolve(owner.dir(), "/xlings", original.home);
    ASSERT_TRUE(selected); ASSERT_TRUE(domain::prepare_private(*selected));
    const auto local = selected->physicalHome / "data/xpkgs/fixture-x-tool/1.0.0";
    tk::write_file(local / "sentinel", "private user data");
    const auto stage = owner.root() / "facade"; fs::create_directory(stage);
    auto facade = source::prepare(*selected, stage);
    ASSERT_TRUE(facade) << facade.error();
    EXPECT_TRUE(facade->payloads.empty());
    ASSERT_EQ(facade->mapping.payloads.size(), 1);
    EXPECT_FALSE(facade->mapping.payloads[0].logicalBound);
    EXPECT_EQ(tk::read_file(local / "sentinel"), "private user data");
}

XTEST(DomainSource, MissingEvidenceAndCorruptSlotAuthorityRefuseInsteadOfGuessing,
      .area = "home", .covers = {"HOME-LAYER-RESOLVE", "DOM-PREFIX"}, .requires_ = {"posix"}) {
    auto owner = tk::Home::isolated("domain-source-strict");
    Fixture original(owner.root() / "source");
    auto selected = domain::resolve(owner.dir(), "/xlings", original.home);
    ASSERT_TRUE(selected); ASSERT_TRUE(domain::prepare_private(*selected));
    const auto stage = owner.root() / "facade"; fs::create_directory(stage);
    const auto record = tk::read_file(original.root / ".xlings-resolution.json");
    fs::remove(original.root / ".xlings-resolution.json");
    EXPECT_FALSE(source::prepare(*selected, stage));
    tk::write_file(original.root / ".xlings-resolution.json", record);
    const auto slots = selected->physicalHome.parent_path() / "source-slots.json";
    tk::write_file(slots, R"({"schema":"bad","physical_home":1,"slots":{}})");
    EXPECT_FALSE(source::prepare(*selected, stage));
    EXPECT_EQ(tk::read_file(slots), R"({"schema":"bad","physical_home":1,"slots":{}})");
    EXPECT_EQ(tk::read_file(original.root / "bin/tool"), "immutable package bytes");
}
