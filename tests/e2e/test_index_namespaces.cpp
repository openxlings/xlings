// Namespaces an index reserves (design 2026-10-18 §4.2): a package another
// index publishes in one is not taken, and is named with its recipe; packages
// in the index's own namespaces, and anyone's elsewhere, are as before.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

std::string recipe(std::string_view name, std::string_view ns, std::string_view description) {
    return std::format(R"LUA(
package = {{ spec = '1', name = '{}', namespace = '{}', description = '{}', type = 'package',
            archs = {{'x86_64', 'aarch64'}},
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }}, macosx = {{ ['1.0.0'] = {{}} }}, windows = {{ ['1.0.0'] = {{}} }} }} }}
)LUA", name, ns, description);
}

}  // namespace

XTEST(IndexNamespaces, AReservedNamespaceIsTakenFromItsOwnIndexOnly, .area = "xim",
      .covers = {"LUBAN-NAMESPACE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("index-namespaces");
    const auto primary = home.root() / "primary", other = home.root() / "other";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(primary / "index-compat.json", R"({"namespaces": {"reserved": ["luban"]}})");
    tk::write_file(primary / "pkgs/luban/core.lua", recipe("core", "luban", "the official one"));
    tk::write_file(other / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(other / "pkgs/luban/desktop.lua", recipe("desktop", "luban", "a squatter"));
    tk::write_file(other / "pkgs/m/myos.lua", recipe("myos", "other", "its own"));
    const nlohmann::json config{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array(
            {{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)}, {"url", primary.generic_string()}, {"source", "git"}},
             {{"name", "other"}, {"url", other.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    auto own = home.xlings({"info", "luban:core"});
    EXPECT_EQ(own.exit_code, 0) << own.transcript();
    EXPECT_NE(own.out.find("the official one"), std::string::npos) << own.transcript();

    auto squat = home.xlings({"info", "luban:desktop"});
    EXPECT_NE(squat.exit_code, 0) << "another index's package in a reserved namespace\n" << squat.transcript();
    const auto said = squat.transcript();
    EXPECT_NE(said.find("luban:desktop"), std::string::npos) << said;
    EXPECT_NE(said.find("desktop.lua"), std::string::npos) << "the recipe is named\n" << said;
    EXPECT_NE(said.find("namespace 'luban'"), std::string::npos) << said;

    auto theirs = home.xlings({"info", "other:myos"});
    EXPECT_EQ(theirs.exit_code, 0) << theirs.transcript();
    EXPECT_NE(theirs.out.find("its own"), std::string::npos) << theirs.transcript();
}

// What a coordinate is, as data (design 2026-10-18 §5.3): package_info says
// its namespace, index, type and categories; a search by category finds what
// a recipe lists (Luban's desktops: luban-desktop), for a person and a program.
XTEST(IndexNamespaces, APackageIsDescribedAsDataAndFoundByCategory, .area = "xim",
      .covers = {"LUBAN-DIST-XPKG"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("index-categories");
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    auto with = [](std::string_view name, std::string_view type, std::string_view categories) {
        return std::format(R"LUA(
package = {{ spec = '1', name = '{}', namespace = 'luban', description = 'the {}', type = '{}',
            categories = {{ {} }}, archs = {{'x86_64', 'aarch64'}},
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }}, macosx = {{ ['1.0.0'] = {{}} }}, windows = {{ ['1.0.0'] = {{}} }} }} }}
)LUA", name, name, type, categories);
    };
    tk::write_file(primary / "pkgs/luban/labwc.lua", with("labwc", "package", "'desktop', 'luban-desktop'"));
    tk::write_file(primary / "pkgs/luban/kde.lua", with("kde", "package", "'luban-desktop'"));
    tk::write_file(primary / "pkgs/luban/foot.lua", with("foot", "package", "'terminal'"));
    tk::write_file(primary / "pkgs/luban/core.lua", with("core", "subos", "'subos'"));
    const nlohmann::json config{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array(
            {{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)}, {"url", primary.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    auto desktops = home.xlings({"search", "--category", "luban-desktop"});
    ASSERT_EQ(desktops.exit_code, 0) << desktops.transcript();
    EXPECT_NE(desktops.out.find("luban:labwc"), std::string::npos) << desktops.out;
    EXPECT_NE(desktops.out.find("luban:kde"), std::string::npos) << desktops.out;
    EXPECT_EQ(desktops.out.find("luban:foot"), std::string::npos) << desktops.out;
    EXPECT_NE(home.xlings({"search"}).exit_code, 0) << "neither a keyword nor a category";

    // The same through the interface, as data.
    auto found = home.xlings({"interface", "search_packages", "--args", R"({"keyword":"","category":"luban-desktop"})"});
    ASSERT_EQ(found.exit_code, 0) << found.transcript();
    EXPECT_NE(found.out.find("\"dataKind\":\"packages\""), std::string::npos) << found.out;
    auto info = home.xlings({"interface", "package_info", "--args", R"({"target":"luban:core"})"});
    ASSERT_EQ(info.exit_code, 0) << info.transcript();
    nlohmann::json described;
    std::istringstream in(info.out);
    for (std::string line; std::getline(in, line);) {
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_object() && j.value("dataKind", "") == "info_panel") {
            auto payload = j["payload"].is_string() ? nlohmann::json::parse(j["payload"].get<std::string>()) : j["payload"];
            described = payload["package"];
        }
    }
    ASSERT_TRUE(described.is_object()) << info.out;
    EXPECT_EQ(described["namespace"], "luban");
    EXPECT_EQ(described["type"], "subos");
    EXPECT_EQ(described["index"], std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME));
    EXPECT_EQ(described["categories"], nlohmann::json::array({"subos"}));
    EXPECT_EQ(described["version"], "1.0.0");
}
