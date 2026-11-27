// A request for an exact entry key is that version (design 2026-11-27 §7.1,
// #739): the index has 2.44, 2.44.2 and 2.44.3, and a request for 2.44 is not
// answered by an active or planned 2.44.x. A range is still met by what is
// active.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

tk::Home home_(std::string_view name) {
    auto home = tk::Home::isolated(name);
    const auto primary = home.root() / "primary", index = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(index / "pkgs" / "r" / "rt.lua", R"LUA(
package = {
    spec = '1', name = 'rt', type = 'package', archs = { 'x86_64', 'aarch64' },
    xpm = {
        linux = { ['2.44'] = {}, ['2.44.2'] = {}, ['2.44.3'] = {} },
        macosx = { ['2.44'] = {}, ['2.44.2'] = {}, ['2.44.3'] = {} },
        windows = { ['2.44'] = {}, ['2.44.2'] = {}, ['2.44.3'] = {} },
    },
}
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    os.mkdir(path.join(pkginfo.install_dir(), 'bin'))
    io.writefile(path.join(pkginfo.install_dir(), 'bin', 'rt'), '#!/bin/sh\necho ' .. pkginfo.version() .. '\n')
    io.writefile(path.join(pkginfo.install_dir(), 'installed-version'), pkginfo.version())
    return true
end
function config()
    xvm.add('rt', { bindir = path.join(pkginfo.install_dir(), 'bin') })
    return true
end
)LUA");
    const nlohmann::json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", index.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    const auto init = home.xlings({"self", "init"});
    EXPECT_EQ(init.exit_code, 0) << init.transcript();
    return home;
}

fs::path payload_(const tk::Home& home, std::string_view version) {
    return home.dir() / "data" / "xpkgs" / "fixture-x-rt" / std::string(version);
}

}  // namespace

XTEST(VersionExactKey, AnActiveLongerVersionDoesNotAnswerTheExactEntry, .area = "xim",
      .covers = {"VERSION-EXACT-KEY"}, .requires_ = {"xlings-bin"}) {
    auto home = home_("version-exact-active");
    const auto first = home.xlings({"install", "fixture:rt@2.44.2", "-y"});
    ASSERT_EQ(first.exit_code, 0) << first.transcript();
    const auto exact = home.xlings({"install", "fixture:rt@2.44", "-y"});
    EXPECT_EQ(exact.exit_code, 0) << exact.transcript();
    EXPECT_EQ(exact.transcript().find("already installed"), std::string::npos) << exact.transcript();
    EXPECT_EQ(tk::read_file(payload_(home, "2.44") / "installed-version"), "2.44") << exact.transcript();

    // A range is still met by what is active: nothing to do.
    const auto range = home.xlings({"install", "fixture:rt@~2.44", "-y"});
    EXPECT_EQ(range.exit_code, 0) << range.transcript();
    EXPECT_FALSE(fs::exists(payload_(home, "2.44.3"))) << range.transcript();
}

XTEST(VersionExactKey, TheExactEntryAndALongerOneInOneCommandAreTwoPayloads, .area = "xim",
      .covers = {"VERSION-EXACT-KEY"}, .requires_ = {"xlings-bin"}) {
    auto home = home_("version-exact-plan");
    const auto both = home.xlings({"install", "fixture:rt@2.44", "fixture:rt@2.44.3", "-y"});
    ASSERT_EQ(both.exit_code, 0) << both.transcript();
    EXPECT_EQ(tk::read_file(payload_(home, "2.44") / "installed-version"), "2.44") << both.transcript();
    EXPECT_EQ(tk::read_file(payload_(home, "2.44.3") / "installed-version"), "2.44.3") << both.transcript();
}
