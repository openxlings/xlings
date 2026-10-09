#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.xdev.selection;
import xlings.libs.json;

namespace sel = xlings::xdev::selection;
namespace fs = std::filesystem;

XTEST(XdevSelection, DependenciesFollowImplementationsPartitionsAndHeadersTransitively,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    std::vector<sel::Source> graph{
        sel::scan_source("modules/base/api.cppm", "export module base; export import :part;"),
        sel::scan_source("modules/base/part.cppm", "export module base:part;"),
        sel::scan_source("modules/base/api.cpp", "module base;\n#include \"local.hpp\""),
        sel::scan_source("modules/base/local.hpp", "// nothing"),
        sel::scan_source("modules/upper/api.cppm", "export module upper; import base;"),
        sel::scan_source("tests/unit/a.cpp", "import upper;"),
        sel::scan_source("tests/unit/b.cpp", "import elsewhere;"),
        sel::scan_source("tests/e2e/c.cpp", "import std;")};
    const std::vector<sel::Test> tests{
        {.id = "unit/a", .source = "tests/unit/a.cpp"},
        {.id = "unit/b", .source = "tests/unit/b.cpp"},
        {.id = "e2e/c", .source = "tests/e2e/c.cpp"}};
    for (const auto& path : {"modules/base/api.cpp", "modules/base/part.cppm", "modules/base/local.hpp"}) {
        const std::vector<fs::path> changed{path};
        const auto impacted = sel::affected(tests, graph, changed);
        EXPECT_TRUE(impacted.contains("unit/a")) << path;
        EXPECT_FALSE(impacted.contains("unit/b")) << path;
        EXPECT_TRUE(impacted.contains("e2e/c")) << "black-box e2e exercises the changed product";
    }
    const std::vector<fs::path> docs{"docs/readme.md"};
    EXPECT_TRUE(sel::affected(tests, graph, docs).empty());
    const std::vector<fs::path> removed{"modules/base/deleted.cppm"};
    EXPECT_EQ(sel::affected(tests, graph, removed).size(), tests.size()) << "unknown removed source is conservative";
}

XTEST(XdevSelection, CommentsAndFixtureStringsCannotInventModuleImports,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    const auto source = sel::scan_source("tests/a.cpp", R"fixture(
        // import comment;
        /* export module wrong; */
        const auto fixture = R"(import fake; export module fake;)";
        const char* text = "import quoted;";
        export module real:part;
        import :other;
        import actual.api;
        #include "local.hpp"
    )fixture");
    EXPECT_EQ(source.module, "real:part");
    EXPECT_EQ(source.imports, (std::vector<std::string>{"real:other", "actual.api"}));
    EXPECT_EQ(source.includes, (std::vector<std::string>{"local.hpp"}));
}

XTEST(XdevSelection, PrKeepsFastTestsButSelectsAffectedIsolationAndNeverNetwork,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    const std::vector<sel::Test> tests{
        {.id = "unit/fast", .source = "tests/unit/fast.cpp", .cases = {{.name = "Unit.Fast"}}, .metadata_known = true},
        {.id = "e2e/mixed", .source = "tests/e2e/mixed.cpp", .cases = {
            {.name = "Flow.Fast", .area = "subos"},
            {.name = "Isolation.Real", .area = "subos", .requires_ = {"linux", "sandbox"}},
            {.name = "Download.Network", .area = "xim", .requires_ = {"network"}},
            {.name = "Mac.Platform", .requires_ = {"macos"}}}, .metadata_known = true}};
    sel::Options options{.lane = "pr", .platform = "linux", .changed = true};
    auto unrelated = sel::select(tests, options);
    ASSERT_TRUE(unrelated.has_value());
    ASSERT_EQ(unrelated->size(), 2);
    EXPECT_EQ(unrelated->front().filter, "Flow.Fast");
    auto changed = sel::select(tests, options, {"e2e/mixed"});
    ASSERT_TRUE(changed.has_value());
    EXPECT_EQ(changed->front().filter, "Flow.Fast:Isolation.Real");
    options.lane = "main";
    auto main = sel::select(tests, options);
    ASSERT_TRUE(main.has_value());
    EXPECT_EQ(main->front().filter, "Flow.Fast:Isolation.Real:Download.Network");
    options.platform = "macos";
    auto mac = sel::select(tests, options);
    ASSERT_TRUE(mac.has_value());
    EXPECT_EQ(mac->front().filter, "Flow.Fast:Download.Network:Mac.Platform");
}

XTEST(XdevSelection, HistoricalDurationShardsAreDeterministicDisjointAndComplete,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    std::vector<sel::Test> tests{
        {.id = "a", .source = "tests/unit/a.cpp", .duration_ms = 9},
        {.id = "b", .source = "tests/unit/b.cpp", .duration_ms = 8},
        {.id = "c", .source = "tests/unit/c.cpp", .duration_ms = 2},
        {.id = "d", .source = "tests/unit/d.cpp", .duration_ms = 1}};
    ASSERT_TRUE(sel::apply_timings(tests, nlohmann::json{{"a", 90}, {"b", 80}, {"c", 20}, {"d", 10}}).has_value());
    sel::Options options{.platform = "linux", .shards = 2};
    const auto plan = sel::matrix(tests, options);
    ASSERT_EQ(plan["include"].size(), 2);
    EXPECT_EQ(plan["include"][0]["estimated_ms"], 100);
    EXPECT_EQ(plan["include"][1]["estimated_ms"], 100);
    std::set<std::string> unionIds;
    for (std::size_t shard = 1; shard <= 2; ++shard) {
        options.shard = shard;
        const auto selected = sel::select(tests, options);
        ASSERT_TRUE(selected.has_value());
        std::vector<std::string> ids;
        for (const auto& test : *selected) {
            EXPECT_TRUE(unionIds.insert(test.test.id).second) << "a test occurs in more than one shard";
            ids.push_back(test.test.id);
        }
        EXPECT_EQ(plan["include"][shard - 1]["tests"], ids) << "plan and execution select the same IDs";
    }
    EXPECT_EQ(unionIds.size(), tests.size());
    std::ranges::reverse(tests);
    options.shard = 0;
    EXPECT_EQ(sel::matrix(tests, options), plan);
}

XTEST(XdevSelection, InvalidSelectorsTimingsAndUnknownMetadataFailExplicitly,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    for (auto bad : {"0/2", "3/2", "1/0", "1/1025", "1", "1/2x", "-1/2"})
        EXPECT_FALSE(sel::parse_shard(bad).has_value()) << bad;
    EXPECT_EQ(sel::parse_shard("2/3").value(), (std::pair<std::size_t, std::size_t>{2, 3}));
    std::vector<sel::Test> tests{{.id = "a", .source = "tests/unit/a.cpp"}};
    sel::Options options{.lane = "unknown"};
    EXPECT_FALSE(sel::select(tests, options).has_value());
    options.lane = "pr";
    options.require_metadata = true;
    EXPECT_FALSE(sel::select(tests, options).has_value());
    options.require_metadata = false;
    options.area = "subos";
    EXPECT_FALSE(sel::select(tests, options).has_value());
    EXPECT_FALSE(sel::apply_timings(tests, nlohmann::json{{"a", -1}}).has_value());
}

XTEST(XdevSelection, DiscoveryRequiresRealSourceFilesAndRejectsDuplicateCaseFilters,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    auto home = xlings::testkit::Home::isolated("selection-discovery");
    xlings::testkit::write_file(home.root() / "tests/unit/a.cpp", "import std;");
    const auto record = nlohmann::json{{"member", ""}, {"test", "unit/a"}, {"main", "tests/unit/a.cpp"}};
    ASSERT_EQ(sel::discovery(record.dump() + "\n", home.root())->size(), 1);
    EXPECT_FALSE(sel::discovery(record.dump() + "\n" + record.dump() + "\n", home.root()).has_value());
    auto invalid = record;
    invalid["main"] = "../outside.cpp";
    EXPECT_FALSE(sel::discovery(invalid.dump(), home.root()).has_value());
    invalid = record;
    invalid["main"] = "tests/unit/missing.cpp";
    EXPECT_FALSE(sel::discovery(invalid.dump(), home.root()).has_value());
    invalid = record;
    invalid["cases"] = nlohmann::json::array({{{"test", "Suite.Case"}}, {{"test", "Suite.Case"}}});
    EXPECT_FALSE(sel::discovery(invalid.dump(), home.root()).has_value());
}
