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
    detail::MetadataLookup metadata{std::map<std::string, detail::CatalogMetadata>{}};
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
    detail::MetadataLookup metadata{std::map<std::string, detail::CatalogMetadata>{}};
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
    detail::MetadataLookup metadata{std::map<std::string, detail::CatalogMetadata>{}};
    const auto records = detail::assemble_inventory(
        db, workspaces, root / "xpkgs", metadata, true);

    EXPECT_TRUE(records.empty());
}

// A package from a project-scoped index repo installs under the PROJECT data
// dir. Deriving every payload path from one global store root reported all of
// them as "degraded: payload missing" while the payload sat where the match
// said it would.
TEST(XimInventory, PayloadPathFollowsTheRepoScope) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-inventory-scope-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    const auto globalStore = root / "global" / "xpkgs";
    const auto projectStore = root / "project" / "xpkgs";
    fs::create_directories(projectStore / "proj-x-tool" / "1.0.0");
    std::ofstream(projectStore / "proj-x-tool" / "1.0.0" / "tool") << "ok";
    fs::create_directories(globalStore);

    xlings::xvm::VersionDB db;
    std::vector<InventoryWorkspace> workspaces{
        {.name = "default",
         .installed = {{"tool", {"proj:1.0.0"}}},
         .current = true},
    };
    detail::MetadataLookup metadata{std::map<std::string, detail::CatalogMetadata>{
        {"proj-x-tool", {.namespaceName = "proj",
                         .name = "tool",
                         .canonicalName = "proj:tool",
                         .description = "project-scoped",
                         .storeRoot = projectStore}},
    }};
    const auto records = detail::assemble_inventory(
        db, workspaces, globalStore, metadata, false);

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].canonicalName, "proj:tool");
    EXPECT_TRUE(records[0].payloadPresent)
        << "payload path resolved against the global store: "
        << records[0].payloadPath.string();
    EXPECT_TRUE(records[0].degradedReason.empty());

    fs::remove_all(root);
}

TEST(XimInventory, ProjectStampOverridesWorkspaceCatalogStoreRoot) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-inventory-stamp-authority-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    const auto globalStore = root / "global" / "xpkgs";
    const auto projectStore = root / "project" / "xpkgs";
    const auto relative = fs::path("proj-x-tool") / "1.0.0";
    fs::create_directories(globalStore / relative);
    fs::create_directories(projectStore / relative);
    std::ofstream(globalStore / relative / ".xpkg-install.json") << "{}";
    std::ofstream(projectStore / relative / ".xpkg-install.json") << "{}";
    std::ofstream(globalStore / relative / "global") << "global";
    std::ofstream(projectStore / relative / "project") << "project";

    xlings::xvm::VersionDB db;
    std::vector<InventoryWorkspace> workspaces{
        {.name = "default",
         .installed = {{"tool", {"proj:1.0.0"}}},
         .current = true},
    };
    detail::MetadataLookup metadata{std::map<std::string, detail::CatalogMetadata>{
        {"proj-x-tool", {.namespaceName = "proj",
                         .name = "tool",
                         .canonicalName = "proj:tool",
                         .description = "global catalog match",
                         .storeRoot = globalStore}},
    }};
    const std::array stores{projectStore, globalStore};

    const auto records = detail::assemble_inventory(
        db, workspaces, stores, metadata, {},
        detail::CoordinateMatch::contains);

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].payloadPath, projectStore / relative);
    EXPECT_TRUE(records[0].payloadPresent);

    fs::remove_all(root);
}

// ── the filtered query must not be narrower than filtering the full one ──
//
// `list <filter>` skips a workspace pair before resolving it, using a cheap
// over-approximation of the owner candidates. If that approximation is ever
// narrower than the real resolution, the filtered query silently omits a row
// the plain query shows -- and nothing in the output says so. This test states
// the relationship as an invariant rather than testing one hand-picked case.
//
// The fixture is the shape the approximation used to miss: `pair` has no
// outgoing edges at all, and its owner is only reachable by first walking
// BACKWARDS to the peer that binds it.
namespace {

std::set<std::string> canonical_names(
        const std::vector<InstalledPackageRecord>& records) {
    std::set<std::string> names;
    for (const auto& record : records) names.insert(record.canonicalName);
    return names;
}

xlings::xvm::VersionDB incoming_only_owner_db() {
    xlings::xvm::VersionDB db;
    // owner@1 binds member@1, and member@1 binds nothing. Reaching "owner"
    // from "member" requires the reverse edge.
    xlings::xvm::VInfo owner;
    owner.versions["1"] = {};
    owner.bindings["member"]["1"] = "1";
    db["owner"] = std::move(owner);

    xlings::xvm::VInfo member;
    member.versions["1"] = {};
    db["member"] = std::move(member);
    return db;
}

}  // namespace

TEST(XimInventoryFilter, AFilteredQueryEqualsFilteringTheFullQuery) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path()
        / std::format("xlings-inventory-filter-{}",
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());
    const auto store = root / "xpkgs";
    fs::create_directories(store);

    const auto db = incoming_only_owner_db();
    std::vector<InventoryWorkspace> workspaces{
        {.name = "default",
         .installed = {{"member", {"1"}}},
         .current = true},
    };
    detail::MetadataLookup full{std::map<std::string, detail::CatalogMetadata>{}};
    const std::array roots{store};
    const auto everything = detail::assemble_inventory(
        db, workspaces, roots, full, {}, detail::CoordinateMatch::contains);

    for (const auto& name : canonical_names(everything)) {
        detail::MetadataLookup scoped{
            std::map<std::string, detail::CatalogMetadata>{}};
        const auto filtered = detail::assemble_inventory(
            db, workspaces, roots, scoped, name,
            detail::CoordinateMatch::contains);
        EXPECT_EQ(canonical_names(filtered), std::set<std::string>{name})
            << "`list " << name << "` does not agree with filtering `list`";
    }

    fs::remove_all(root);
}

// The reverse walk is what makes the above hold; state it directly so a
// regression names the mechanism instead of only the symptom.
TEST(XimInventoryFilter, TheSkipDecisionFollowsEdgesBackwards) {
    const auto db = incoming_only_owner_db();
    const auto incoming = detail::build_incoming_edges(db);
    const auto blind = detail::shallow_owner_candidates_for(
        db, {"member", "1"}, nullptr);
    const auto full = detail::shallow_owner_candidates_for(
        db, {"member", "1"}, &incoming);

    const auto names = [](const auto& candidates) {
        std::set<std::string> out;
        for (const auto& c : candidates) out.insert(c.package);
        return out;
    };
    EXPECT_FALSE(names(blind).contains("owner"))
        << "fixture no longer exercises the reverse edge";
    EXPECT_TRUE(names(full).contains("owner"));
}
