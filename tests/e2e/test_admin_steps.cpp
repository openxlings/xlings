// A recipe declares what only an administrator may do; xlings settles it
// (Luban OS part 3 §8.2): asked once at a terminal, through the one door, or
// -- when nobody can answer -- kept, said, and taken by `self doctor --fix`.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(AdminSteps, AnInstallNobodyCanAnswerFinishesAndKeepsWhatNeedsRoot, .area = "xim",
      .covers = {"RECIPE-ADMIN-STEP", "ELEVATION-NOBODY-TO-ASK"}, .requires_ = {"xlings-bin", "posix"}) {
    auto home = tk::Home::isolated("admin-steps");
    const auto repo = home.root() / "index";
    const auto primary = home.root() / "primary";
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    const auto marker = home.root() / "made-by-root";
    tk::write_file(repo / "pkgs/s/sandboxed.lua", std::format(R"LUA(
        package = {{ spec = '1', name = 'sandboxed', type = 'package', archs = {{'x86_64', 'aarch64'}},
            programs = {{'sandboxed'}}, xpm = {{linux = {{['1.0.0'] = {{}}}}, macosx = {{['1.0.0'] = {{}}}}}} }}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.system')
        import('xim.libxpkg.xvm')
        function install()
            local dir = pkginfo.install_dir()
            os.mkdir(path.join(dir, 'bin'))
            io.writefile(path.join(dir, 'bin', 'sandboxed'), '#!/bin/sh\necho sandboxed\n')
            system.admin_step({{'touch', '{}'}}, 'its helper must be root-owned')
            return true
        end
        function config()
            xvm.add('sandboxed', {{bindir = path.join(pkginfo.install_dir(), 'bin')}})
            return true
        end
    )LUA", marker.generic_string()));
    const nlohmann::json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                                                 {"url", primary.generic_string()}, {"source", "git"}},
                                                {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    // Nobody to ask (agent mode): installed, nothing run, the step kept and said.
    const std::map<std::string, std::string> agent{{"XLINGS_AGENT_MODE", "1"}};
    auto installed = home.xlings({"install", "fixture:sandboxed@1.0.0", "-y"}, agent);
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    EXPECT_FALSE(fs::exists(marker)) << "a recipe never takes the step itself";
    EXPECT_NE(installed.transcript().find("its helper must be root-owned"), std::string::npos) << installed.transcript();
    EXPECT_NE(installed.transcript().find("xlings self doctor --fix"), std::string::npos) << installed.transcript();
    const auto waiting = nlohmann::json::parse(tk::read_file(home.dir() / "state/admin-steps.json"));
    ASSERT_EQ(waiting.size(), 1u);
    EXPECT_EQ(waiting[0]["argv"][0], "touch");
    // Doctor says it waits, every time, until it is taken.
    auto doctor = home.xlings({"self", "doctor"}, agent);
    EXPECT_NE(doctor.transcript().find("wait for administrator rights"), std::string::npos) << doctor.transcript();
}
