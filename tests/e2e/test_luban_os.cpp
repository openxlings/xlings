// The Luban OS app (Luban OS architecture design §14), headless (SDL's
// offscreen driver): what a person sees first, with no system and with one;
// the management panel; and that it draws something.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::optional<fs::path> app_binary() {
    if (const char* forced = std::getenv("LUBAN_OS_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& c : {xlings.parent_path() / "luban-os", xlings.parent_path() / "luban-os" / "luban-os"})
        if (fs::is_regular_file(c)) return c;
    return std::nullopt;
}

tk::RunResult app(const tk::Home& home, std::vector<std::string> args) {
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "--headless", "--frames", "4", "--dump-state"};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    o.timeout = std::chrono::seconds(60);
    return tk::run(o);
}

Json state(const tk::RunResult& r) {
    std::istringstream in(r.out);
    Json last;
    for (std::string line; std::getline(in, line);)
        if (auto j = Json::parse(line, nullptr, false); j.is_object() && j.contains("view")) last = j;
    return last;
}

}  // namespace

XTEST(LubanOs, NoSystemYetIsQuickStartOneIsTheLauncherWithItsCountdown, .area = "luban", .covers = {"LUBAN-OS-APP"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test (mcpp build -p luban-os, or LUBAN_OS_BIN)";
    auto home = tk::Home::isolated("luban-os-first");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    auto first = app(home, {});
    ASSERT_EQ(first.exit_code, 0) << first.transcript();
    EXPECT_EQ(state(first).value("view", ""), "quick-start") << first.transcript();
    // One system: the launcher, counting down to it.
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto second = app(home, {});
    ASSERT_EQ(second.exit_code, 0) << second.transcript();
    const auto s = state(second);
    EXPECT_EQ(s.value("view", ""), "launcher") << second.transcript();
    EXPECT_EQ(s["systems"], Json::array({"box"})) << second.transcript();
    EXPECT_EQ(s.value("countdown_to", ""), "box") << second.transcript();
    // A countdown of 0 enters nothing by itself.
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0})");
    EXPECT_FALSE(state(app(home, {})).contains("countdown_to"));
}

XTEST(LubanOs, TheManagementPanelOpensAndTheWindowIsDrawn, .area = "luban", .covers = {"LUBAN-OS-APP"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-admin");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto shot = home.root() / "admin.ppm";
    auto r = app(home, {"--view", "admin", "--screenshot", shot.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(state(r).value("view", ""), "admin");
    const auto bytes = tk::read_file(shot);
    ASSERT_TRUE(bytes.starts_with("P6")) << "a screenshot";
    // More than one colour: the panel drew its controls.
    std::set<std::string> colours;
    const auto at = bytes.find("255\n") + 4;
    for (std::size_t k = at; k + 3 <= bytes.size() && colours.size() < 4; k += 3 * 101) colours.insert(bytes.substr(k, 3));
    EXPECT_GE(colours.size(), 3u);
}
