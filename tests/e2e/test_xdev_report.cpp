#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(XdevReport, SkippedIsolationIsDeclaredButNotVerified,
      .area = "testkit", .covers = {"REPORT-EXECUTION-EVIDENCE"}) {
    const auto* binary = std::getenv("XDEV_BIN");
    if (!binary || !*binary) GTEST_SKIP() << "run through xdev test or set XDEV_BIN";
    auto home = tk::Home::isolated("report");
    const auto run = home.root() / "run";
    const auto requirements = home.root() / "requirements.toml";
    tk::write_file(requirements, "[ISOLATION]\nkind = \"isolation\"\nstatus = \"required\"\n");
    tk::write_file(run / "meta.ndjson",
        R"({"test":"probe","covers":["ISOLATION"],"proves":"isolation"})" "\n");
    tk::write_file(run / "cases.ndjson", R"({"test":"probe","status":"skip","message":"no sandbox"})" "\n");
    const auto report = [&](bool enforce) {
        std::vector<std::string> argv{binary, "report", "--in", run.string(),
            "--requirements", requirements.string(), "--fail-uncovered", "--write", run.string()};
        if (enforce) argv.push_back("--fail-unverified");
        return tk::run({.argv = std::move(argv)});
    };
    auto declared = report(false);
    ASSERT_EQ(declared.exit_code, 0) << declared.transcript();
    auto evidence = nlohmann::json::parse(tk::read_file(run / "report.json"));
    EXPECT_EQ(evidence["requirements"]["ISOLATION"]["declared_by"].size(), 1);
    EXPECT_TRUE(evidence["requirements"]["ISOLATION"]["passed_by"].empty());
    EXPECT_NE(report(true).exit_code, 0);

    tk::write_file(run / "cases.ndjson", R"({"test":"probe","status":"pass"})" "\n");
    auto passed = report(true);
    ASSERT_EQ(passed.exit_code, 0) << passed.transcript();
    evidence = nlohmann::json::parse(tk::read_file(run / "report.json"));
    EXPECT_EQ(evidence["requirements"]["ISOLATION"]["passed_by"].size(), 1);

    tk::write_file(run / "meta.ndjson",
        R"({"test":"probe","covers":["ISOLATION"],"proves":"flow"})" "\n");
    EXPECT_NE(report(true).exit_code, 0) << "a passing flow cannot verify isolation";
}
