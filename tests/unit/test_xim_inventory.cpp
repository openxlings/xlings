#include <gtest/gtest.h>

import std;
import xlings.core.xim.inventory;
import xlings.core.xvm.types;

using namespace xlings::xim;

TEST(XimInventory, StoreNamesPreserveCanonicalIdentity) {
    EXPECT_EQ(identity_from_store_name("mcpp"),
              (std::pair<std::string, std::string>{"", "mcpp"}));
    EXPECT_EQ(identity_from_store_name("d2x-x-mcpp"),
              (std::pair<std::string, std::string>{"d2x", "mcpp"}));
}

TEST(XimInventory, WorkspaceStateDrivesExactInventoryAndMissingPayloadIsVisible) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-inventory-{}", std::chrono::steady_clock::now()
            .time_since_epoch().count());
    fs::create_directories(root / "xpkgs" / "orphan" / "9.9.9");
    std::ofstream(root / "xpkgs" / "orphan" / "9.9.9" / "file") << "orphan";
    fs::create_directories(root / "xpkgs" / "ns-x-tool" / "1.2.3");
    std::ofstream(root / "xpkgs" / "ns-x-tool" / "1.2.3" / "tool") << "ok";

    xlings::xvm::VersionDB db;
    std::vector<InventoryWorkspace> workspaces{
        {.name = "default",
         .active = {{"tool", "ns:1.2.3"}},
         .installed = {{"tool", {"ns:1.2.3", "ns:1.2.2"}}},
         .current = true},
        {.name = "other",
         .installed = {{"tool", {"ns:1.2.3"}}}},
    };
    std::map<std::string, detail::CatalogMetadata> metadata;
    auto records = detail::assemble_inventory(
        db, workspaces, root / "xpkgs", metadata, true);

    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].canonicalName, "ns:tool");
    EXPECT_EQ(records[0].version, "1.2.3");
    EXPECT_EQ(records[0].suboses,
              (std::set<std::string>{"default", "other"}));
    EXPECT_TRUE(records[0].inCurrentSubos);
    EXPECT_TRUE(records[0].payloadPresent);
    EXPECT_TRUE(records[0].active);
    EXPECT_EQ(records[0].degradedReason, "index entry unavailable");
    EXPECT_EQ(records[1].version, "1.2.2");
    EXPECT_FALSE(records[1].payloadPresent);
    EXPECT_EQ(records[1].degradedReason, "payload missing");
    EXPECT_TRUE(std::ranges::none_of(records, [](const auto& record) {
        return record.name == "orphan";
    }));

    fs::remove_all(root);
}

TEST(XimInventory, InstallerMetadataRetainsPackagesWithoutTargets) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-inventory-marker-{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
    const auto payload = root / "xpkgs" / "data-only" / "2.0.0";
    fs::create_directories(payload);
    std::ofstream(payload / ".xim-installed") << "";

    xlings::xvm::VersionDB db;
    std::vector<InventoryWorkspace> workspaces;
    std::map<std::string, detail::CatalogMetadata> metadata;
    auto records = detail::assemble_inventory(
        db, workspaces, root / "xpkgs", metadata, true);

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].canonicalName, "data-only");
    EXPECT_EQ(records[0].version, "2.0.0");
    EXPECT_TRUE(records[0].payloadPresent);
    fs::remove_all(root);
}

TEST(XimInventory, MissingPayloadRootIsAnEmptyInventory) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-inventory-empty-{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
    fs::remove_all(root);

    xlings::xvm::VersionDB db;
    std::vector<InventoryWorkspace> workspaces;
    std::map<std::string, detail::CatalogMetadata> metadata;
    const auto records = detail::assemble_inventory(
        db, workspaces, root / "xpkgs", metadata, true);

    EXPECT_TRUE(records.empty());
}
