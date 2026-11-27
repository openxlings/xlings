// Aliases (design 2026-11-27 §7.3, #709): a key of the platform table with a
// `ref` that is no version -- `lts` -- installs the version it names, followed
// through a chain; the request is remembered, and `update` moves the package
// along its alias, not along `latest`. A version asked for afterwards ends it.
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

// `tool` at the given versions, `lts` and `latest` naming two of them, and
// `stable` naming `lts`.
void recipe_(const fs::path& index, std::string_view versions, std::string_view lts, std::string_view latest) {
    const auto table = std::format("{{ {0} ['lts'] = {{ ref = '{1}' }}, ['stable'] = {{ ref = 'lts' }}, "
                                   "['latest'] = {{ ref = '{2}' }} }}", versions, lts, latest);
    tk::write_file(index / "pkgs" / "t" / "tool.lua", std::format(R"LUA(
package = {{
    spec = '1', name = 'tool', type = 'package', archs = {{ 'x86_64', 'aarch64' }},
    xpm = {{ linux = {0}, macosx = {0}, windows = {0} }},
}}
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    os.mkdir(path.join(pkginfo.install_dir(), 'bin'))
    io.writefile(path.join(pkginfo.install_dir(), 'bin', 'tool'), '#!/bin/sh\necho ' .. pkginfo.version() .. '\n')
    return true
end
function config()
    xvm.add('tool', {{ bindir = path.join(pkginfo.install_dir(), 'bin') }})
    return true
end
)LUA", table));
}

std::string alias_(const tk::Home& home) {
    const auto doc = Json::parse(tk::read_file(home.dir() / "subos" / "default" / ".xlings.json"));
    return doc.value("aliases", Json::object()).value("fixture:tool", std::string{});
}

}  // namespace

XTEST(VersionAlias, AnAliasInstallsIsRememberedAndUpdateFollowsIt, .area = "xim",
      .covers = {"VERSION-ALIAS"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("version-alias");
    const auto primary = home.root() / "primary", index = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    recipe_(index, "['1.0.0'] = {}, ['2.0.0'] = {}, ['3.0.0'] = {},", "2.0.0", "3.0.0");
    const Json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", index.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto payload = [&](std::string_view v) {
        return home.dir() / "data" / "xpkgs" / "fixture-x-tool" / std::string(v);
    };

    const auto lts = home.xlings({"install", "fixture:tool@lts", "-y"});
    ASSERT_EQ(lts.exit_code, 0) << lts.transcript();
    EXPECT_TRUE(fs::exists(payload("2.0.0"))) << lts.transcript();
    EXPECT_FALSE(fs::exists(payload("3.0.0"))) << lts.transcript();
    EXPECT_EQ(alias_(home), "lts");

    // The index moves `lts`; `update` follows it, not `latest`.
    recipe_(index, "['1.0.0'] = {}, ['2.0.0'] = {}, ['2.1.0'] = {}, ['3.0.0'] = {},", "2.1.0", "3.0.0");
    const auto moved = home.xlings({"update", "fixture:tool", "-y"});
    ASSERT_EQ(moved.exit_code, 0) << moved.transcript();
    EXPECT_TRUE(fs::exists(payload("2.1.0"))) << moved.transcript();
    EXPECT_FALSE(fs::exists(payload("3.0.0"))) << moved.transcript();
    EXPECT_EQ(alias_(home), "lts") << moved.transcript();

    // A chain of aliases ends at a version.
    const auto stable = home.xlings({"install", "fixture:tool@stable", "-y"});
    ASSERT_EQ(stable.exit_code, 0) << stable.transcript();
    EXPECT_EQ(alias_(home), "stable");

    // A version asked for ends the alias: `update` follows `latest` again.
    ASSERT_EQ(home.xlings({"install", "fixture:tool@1.0.0", "-y"}).exit_code, 0);
    EXPECT_EQ(alias_(home), "");
    const auto latest = home.xlings({"update", "fixture:tool", "-y"});
    ASSERT_EQ(latest.exit_code, 0) << latest.transcript();
    EXPECT_TRUE(fs::exists(payload("3.0.0"))) << latest.transcript();
}
