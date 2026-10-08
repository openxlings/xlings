#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct Fixture {
    tk::Home home = tk::Home::isolated("xdev-selection");
    const char* binary = std::getenv("XDEV_BIN");
    Fixture() {
        tk::write_file(home.root() / "mcpp.toml", "[package]\nname = \"selection-fixture\"\n");
        fs::create_directories(home.root() / "tests/unit");
    }
    tk::RunResult run(std::vector<std::string> args, std::map<std::string, std::string> extra = {}) const {
        args.insert(args.begin(), binary ? binary : "");
        auto env = tk::inherited_env();
        for (const auto& [key, value] : extra) env[key] = value;
        return tk::run({.argv = std::move(args), .env = std::move(env), .cwd = home.root()});
    }
    fs::path discovery(const std::vector<json>& records) const {
        const auto file = home.root() / "discovery.ndjson";
        std::string text;
        for (const auto& row : records) text += row.dump() + "\n";
        tk::write_file(file, text);
        return file;
    }
};

json row(std::string id, std::string source, std::vector<json> cases) {
    return {{"member", ""}, {"test", std::move(id)}, {"main", std::move(source)}, {"cases", std::move(cases)}};
}

json test_case(std::string name, std::vector<std::string> capabilities = {}) {
    return {{"test", std::move(name)}, {"area", "fixture"}, {"cost", "fast"}, {"requires", capabilities}};
}

void executable(const fs::path& path, std::string_view contents) {
    tk::write_file(path, contents);
    fs::permissions(path, fs::perms::owner_all);
}

}  // namespace

XTEST(XdevCli, ProductionDiscoveryInvokesMcppListWithoutBuilding,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "POSIX executable discovery fixture";
    Fixture fixture;
    if (!fixture.binary || !*fixture.binary) GTEST_SKIP() << "set XDEV_BIN to the built xdev";
    tk::write_file(fixture.home.root() / "tests/unit/new_test.cpp", "import std;");
    const auto tools = fixture.home.root() / "tools";
    executable(tools / "mcpp", R"(#!/bin/sh
if [ "$*" != "test --list --message-format json" ]; then
  echo "unexpected mcpp invocation: $*" >&2
  exit 41
fi
printf '%s\n' "$*" > "$FAKE_MCPP_LOG"
printf '%s\n' '{"member":"","test":"unit/new_test","main":"tests/unit/new_test.cpp"}' '{"summary":{"total":1}}'
)");
    const auto log = fixture.home.root() / "mcpp-call";
    const auto planned = fixture.run({"ci", "plan", "--platform", "linux", "--shards", "2"},
        {{"PATH", tools.string()}, {"FAKE_MCPP_LOG", log.string()}});
    ASSERT_EQ(planned.exit_code, 0) << planned.transcript();
    const auto matrix = json::parse(planned.out);
    ASSERT_EQ(matrix["include"].size(), 1);
    EXPECT_EQ(matrix["include"][0]["tests"], std::vector<std::string>{"unit/new_test"});
    EXPECT_EQ(tk::read_file(log), "test --list --message-format json\n");
    EXPECT_FALSE(fs::exists(fixture.home.root() / "target")) << "discovery did not build artifacts";
}

XTEST(XdevCli, MatrixAndArtifactExecutionShareTheSameCompleteDisjointShards,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "POSIX executable artifact fixture";
    Fixture fixture;
    if (!fixture.binary || !*fixture.binary) GTEST_SKIP() << "set XDEV_BIN to the built xdev";
    std::vector<json> records;
    for (const auto& name : {"a", "b", "c"}) {
        const auto source = "tests/unit/" + std::string(name) + ".cpp";
        tk::write_file(fixture.home.root() / source, "import std;");
        records.push_back(row("unit/" + std::string(name), source,
            {test_case(std::string(name) + ".Fast"), test_case(std::string(name) + ".Network", {"network"})}));
        executable(fixture.home.root() / "target/fixture/bin/unit" / name, std::format(R"(#!/bin/sh
if [ "$1" = "--gtest_list_tests" ]; then
  printf '%s\n' '{}.' '  Fast' '  Network'
  printf '%s\n' '{{"test":"{}.Fast","area":"fixture","requires":[]}}' '{{"test":"{}.Network","area":"fixture","requires":["network"]}}' > "$XTEST_META_OUT"
  exit 0
fi
printf '%s\n' '{}:'"$*" >> "$EXEC_TRACE"
printf '%s\n' '{{"test":"{}.Fast","status":"pass"}}' >> "$XTEST_RESULTS_OUT"
)", name, name, name, name, name));
    }
    const auto inventory = fixture.discovery(records);
    const auto planned = fixture.run({"ci", "plan", "--platform", "linux", "--lane", "pr",
        "--shards", "2", "--discovery", inventory.string(), "--require-metadata"});
    ASSERT_EQ(planned.exit_code, 0) << planned.transcript();
    const auto matrix = json::parse(planned.out);
    ASSERT_EQ(matrix["include"].size(), 2);
    const auto trace = fixture.home.root() / "trace";
    std::set<std::string> executed;
    for (const auto& job : matrix["include"]) {
        const auto shard = job["shard"].get<std::string>();
        const auto output = fixture.home.root() / ("run-" + shard.substr(0, shard.find('/')));
        const auto run = fixture.run({"test", "--lane", "pr", "--shard", shard, "--no-build",
            "--discovery", inventory.string(), "--out", output.string()}, {{"EXEC_TRACE", trace.string()}});
        ASSERT_EQ(run.exit_code, 0) << run.transcript();
        const auto report = json::parse(tk::read_file(output / "report.json"));
        std::vector<std::string> binaryIds;
        for (const auto& record : report["records"])
            if (record["kind"] == "binary") {
                binaryIds.push_back(record["name"].get<std::string>());
                EXPECT_TRUE(executed.insert(binaryIds.back()).second);
            }
        EXPECT_EQ(binaryIds, job["tests"].get<std::vector<std::string>>());
    }
    EXPECT_EQ(executed.size(), records.size());
    const auto called = tk::read_file(trace);
    for (const auto& name : {"a", "b", "c"})
        EXPECT_NE(called.find(std::string(name) + ":--gtest_filter=" + name + ".Fast"), std::string::npos);
    EXPECT_EQ(called.find(".Network"), std::string::npos);
}

XTEST(XdevCli, ChangedSelectionFollowsARealGitDiffThroughAnImplementationDependency,
      .area = "testkit", .covers = {"CI-TEST-SELECTION"}) {
    Fixture fixture;
    if (!fixture.binary || !*fixture.binary) GTEST_SKIP() << "set XDEV_BIN to the built xdev";
    tk::write_file(fixture.home.root() / "modules/base/api.cppm", "export module fixture.base;");
    tk::write_file(fixture.home.root() / "modules/base/api.cpp", "module fixture.base;");
    tk::write_file(fixture.home.root() / "tests/unit/affected.cpp", "import fixture.base;");
    tk::write_file(fixture.home.root() / "tests/unit/unrelated.cpp", "import fixture.elsewhere;");
    const auto inventory = fixture.discovery({
        row("unit/affected", "tests/unit/affected.cpp", {test_case("Affected.Isolation", {"sandbox"})}),
        row("unit/unrelated", "tests/unit/unrelated.cpp", {test_case("Unrelated.Isolation", {"sandbox"})})});
    auto git = [&](std::vector<std::string> args) {
        args.insert(args.begin(), "git");
        auto env = tk::inherited_env();
        env["HOME"] = fixture.home.root().string();
        env["USERPROFILE"] = fixture.home.root().string();
        env["GIT_CONFIG_NOSYSTEM"] = "1";
        env["GIT_CONFIG_GLOBAL"] = (fixture.home.root() / "no-global-config").string();
        return tk::run({.argv = std::move(args), .env = std::move(env), .cwd = fixture.home.root()});
    };
    ASSERT_EQ(git({"init", "-q"}).exit_code, 0);
    ASSERT_EQ(git({"add", "modules", "tests", "mcpp.toml"}).exit_code, 0);
    ASSERT_EQ(git({"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "base"}).exit_code, 0);
    const auto base = git({"rev-parse", "HEAD"}).out;
    auto revision = base;
    while (!revision.empty() && std::isspace(static_cast<unsigned char>(revision.back()))) revision.pop_back();
    tk::write_file(fixture.home.root() / "modules/base/api.cpp", "module fixture.base;\nint changed = 1;\n");
    ASSERT_EQ(git({"add", "modules/base/api.cpp"}).exit_code, 0);
    ASSERT_EQ(git({"-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "change"}).exit_code, 0);
    const auto planned = fixture.run({"ci", "plan", "--platform", "linux", "--lane", "pr",
        "--changed", revision, "--discovery", inventory.string()});
    ASSERT_EQ(planned.exit_code, 0) << planned.transcript();
    const auto matrix = json::parse(planned.out);
    ASSERT_EQ(matrix["include"].size(), 1);
    EXPECT_EQ(matrix["include"][0]["tests"], std::vector<std::string>{"unit/affected"});
}

XTEST(XdevCli, TrendArtifactsPreserveMemberAndPlatformAndFeedFutureShardPlans,
      .area = "testkit", .covers = {"CI-TRENDS"}) {
    Fixture fixture;
    if (!fixture.binary || !*fixture.binary) GTEST_SKIP() << "set XDEV_BIN to the built xdev";
    auto report = [&](std::string run, long long ms, std::string status, fs::path prior = {}) {
        const auto input = fixture.home.root() / ("input-" + run);
        const auto output = fixture.home.root() / ("report-" + run);
        tk::write_file(input / "lane.json", json{{"name", "linux-unit"}, {"platform", "linux"},
            {"run", run}, {"declared", json::array()}, {"probes", json::object()}}.dump());
        tk::write_file(input / "mcpp.ndjson", json{{"member", "xdev"}, {"test", "unit/slow"},
            {"status", status}, {"duration_ms", ms}}.dump() + "\n");
        std::vector<std::string> args{"report", "--in", input.string(), "--write", output.string(),
            "--timings-out", (fixture.home.root() / "timings.json").string()};
        if (!prior.empty()) args.insert(args.end(), {"--trend", prior.string()});
        return std::pair{fixture.run(args), output};
    };
    const auto [first, baseline] = report("1", 3000, "pass");
    ASSERT_EQ(first.exit_code, 0) << first.transcript();
    const auto [second, latest] = report("2", 6000, "pass", baseline / "trend.json");
    ASSERT_EQ(second.exit_code, 0) << second.transcript();
    EXPECT_NE(second.out.find("100.0%"), std::string::npos) << second.out;
    auto history = json::parse(tk::read_file(latest / "trend.json"));
    EXPECT_EQ(history.at("observations").size(), 2);
    const auto timing = json::parse(tk::read_file(fixture.home.root() / "timings.json"));
    EXPECT_EQ(timing.at("linux:xdev:unit/slow"), 4500);
    const auto [failed, failure] = report("3", 99999, "fail", latest / "trend.json");
    EXPECT_EQ(failed.exit_code, 1) << failed.transcript();
    EXPECT_NE(failed.out.find("both passing and failing"), std::string::npos) << failed.out;
    EXPECT_EQ(json::parse(tk::read_file(fixture.home.root() / "timings.json")), timing)
        << "failed duration never becomes a shard weight";
    // Re-render the same execution; it must not manufacture another sample.
    const auto duplicate = fixture.run({"report", "--in", (fixture.home.root() / "input-3").string(),
        "--trend", (failure / "trend.json").string(), "--write", (fixture.home.root() / "repeat").string()});
    EXPECT_EQ(duplicate.exit_code, 1);
    EXPECT_EQ(json::parse(tk::read_file(fixture.home.root() / "repeat/trend.json"))["observations"].size(), 3);
    tk::write_file(fixture.home.root() / "tests/unit/slow.cpp", "import std;");
    const auto inventory = fixture.discovery({json{{"member", "xdev"}, {"test", "unit/slow"},
        {"main", "tests/unit/slow.cpp"}, {"cases", json::array({test_case("Slow.Check")})}}});
    const auto plan = fixture.run({"ci", "plan", "--platform", "linux", "--discovery", inventory.string(),
        "--timings", (fixture.home.root() / "timings.json").string()});
    ASSERT_EQ(plan.exit_code, 0) << plan.transcript();
    const auto matrix = json::parse(plan.out);
    EXPECT_EQ(matrix["include"][0]["estimated_ms"], 4500);
}
