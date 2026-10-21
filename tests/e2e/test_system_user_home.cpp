// A user's xlings inside a Luban system (design 2026-10-19 §19, F18;
// 2026-10-20 §4): the system's home is root's, a user's ~/.xlings is a view of
// it. A command that only reads makes nothing and says nothing; the first
// write makes the view -- its own settings, the system's index read in place
// -- and asks nothing: making it installs nothing.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

XTEST(SystemUserHome, AReadOnlyCommandMakesNothingTheFirstWriteMakesTheView,
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
    // The first write -- an agent's too -- makes the view without a question:
    // the system's mirror, the system's index read where it is (no copy).
    auto agentEnv = inside;
    agentEnv["XLINGS_AGENT_MODE"] = "1";
    const auto first = home.xlings({"config", "--lang", "en"}, agentEnv);
    EXPECT_EQ(first.exit_code, 0) << first.transcript();
    EXPECT_EQ(first.transcript().find("[Y/n]"), std::string::npos) << first.transcript();
    EXPECT_TRUE(fs::exists(user / ".xlings"));
    EXPECT_FALSE(fs::exists(user / ".xlings/data/xim-pkgindex")) << "an index of its own";
    EXPECT_EQ(Json::parse(tk::read_file(user / ".xlings/.xlings.json"), nullptr, false).value("mirror", ""), "CN");
    fs::permissions(home.dir(), fs::perms::owner_all, fs::perm_options::add);
}

// What the system has is every user's: a user asking for it is told so, and
// nothing is installed into their own home (design 2026-10-19 §27.5;
// 2026-10-20 §4.3).
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
    EXPECT_NE(again.out.find("hello 1.0.0 is provided by the system"), std::string::npos) << again.transcript();
    EXPECT_FALSE(fs::exists(user / ".xlings/data/xpkgs/xim-x-hello")) << "a second copy for one user";
}

// A user's view that has made nothing of its own active runs what the
// system default has (design 2026-10-20 §4.2; WuXian §13.3, H25): the
// system's program through the system's shim, not "not installed in this
// subos"; and self doctor finds nothing wrong with a view that only inherits.
XTEST(SystemUserHome, AViewWithNothingOfItsOwnRunsTheSystemDefaults,
      .area = "subos", .covers = {"SUBOS-USER-VIEW-INHERITS"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban system is Linux";
    if (xlings::platform::is_root()) GTEST_SKIP() << "root writes the system's home itself";
    auto home = tk::Home::isolated("system-user-inherits");
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
    io.writefile(path.join(dir, 'bin', 'hello'), '#!/bin/sh\necho hello-from-the-system\n')
    os.exec('chmod +x ' .. path.join(dir, 'bin', 'hello'))
    return true
end
function config() xvm.add('hello', { bindir = path.join(pkginfo.install_dir(), 'bin') }); return true end
)LUA");
    const Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", repo.generic_string()}}},
                      {"index_repos", Json::array({{{"name", "xim"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    // Its entry, which the shims are of.
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / "xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // The system (box) has it, as a system's own subos does.
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto installed = home.xlings({"install", "hello@1.0.0", "-y"}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    std::optional<fs::path> shim;
    for (const auto& dir : {home.dir() / "subos" / "box" / "bin", home.dir() / "bin"})
        if (fs::exists(dir / "hello")) { shim = dir / "hello"; break; }
    if (!shim)
        for (const auto& e : fs::recursive_directory_iterator(home.dir()))
            if (e.path().filename() == "hello") std::println(stderr, "hello at {}", e.path().string());
    ASSERT_TRUE(shim) << "no shim for hello\n" << installed.transcript();
    fs::permissions(home.dir(), fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read
                    | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    const auto user = home.root() / "alice";
    fs::create_directories(user);
    const std::map<std::string, std::string> inside{{"XLINGS_ROOT_INSTANCE", "box"}, {"HOME", user.string()}};
    // The view made (a first write), with nothing of its own.
    EXPECT_EQ(home.xlings({"config", "--lang", "en"}, inside).exit_code, 0);
    ASSERT_TRUE(fs::exists(user / ".xlings"));
    tk::RunOptions o;
    o.argv = {shim->string()};
    o.env = home.env();
    for (const auto& [k, v] : inside) o.env[k] = v;
    // As a session has it once the view is made: its own home named, the
    // system's below it.
    o.env["XLINGS_HOME"] = (user / ".xlings").string();
    o.env["XLINGS_SYSTEM_LAYER"] = home.dir().string();
    const auto ran = tk::run(o);
    const auto doctor = home.xlings({"self", "doctor"}, inside);
    fs::permissions(home.dir(), fs::perms::owner_all, fs::perm_options::add);
    EXPECT_EQ(ran.exit_code, 0) << ran.transcript();
    EXPECT_NE(ran.out.find("hello-from-the-system"), std::string::npos) << ran.transcript();
    EXPECT_EQ(ran.transcript().find("not installed"), std::string::npos) << ran.transcript();
    EXPECT_EQ(doctor.transcript().find("subos manifest"), std::string::npos) << doctor.transcript();
}

// Inside a system its own subos is the system (design 2026-10-16 L §9): with
// no XLINGS_ACTIVE_SUBOS -- a machine's session -- the workspace read is the
// system's, not the home's `default`. 2026.10.21.1's machines said "dsh is
// not installed in this subos (e-desktop)" of a program the system had.
XTEST(SystemUserHome, AShimInASystemReadsTheSystemsOwnWorkspace,
      .area = "subos", .covers = {"SUBOS-ROOT-INSTANCE-WORKSPACE"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban system is Linux";
    auto home = tk::Home::isolated("system-own-workspace");
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
    io.writefile(path.join(dir, 'bin', 'hello'), '#!/bin/sh\necho hello-from-box\n')
    os.exec('chmod +x ' .. path.join(dir, 'bin', 'hello'))
    return true
end
function config() xvm.add('hello', { bindir = path.join(pkginfo.install_dir(), 'bin') }); return true end
)LUA");
    const Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", repo.generic_string()}}},
                      {"index_repos", Json::array({{{"name", "xim"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / "xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto installed = home.xlings({"install", "hello@1.0.0", "-y"}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto shim = home.dir() / "subos" / "box" / "bin" / "hello";
    ASSERT_TRUE(fs::exists(shim)) << installed.transcript();
    // The home's own choice stays `default`; the system names itself.
    tk::RunOptions o;
    o.argv = {shim.string()};
    o.env = home.env();
    o.env.erase("XLINGS_ACTIVE_SUBOS");
    o.env["XLINGS_ROOT_INSTANCE"] = "box";
    const auto ran = tk::run(o);
    EXPECT_EQ(ran.exit_code, 0) << ran.transcript();
    EXPECT_NE(ran.out.find("hello-from-box"), std::string::npos) << ran.transcript();
    EXPECT_EQ(ran.transcript().find("not installed"), std::string::npos) << ran.transcript();
}

// No network inside a system: the index cannot be fetched, said once with
// where to look -- the host, for a Luban system -- and quickly: no clone, no
// git bootstrapped to clone with (design 2026-10-19 §27.4, F25: the same
// failure five times over minutes).
XTEST(SystemUserHome, NoNetworkIsSaidOnceWithWhereToLook,
      .area = "subos", .covers = {"LUBAN-MACHINE-NETWORK"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban system is Linux";
    if (std::system("unshare -rn true >/dev/null 2>&1") != 0) GTEST_SKIP() << "no unprivileged network namespace here";
    auto home = tk::Home::isolated("system-no-network");
    // The official index, remote, and none of it here: as a fresh home is.
    tk::write_file(home.dir() / ".xlings.json", R"({"mirror": "GLOBAL"})");
    std::error_code ec;
    fs::remove_all(home.dir() / "data/xim-pkgindex", ec);
    tk::RunOptions o;
    o.argv = {"unshare", "-rn", tk::xlings_binary().string(), "install", "nvim", "-y"};
    o.env = home.env();
    o.env["XLINGS_ROOT_INSTANCE"] = "box";
    o.timeout = std::chrono::seconds(60);
    const auto r = tk::run(o);
    EXPECT_NE(r.exit_code, 0);
    const auto said = r.transcript();
    EXPECT_NE(said.find("no network"), std::string::npos) << said;
    EXPECT_NE(said.find("luban status box --check"), std::string::npos) << "the host is where to look\n" << said;
    EXPECT_EQ(said.find("git is not available"), std::string::npos) << said;
    EXPECT_EQ(said.find("no network"), said.rfind("no network")) << "said once\n" << said;
    EXPECT_LT(r.elapsed, std::chrono::seconds(20));
}
