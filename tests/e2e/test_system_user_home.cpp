// A user's xlings inside a Luban system (design 2026-10-19 §19, F18): the
// system's home is root's, a user's installs go to their own ~/.xlings. A
// command that only reads makes nothing and says nothing; the first write
// makes the user's home and says once how to install for every user -- from
// the host, never with sudo.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

XTEST(SystemUserHome, AReadOnlyCommandMakesNothingTheFirstWriteSaysHowOnce,
      .area = "subos", .covers = {"SUBOS-READONLY-USER", "SUBOS-USER-HOME-SEED"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban system is Linux";
    if (xlings::platform::is_root()) GTEST_SKIP() << "root writes the system's home itself";
    auto home = tk::Home::isolated("system-user-home");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // The system's index snapshot and where it gets packages from.
    tk::write_file(home.dir() / "data/xim-pkgindex/.xlings-index-version", "snapshot-1\n");
    tk::write_file(home.dir() / "data/xim-pkgindex/pkgs/h/hello.lua", "package = {}\n");
    {
        auto cfg = Json::parse(tk::read_file(home.dir() / ".xlings.json"), nullptr, false);
        if (!cfg.is_object()) cfg = Json::object();
        cfg["mirror"] = "CN";
        tk::write_file(home.dir() / ".xlings.json", cfg.dump());
    }
    // The system's home as a user meets it: root's, not theirs to write.
    fs::permissions(home.dir(), fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read
                    | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    const auto user = home.root() / "alice";
    fs::create_directories(user);
    const std::map<std::string, std::string> inside{{"XLINGS_ROOT_INSTANCE", "box"}, {"HOME", user.string()}};
    for (const std::vector<std::string> args : {std::vector<std::string>{"--version"}, {"list"}, {"info", "xlings"}}) {
        const auto r = home.xlings(args, inside);
        EXPECT_EQ(r.transcript().find("your own home"), std::string::npos) << args[0] << "\n" << r.transcript();
        EXPECT_EQ(r.transcript().find("sudo"), std::string::npos) << r.transcript();
        EXPECT_FALSE(fs::exists(user / ".xlings")) << "a read made a home: " << args[0];
    }
    // An agent is told the flag that says so; nothing is made.
    auto agentEnv = inside;
    agentEnv["XLINGS_AGENT_MODE"] = "1";
    const auto agent = home.xlings({"config", "--lang", "en"}, agentEnv);
    EXPECT_EQ(agent.exit_code, 2) << agent.transcript();
    EXPECT_NE(agent.err.find("-y"), std::string::npos) << agent.transcript();
    EXPECT_FALSE(fs::exists(user / ".xlings")) << "an agent's refused write made a home";
    const auto first = home.xlings({"config", "--lang", "en"}, inside);
    EXPECT_NE(first.transcript().find("your own home"), std::string::npos) << first.transcript();
    EXPECT_NE(first.transcript().find("luban run"), std::string::npos) << first.transcript();
    EXPECT_NE(first.transcript().find("--user root -- xlings install --system"), std::string::npos) << first.transcript();
    EXPECT_EQ(first.transcript().find("sudo"), std::string::npos) << first.transcript();
    EXPECT_TRUE(fs::exists(user / ".xlings"));
    // Started from the system's: its index snapshot and its mirror, no network.
    EXPECT_EQ(tk::read_file(user / ".xlings/data/xim-pkgindex/.xlings-index-version"), "snapshot-1\n");
    EXPECT_EQ(Json::parse(tk::read_file(user / ".xlings/.xlings.json"), nullptr, false).value("mirror", ""), "CN");
    const auto again = home.xlings({"--version"}, inside);
    EXPECT_EQ(again.transcript().find("your own home"), std::string::npos) << "said once\n" << again.transcript();
    fs::permissions(home.dir(), fs::perms::owner_all, fs::perm_options::add);
}

// What the system has at that version is every user's: a user asking for it
// is told so, and nothing is installed into their own home (design
// 2026-10-19 §27.5).
XTEST(SystemUserHome, WhatTheSystemHasIsNotInstalledAgainForAUser,
      .area = "subos", .covers = {"SUBOS-USER-HOME-SEED"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban system is Linux";
    if (xlings::platform::is_root()) GTEST_SKIP() << "root writes the system's home itself";
    auto home = tk::Home::isolated("system-user-has");
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/h/hello.lua", R"LUA(
package = { spec = '1', name = 'hello', archs = {'x86_64', 'aarch64'}, programs = {'hello'}, xvm_enable = true,
            xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, 'bin'))
    io.writefile(path.join(dir, 'bin', 'hello'), '#!/bin/sh\necho hello\n')
    return true
end
function config() xvm.add('hello', { bindir = path.join(pkginfo.install_dir(), 'bin') }); return true end
)LUA");
    const Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", repo.generic_string()}}},
                      {"index_repos", Json::array({{{"name", "xim"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto installed = home.xlings({"install", "hello@1.0.0", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    fs::permissions(home.dir(), fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read
                    | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    const auto user = home.root() / "alice";
    fs::create_directories(user);
    const std::map<std::string, std::string> inside{{"XLINGS_ROOT_INSTANCE", "box"}, {"HOME", user.string()}};
    const auto again = home.xlings({"install", "hello@1.0.0", "-y"}, inside);
    fs::permissions(home.dir(), fs::perms::owner_all, fs::perm_options::add);
    EXPECT_EQ(again.exit_code, 0) << again.transcript();
    EXPECT_NE(again.out.find("installed for every user already"), std::string::npos) << again.transcript();
    EXPECT_FALSE(fs::exists(user / ".xlings/data/xpkgs/xim-x-hello")) << "a second copy for one user";
}
