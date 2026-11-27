// The verbs 2026.9.12.1 gave the command line only (#593, design 2026-11-27
// X10) are capabilities too: remove's --all/--subos/--all-subos/--force as
// remove_package's fields, --list-xpkg/--remove-xpkg/--clear-xpkg as
// local_recipes, self doctor's --fix/--subos/--show-ok as self_doctor.
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

Json data_(const tk::RunResult& r, std::string_view kind) {
    for (const auto& line : r.json_lines())
        if (line.value("kind", "") == "data" && line.value("dataKind", "") == kind) {
            const auto& payload = line["payload"];
            return payload.is_string() ? Json::parse(payload.get<std::string>(), nullptr, false) : payload;
        }
    return Json();
}

}  // namespace

XTEST(InterfaceCliVerbs, TheCommandLinesRemoveOverlayAndDoctorVerbsAreCapabilities, .area = "interface",
      .covers = {"INTERFACE-CLI-VERBS"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("interface-cli-verbs");
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    const Json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                                      {"url", primary.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    // The fields and the capabilities, as a client reads them.
    const auto listed = Json::parse(home.xlings({"interface", "--list"}).out, nullptr, false);
    ASSERT_TRUE(listed.is_object());
    std::map<std::string, Json> schemas;
    for (const auto& c : listed["capabilities"])
        schemas[c.value("name", "")] = c.value("inputSchema", Json::object());
    for (const auto* field : {"force", "all", "subos", "older_than", "dry_run"})
        EXPECT_TRUE(schemas["remove_package"]["properties"].contains(field)) << field;
    ASSERT_TRUE(schemas.contains("local_recipes"));
    ASSERT_TRUE(schemas.contains("self_doctor"));
    for (const auto* field : {"fix", "dry_run", "deep", "show_ok", "subos"})
        EXPECT_TRUE(schemas["self_doctor"]["properties"].contains(field)) << field;

    // A local recipe, listed, then removed.
    const auto file = home.root() / "mine.lua";
    tk::write_file(file, R"LUA(
package = { spec = '1', name = 'mine', type = 'package', archs = { 'x86_64', 'aarch64' },
    xpm = { linux = { ['1.0.0'] = {} }, macosx = { ['1.0.0'] = {} }, windows = { ['1.0.0'] = {} } } }
)LUA");
    ASSERT_EQ(home.xlings({"config", "--add-xpkg", file.string()}).exit_code, 0);
    const auto list = home.xlings({"interface", "local_recipes", "--args", R"({"action":"list"})"});
    const auto recipes = data_(list, "local_recipes")["recipes"];
    ASSERT_EQ(recipes.size(), 1u) << list.transcript();
    EXPECT_EQ(recipes[0]["name"], "mine");
    EXPECT_EQ(recipes[0]["status"], "unique");
    const auto removed = home.xlings({"interface", "local_recipes", "--args", R"({"action":"remove","name":"mine"})"});
    EXPECT_NE(removed.out.find("\"exitCode\":0"), std::string::npos) << removed.transcript();
    const auto after = home.xlings({"interface", "local_recipes", "--args", R"({"action":"list"})"});
    EXPECT_TRUE(data_(after, "local_recipes")["recipes"].empty()) << after.transcript();

    // The doctor, scoped to a subos.
    const auto doctor = home.xlings({"interface", "self_doctor", "--args", R"({"subos":"default"})"});
    EXPECT_NE(doctor.out.find("\"exitCode\""), std::string::npos) << doctor.transcript();
}
