#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

nlohmann::json fixture_indexes(const fs::path& fixture) {
    const auto primary = fixture.parent_path() / "empty-primary-index";
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    return {{"mirror", "GLOBAL"},
            {"xim", {{"index-repo", primary.generic_string()}}},
            {"index_repos",
             nlohmann::json::array(
                 {{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                   {"url", primary.generic_string()},
                   {"source", "git"}},
                  {{"name", "fixture"}, {"url", fixture.generic_string()}, {"source", "git"}}})}};
}

// A package with the given versions. `withLib` gives it a lib/ directory: a
// consumer's record then carries libdirs (it links it).
void write_pkg(const fs::path& repo, std::string_view name,
               std::vector<std::string> versions, bool withLib,
               std::string_view dep = {}) {
    std::string keys;
    for (const auto& v : versions) keys += std::format("['{}'] = {{}}, ", v);
    const auto deps = dep.empty() ? std::string{} : std::format("deps = {{'{}'}}, ", dep);
    tk::write_file(repo / "pkgs" / name.substr(0, 1) / std::format("{}.lua", name),
                   std::format(R"LUA(
        package = {{
            spec = '1', name = '{0}', type = 'package', archs = {{'x86_64', 'aarch64'}},
            programs = {{'{0}'}},
            xpm = {{linux = {{{1}{2}}}, macosx = {{{1}{2}}}, windows = {{{1}{2}}}}}
        }}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            local dir = pkginfo.install_dir()
            os.mkdir(path.join(dir, 'bin'))
            io.writefile(path.join(dir, 'bin', '{0}'), '#!/bin/sh\necho {0}\n')
            {3}
            return true
        end
        function config()
            xvm.add('{0}', {{bindir = path.join(pkginfo.install_dir(), 'bin')}})
            return true
        end
        function uninstall()
            xvm.remove('{0}')
            return true
        end
    )LUA", name, deps, keys,
           withLib ? "os.mkdir(path.join(dir, 'lib')); io.writefile(path.join(dir, 'lib', 'lib.so'), 'x')"
                   : ""));
}

tk::Home seeded_home(std::string_view name, const std::function<void(const fs::path&)>& recipes) {
    auto home = tk::Home::isolated(name);
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    recipes(repo);
    tk::write_file(home.dir() / ".xlings.json", fixture_indexes(repo).dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(),
                  home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    EXPECT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    return home;
}

fs::path payload(const tk::Home& home, std::string_view name, std::string_view version) {
    return home.dir() / "data" / "xpkgs" / std::format("fixture-x-{}", name) / version;
}

}  // namespace

// The remove guard asks about the VERSION being removed. A dependent that
// resolved another version is not affected; one that only runs the target's
// program is satisfied by another installed version; only one that links the
// payload (or would be left with nothing) blocks.
XTEST(RemoveVersions, OnlyARemovalThatBreaksADependentIsRefused, .area = "xim",
      .covers = {"XIM-REMOVE-GUARD-VERSION"}, .requires_ = {"xlings-bin"}) {
    auto home = seeded_home("remove-guard-version", [](const fs::path& repo) {
        write_pkg(repo, "tool", {"1.0.0", "2.0.0"}, false);
        write_pkg(repo, "runner", {"1.0.0"}, false, "fixture:tool");
        write_pkg(repo, "libz", {"1.0.0", "2.0.0"}, true);
        write_pkg(repo, "linker", {"1.0.0"}, false, "fixture:libz@1.0.0");
    });
    for (const auto* t : {"fixture:tool@1.0.0", "fixture:tool@2.0.0", "fixture:runner@1.0.0",
                          "fixture:libz@2.0.0", "fixture:linker@1.0.0"}) {
        auto r = home.xlings({"install", t, "-y"});
        ASSERT_EQ(r.exit_code, 0) << t << "\n" << r.transcript();
    }

    // runner runs `tool` from PATH; the other version stays: allowed, and said.
    const auto ranOn = nlohmann::json::parse(
        tk::read_file(payload(home, "runner", "1.0.0") / ".xlings-resolution.json"));
    const auto recorded = ranOn["deps"][0]["version"].get<std::string>();
    const auto other = recorded == "1.0.0" ? "2.0.0" : "1.0.0";
    auto ok = home.xlings({"remove", std::format("fixture:tool@{}", recorded), "-y"});
    EXPECT_EQ(ok.exit_code, 0) << ok.transcript();
    EXPECT_NE(ok.transcript().find(std::format("{} remains", other)), std::string::npos)
        << ok.transcript();
    EXPECT_FALSE(fs::exists(payload(home, "tool", recorded))) << ok.transcript();

    // The last version: runner would have none. Refused, and why.
    auto last = home.xlings({"remove", std::format("fixture:tool@{}", other), "-y"});
    EXPECT_EQ(last.exit_code, 2) << last.transcript();
    EXPECT_NE(last.transcript().find("no other installed version"), std::string::npos)
        << last.transcript();

    // linker links libz 1.0.0: removing 2.0.0 is not its business ...
    auto unrelated = home.xlings({"remove", "fixture:libz@2.0.0", "-y"});
    EXPECT_EQ(unrelated.exit_code, 0) << unrelated.transcript();
    // ... removing 1.0.0 is, and the event says it is a link.
    auto linked = home.xlings({"--agent", "remove", "fixture:libz@1.0.0", "-y"});
    EXPECT_EQ(linked.exit_code, 2) << linked.transcript();
    EXPECT_NE(linked.transcript().find("links it"), std::string::npos) << linked.transcript();
    EXPECT_TRUE(fs::exists(payload(home, "libz", "1.0.0")));
}

// `remove <name> --older-than V` removes what is older and unused, and keeps
// -- saying why -- what is active somewhere. --dry-run and an agent without -y
// change nothing.
XTEST(RemoveVersions, OlderThanRemovesWhatNothingUsesAndSaysWhatStays, .area = "xim",
      .covers = {"XIM-REMOVE-OLDER-THAN"}, .requires_ = {"xlings-bin"}) {
    auto home = seeded_home("remove-older-than", [](const fs::path& repo) {
        write_pkg(repo, "tool", {"1.0.0", "1.1.0", "1.2.0", "2.0.0"}, false);
    });
    for (const auto* v : {"1.0.0", "1.1.0", "1.2.0", "2.0.0"}) {
        auto r = home.xlings({"install", std::format("fixture:tool@{}", v), "-y"});
        ASSERT_EQ(r.exit_code, 0) << v << "\n" << r.transcript();
    }
    ASSERT_EQ(home.xlings({"use", "tool", "1.1.0"}).exit_code, 0);
    auto made = home.xlings({"subos", "new", "other", "-y"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    auto there = home.xlings({"install", "fixture:tool@1.0.0", "--subos", "other", "-y"});
    ASSERT_EQ(there.exit_code, 0) << there.transcript();
    auto usedThere = home.xlings({"use", "tool", "1.0.0"}, {{"XLINGS_ACTIVE_SUBOS", "other"}});
    ASSERT_EQ(usedThere.exit_code, 0) << usedThere.transcript();

    auto dry = home.xlings({"remove", "tool", "--older-than", "2.0.0", "--dry-run"});
    EXPECT_EQ(dry.exit_code, 0) << dry.transcript();
    const auto plan = dry.transcript();
    EXPECT_NE(plan.find("remove  1.2.0"), std::string::npos) << plan;
    EXPECT_NE(plan.find("active in subos 'other'"), std::string::npos) << plan;
    EXPECT_NE(plan.find("active in subos 'default'"), std::string::npos) << plan;
    EXPECT_TRUE(fs::exists(payload(home, "tool", "1.2.0"))) << plan;

    auto agent = home.xlings({"--agent", "remove", "tool", "--older-than", "2.0.0"});
    EXPECT_EQ(agent.exit_code, 2) << agent.transcript();
    EXPECT_TRUE(fs::exists(payload(home, "tool", "1.2.0"))) << agent.transcript();

    auto done = home.xlings({"remove", "tool", "--older-than", "2.0.0", "-y"});
    EXPECT_EQ(done.exit_code, 0) << done.transcript();
    EXPECT_FALSE(fs::exists(payload(home, "tool", "1.2.0"))) << done.transcript();
    for (const auto* v : {"1.0.0", "1.1.0", "2.0.0"}) {
        EXPECT_TRUE(fs::exists(payload(home, "tool", v))) << v << "\n" << done.transcript();
    }
    const auto log = tk::read_file(home.dir() / "logs" / "destructive.ndjson");
    EXPECT_NE(log.find("--older-than 2.0.0"), std::string::npos) << log;
}
