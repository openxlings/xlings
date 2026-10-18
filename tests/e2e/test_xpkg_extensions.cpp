// A recipe's extensions (xpkg spec, "extensions"; design 2026-10-19 §8):
// namespaced data for other programs, which xlings keeps and hands on as the
// recipe wrote it -- in package_info and in search results -- with the
// recipe's status beside it.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

nlohmann::json data_of(const tk::RunResult& r, std::string_view kind) {
    for (const auto& j : r.json_lines())
        if (j.value("kind", "") == "data" && j.value("dataKind", "") == kind) return j["payload"];
    return {};
}

}  // namespace

XTEST(XpkgExtensions, ARecipesExtensionsAndStatusAreHandedOnAsWritten, .area = "xim",
      .covers = {"XPKG-EXTENSIONS"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("xpkg-extensions");
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(primary / "pkgs/l/labwc.lua", R"LUA(
package = { spec = '1', name = 'labwc', namespace = 'luban', description = 'a desktop', type = 'package',
            status = 'stable', categories = {'luban-desktop'}, archs = {'x86_64', 'aarch64'},
            extensions = { luban = { tier = 2, title = { en = 'labwc', zh = 'labwc' }, highlights = { 'foot' } } },
            xpm = { linux = { ['1.0.0'] = {} }, macosx = { ['1.0.0'] = {} }, windows = { ['1.0.0'] = {} } } }
)LUA");
    tk::write_file(primary / "pkgs/p/plain.lua", R"LUA(
package = { spec = '1', name = 'plain', description = 'nothing extra', type = 'package', categories = {'luban-desktop'},
            archs = {'x86_64', 'aarch64'},
            xpm = { linux = { ['1.0.0'] = {} }, macosx = { ['1.0.0'] = {} }, windows = { ['1.0.0'] = {} } } }
)LUA");
    const nlohmann::json config{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array(
            {{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)}, {"url", primary.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    const auto info = home.xlings({"interface", "package_info", "--args", R"({"target":"luban:labwc"})"});
    ASSERT_EQ(info.exit_code, 0) << info.transcript();
    const auto pkg = data_of(info, "info_panel")["package"];
    EXPECT_EQ(pkg.value("status", ""), "stable") << pkg.dump();
    ASSERT_TRUE(pkg.contains("extensions")) << pkg.dump();
    EXPECT_EQ(pkg["extensions"]["luban"]["tier"], 2);
    EXPECT_EQ(pkg["extensions"]["luban"]["title"]["zh"], "labwc");
    EXPECT_EQ(pkg["extensions"]["luban"]["highlights"], nlohmann::json::array({"foot"}));

    const auto found = home.xlings({"interface", "search_packages", "--args", R"({"keyword":"","category":"luban-desktop"})"});
    ASSERT_EQ(found.exit_code, 0) << found.transcript();
    const auto packages = data_of(found, "packages");
    ASSERT_TRUE(packages.is_array()) << found.out;
    bool sawLabwc = false, sawPlain = false;
    for (const auto& p : packages) {
        if (p.value("name", "") == "labwc") {
            sawLabwc = true;
            EXPECT_EQ(p.value("status", ""), "stable");
            EXPECT_EQ(p["extensions"]["luban"]["tier"], 2) << p.dump();
        } else if (p.value("name", "") == "plain") {
            sawPlain = true;
            EXPECT_EQ(p.value("status", ""), "dev");
            EXPECT_FALSE(p.contains("extensions")) << "none written, none handed on: " << p.dump();
        }
    }
    EXPECT_TRUE(sawLabwc && sawPlain) << packages.dump();

    // A person's info shows them only with -v, by namespace.
    const auto quiet = home.xlings({"info", "luban:labwc"});
    EXPECT_EQ(quiet.out.find("extensions"), std::string::npos) << quiet.out;
    const auto verbose = home.xlings({"info", "luban:labwc", "-v"});
    EXPECT_NE(verbose.out.find("extensions"), std::string::npos) << verbose.out;
}
