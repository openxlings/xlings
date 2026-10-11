// A home the size of a real one: what changing ONE package costs there.
//
// Measured on a maintainer's home (2026-10-12): 3,986 targets in the version
// database, 2,606 of them active in one SubOS, 18 known projects. `use`,
// `install --use` and `remove` of a single package each took tens of seconds
// to minutes there before 2026.10.12.1, because every one of them redid the
// whole SubOS's projection. This home is generated (no network, small files)
// and each command has a budget, asserted in the static performance lane
// (XLINGS_PERF_BUDGETS=1); elsewhere the timings are printed. A failure
// carries the command's XLINGS_TRACE=timing lines, which name the phase.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr int kPackages = 50;
constexpr int kProgramsEach = 50;     // 2,500 active targets
constexpr int kProjects = 10;

std::string pkg(int i) { return std::format("p{:02}", i); }

void write_index(const fs::path& repo) {
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    for (int i = 0; i < kPackages; ++i) {
        tk::write_file(repo / "pkgs" / "p" / (pkg(i) + ".lua"), std::format(R"LUA(
package = {{ spec = '1', name = '{0}', type = 'package', archs = {{'x86_64', 'aarch64'}},
            xpm = {{ linux = {{ ['1.0.0'] = {{}}, ['2.0.0'] = {{}} }},
                    macosx = {{ ['1.0.0'] = {{}}, ['2.0.0'] = {{}} }} }} }}
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    local bin = path.join(pkginfo.install_dir(), 'bin')
    os.mkdir(bin)
    for i = 1, {1} do io.writefile(path.join(bin, '{0}-' .. i), '#!/bin/sh\n') end
    return true
end
function config()
    local bin = path.join(pkginfo.install_dir(), 'bin')
    local release = '{0}@' .. pkginfo.version()
    xvm.add('{0}')
    for i = 1, {1} do xvm.add('{0}-' .. i, {{ bindir = bin, binding = release }}) end
    return true
end
)LUA", pkg(i), kProgramsEach));
    }
}

struct Timing { std::string command; long long ms; std::string trace; };

}  // namespace

XTEST(HomePerf, ChangingOnePackageCostsWhatItMovesNotWhatTheHomeHolds, .area = "xim",
      .cost = tk::Cost::Slow, .covers = {"PERF-LARGE-HOME"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("home-perf");
    const auto repo = home.root() / "index";
    const auto primary = home.root() / "primary";
    write_index(repo);
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                                      {"url", primary.generic_string()}, {"source", "git"}},
                                     {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    // The home: every package at 1.0.0 (active) and 2.0.0, in two plans.
    for (const auto* version : {"1.0.0", "2.0.0"}) {
        std::vector<std::string> args{"install", "-y"};
        for (int i = 0; i < kPackages; ++i) args.push_back(std::format("fixture:{}@{}", pkg(i), version));
        auto made = home.xlings(args, {}, std::chrono::minutes(10));
        ASSERT_EQ(made.exit_code, 0) << made.transcript();
        std::cout << std::format("generated {} x {} programs at {} in {} ms\n", kPackages, kProgramsEach,
                                 version, made.elapsed.count());
    }
    for (int i = 0; i < kPackages; ++i)
        ASSERT_EQ(home.xlings({"use", pkg(i), "1.0.0"}).exit_code, 0);
    // Known projects: their routes are rebuilt with every change.
    auto state = Json::parse(tk::read_file(home.dir() / ".xlings.json"));
    for (int i = 0; i < kProjects; ++i) {
        const auto project = home.root() / "projects" / std::format("proj{}", i);
        tk::write_file(project / ".xlings.json", Json{{"workspace", Json::object()}}.dump());
        tk::write_file(project / ".xlings" / ".xlings.json", Json{{"workspace", Json::object()}}.dump());
        state["knownProjects"][project.string()] = Json{{"lastSeen", "2026-10-12"}};
    }
    tk::write_file(home.dir() / ".xlings.json", state.dump());

    // Each budget is for an unoptimised build on a loaded runner as much as
    // for the release: the bound is on growth with the home, which is what
    // regressed (72 s for `use` on a home this size).
    struct Case { std::vector<std::string> args; long long budgetMs; };
    const std::vector<Case> cases{
        {{"use", pkg(7), "2.0.0"}, 2000},
        {{"install", std::format("fixture:{}@2.0.0", pkg(8)), "--use", "-y"}, 3000},
        {{"remove", std::format("fixture:{}@2.0.0", pkg(9)), "-y"}, 2000},
        {{"remove", pkg(10), "--older-than", "2.0.0", "-y"}, 3000},
        {{"list"}, 2000},
        {{"self", "doctor"}, 10000},
    };
    std::vector<Timing> timings;
    for (const auto& c : cases) {
        auto r = home.xlings(c.args, {{"XLINGS_TRACE", "timing"}}, std::chrono::minutes(5));
        std::string line;
        for (const auto& a : c.args) line += (line.empty() ? "" : " ") + a;
        // doctor reports findings with a non-zero code; it ran is what counts.
        if (c.args[0] != "self") EXPECT_EQ(r.exit_code, 0) << line << "\n" << r.transcript();
        timings.push_back({line, r.elapsed.count(), r.err});
        std::cout << Json{{"perf", line}, {"ms", r.elapsed.count()}, {"budget_ms", c.budgetMs}}.dump() << '\n';
    }
    if (const char* on = std::getenv("XLINGS_PERF_BUDGETS"); !on || std::string_view(on) != "1")
        GTEST_SKIP() << "workload validated; the timing budgets are the static performance lane's";
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_LE(timings[i].ms, cases[i].budgetMs)
            << timings[i].command << " took " << timings[i].ms << " ms\n" << timings[i].trace;
    }
}
