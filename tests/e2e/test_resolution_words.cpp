// What resolving a name says (design 2026-10-20 WuXian §13.2, H26): a recipe
// that is there and cannot be read is said as that, with why -- never "not
// in the index"; a name that matches several packages lists them nearest
// first, ten at most, and offers only a full coordinate, never "take the
// first" (which installed packages nobody named).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

std::string recipe_(std::string_view name) {
    return std::format(R"LUA(
package = {{ spec = '1', name = '{}', type = 'script', archs = {{'x86_64', 'aarch64'}},
            xpm = {{ linux = {{['1.0.0'] = {{}}}}, macosx = {{['1.0.0'] = {{}}}}, windows = {{['1.0.0'] = {{}}}} }} }}
function xpkg_main() return true end
)LUA", name);
}

tk::Home home_with_(std::string_view tag, const std::vector<std::string>& names) {
    auto home = tk::Home::isolated(tag);
    const auto repo = home.root() / "index", primary = home.root() / "empty-primary-index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    for (const auto& n : names) tk::write_file(repo / "pkgs" / n.substr(0, 1) / (n + ".lua"), recipe_(n));
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    const nlohmann::json config{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                                                {"url", primary.generic_string()}, {"source", "git"}},
                                               {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    return home;
}

}  // namespace

XTEST(ResolutionWords, ARecipeThatCannotBeReadIsSaidAsThatNotAsMissing, .area = "xim",
      .covers = {"XIM-UNREADABLE-RECIPE"}, .requires_ = {"xlings-bin"}) {
    auto home = home_with_("resolution-unreadable", {"widget"});
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"info", "fixture:widget"}).exit_code, 0) << "read, before it breaks";
    // The index (a directory read where it is), its catalog kept as an
    // artifact index's is -- by its version marker -- and the recipe in it
    // broken: the index has the name, the recipe cannot be read.
    const auto index_dir = home.root() / "index";
    const auto copy = index_dir / "pkgs" / "w" / "widget.lua";
    tk::write_file(index_dir / ".xlings-index-version", "fixture-1\n");
    ASSERT_EQ(home.xlings({"info", "fixture:widget"}).exit_code, 0) << "its catalog kept by the marker";
    tk::write_file(copy, "package = { this is not Lua");
    const auto info = home.xlings({"info", "fixture:widget"});
    EXPECT_NE(info.exit_code, 0) << info.transcript();
    EXPECT_EQ(info.transcript().find("not found"), std::string::npos) << info.transcript();
    EXPECT_NE(info.transcript().find("the recipe of 'fixture:widget' could not be read"), std::string::npos) << info.transcript();
    EXPECT_NE(info.transcript().find("xlings self doctor"), std::string::npos) << info.transcript();
    const auto install = home.xlings({"install", "fixture:widget", "-y", "--agent"});
    EXPECT_NE(install.exit_code, 0) << install.transcript();
    EXPECT_NE(install.transcript().find("could not be read"), std::string::npos) << install.transcript();
    EXPECT_EQ(install.transcript().find("matches more than one"), std::string::npos) << install.transcript();
}

XTEST(ResolutionWords, AmbiguousNamesAreListedNearestFirstAndOnlyAFullCoordinateIsOffered, .area = "xim",
      .covers = {"XIM-FUZZY-ORDER"}, .requires_ = {"xlings-bin"}) {
    std::vector<std::string> names{"zz-widgetry", "awidget", "widget-tools", "my-widget-kit", "widgetor"};
    for (int k = 0; k < 9; ++k) names.push_back(std::format("other-widget-{}", k));
    auto home = home_with_("resolution-fuzzy", names);
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto r = home.xlings({"install", "widge", "--agent"});
    EXPECT_EQ(r.exit_code, 2) << r.transcript();
    const auto out = r.transcript();
    // Names it begins before names holding it, and the count of the rest.
    const auto first = out.find("fixture:widget-tools"), later = out.find("fixture:awidget");
    ASSERT_NE(first, std::string::npos) << out;
    ASSERT_NE(later, std::string::npos) << out;
    EXPECT_LT(first, later) << out;
    EXPECT_NE(out.find("xlings search widge"), std::string::npos) << "more than ten: the others counted\n" << out;
    EXPECT_EQ(out.find("-y"), std::string::npos) << "no \"take the first\"\n" << out;
    EXPECT_NE(out.find("xlings install fixture:widget"), std::string::npos) << out;
}
