// xdesk.config (Luban OS desktop part 2 §2.4, D6): a profile's settings and
// a person's own over them, key by key; what a file gets wrong said, not fatal.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xdesk.config;

namespace config = xdesk::config;
namespace fs = std::filesystem;

XTEST(XdeskConfig, AnIniIsReadAndWhatIsWrongIsSaidByLine, .area = "luban", .covers = {"XDESK-CONFIG"}) {
    const auto s = config::Settings::parse("# a comment\n[Desktop]\nterminal = foot -e fish\nautostart = a\nautostart = b\n"
                                           "gap=12\n[theme]\naccent = #7aa2f7\nbackground = blue\noops\n[broken\n", "t.ini");
    EXPECT_EQ(s.get("desktop", "terminal"), "foot -e fish");          // sections and keys by any case
    EXPECT_EQ(s.all("desktop", "autostart"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(s.get_int("desktop", "gap", 8), 12);
    EXPECT_EQ(s.get_int("desktop", "workspaces", 4), 4);
    EXPECT_EQ(s.colour("theme", "accent", 0), 0x7aa2f7u);
    EXPECT_EQ(s.colour("theme", "background", 0x123456), 0x123456u);   // not a colour: the fallback
    ASSERT_EQ(s.problems.size(), 2u);
    EXPECT_EQ(s.problems[0], "t.ini:10: not key = value");
    EXPECT_EQ(s.problems[1], "t.ini:11: a section without its ]");
}

XTEST(XdeskConfig, APersonsOwnFileWinsKeyByKeyOverTheProfiles, .area = "luban", .covers = {"XDESK-CONFIG"}) {
    const auto root = fs::temp_directory_path() / std::format("xdesk-config-{}", std::random_device{}());
    // $XDG_*_DIRS are ':'-separated POSIX paths (a drive letter's ':' is not
    // one of them): xdesk runs where they are.
    if (root.string().find(':') != std::string::npos) GTEST_SKIP() << "XDG directory lists are POSIX paths";
    fs::create_directories(root / "etc" / "xdesk");
    fs::create_directories(root / "share" / "xdesk");
    std::ofstream(root / "share" / "xdesk" / "canvas.ini") << "[desktop]\nterminal = from-share\nworkspaces = 6\n";
    fs::create_directories(root / "home" / ".config" / "xdesk");
    std::ofstream(root / "etc" / "xdesk" / "canvas.ini") << "[desktop]\nterminal = foot\nautostart = a\nautostart = b\n[theme]\naccent = #111111\n";
    std::ofstream(root / "home" / ".config" / "xdesk" / "canvas.ini") << "[theme]\naccent = #222222\n[desktop]\nautostart = mine\n";
    const std::map<std::string, std::string> env{{"XDG_DATA_DIRS", (root / "share").string()},
                                                 {"XDG_CONFIG_DIRS", (root / "etc").string()}, {"HOME", (root / "home").string()}};
    const auto files = config::files("canvas", env);
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files.front(), root / "share" / "xdesk" / "canvas.ini");             // a profile's defaults first
    EXPECT_EQ(files.back(), root / "home" / ".config" / "xdesk" / "canvas.ini");   // the person's last
    const auto s = config::load("canvas", env);
    EXPECT_EQ(s.get("desktop", "terminal"), "foot");                  // the system's over the profile's
    EXPECT_EQ(s.get_int("desktop", "workspaces", 4), 6);              // the profile's, nobody else set it
    EXPECT_EQ(s.colour("theme", "accent", 0), 0x222222u);             // theirs
    EXPECT_EQ(s.all("desktop", "autostart"), (std::vector<std::string>{"mine"}));   // their list, not added to
    EXPECT_TRUE(config::load("classic", env).get("desktop", "terminal").empty());   // another profile: nothing
    std::error_code ec;
    fs::remove_all(root, ec);
}

// One settings file for every session (design 2026-10-20 WuXian §12.2): what
// a person kept in their session's file of before becomes their xdesk.ini,
// once, and the old file stays as it was.
XTEST(XdeskConfig, APersonsSessionFileOfBeforeBecomesTheirXdeskIniOnce, .area = "luban", .covers = {"XDESK-CONFIG"}) {
    const auto root = fs::temp_directory_path() / std::format("xdesk-ini-{}", std::random_device{}());
    fs::create_directories(root / "xdesk");
    std::ofstream(root / "xdesk" / "classic.ini") << "[theme]\naccent = #123456\n";
    std::ofstream(root / "xdesk" / "canvas.ini") << "[theme]\naccent = #654321\n";
    const std::map<std::string, std::string> env{{"XDG_CONFIG_HOME", root.string()}, {"XDG_DATA_DIRS", (root / "none").string()},
                                                 {"XDG_CONFIG_DIRS", (root / "none").string()}};
    const auto s = config::load("xdesk", env);
    EXPECT_EQ(s.colour("theme", "accent", 0), 0x123456u) << "the default session's of before";
    EXPECT_TRUE(fs::is_regular_file(root / "xdesk" / "xdesk.ini"));
    EXPECT_TRUE(fs::is_regular_file(root / "xdesk" / "classic.ini")) << "the old file is kept";
    // Once: what is set now is xdesk.ini's, the old file not read again.
    ASSERT_TRUE(config::set_value("xdesk", env, "theme", "accent", "#abcdef"));
    std::ofstream(root / "xdesk" / "classic.ini") << "[theme]\naccent = #000000\n";
    EXPECT_EQ(config::load("xdesk", env).colour("theme", "accent", 0), 0xabcdefu);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// state.json and keys.json (design 2026-10-19 §16.4, §17.12): the person's,
// read and written whole; one that cannot be read is said and left as it is.
XTEST(XdeskConfig, ThePersonsStateAndKeysAreKeptAndABrokenOneIsSaid, .area = "luban", .covers = {"XDESK-CONFIG", "XDESK-ARRANGEMENTS", "WUXIAN-STATE"}) {
    const auto root = fs::temp_directory_path() / std::format("xdesk-state-{}", std::random_device{}());
    const std::map<std::string, std::string> env{{"XDG_CONFIG_HOME", root.string()}};
    // Nothing there: the defaults.
    auto s = config::read_state(env);
    ASSERT_TRUE(s);
    EXPECT_EQ(*s, config::State{});
    // Written and read back the same.
    config::State st{.rules = {{"foot", {.floating = true, .workspace = 2, .rect = std::array{10, 20, 640, 480}}}}};
    ASSERT_TRUE(config::write_state(env, st));
    s = config::read_state(env);
    ASSERT_TRUE(s) << s.error();
    EXPECT_EQ(*s, st);
    // The workspaces' arrangements of before (WuXian keeps its own): seen, not read.
    std::ofstream(config::state_file(env)) << R"({"arrangements": ["stack"], "last": "stack", "hinted": true})";
    s = config::read_state(env);
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->old_arrangements);
    EXPECT_TRUE(s->hinted);
    const auto aside = config::keep_aside(config::state_file(env));
    ASSERT_TRUE(aside) << aside.error();
    EXPECT_TRUE(fs::exists(*aside));
    EXPECT_TRUE(aside->filename().string().starts_with("state.json.before-"));
    // wuxian.json beside it: nothing there is no text; written, read back as it was.
    auto w = config::read_wuxian(env);
    ASSERT_TRUE(w);
    EXPECT_FALSE(*w);
    ASSERT_TRUE(config::write_wuxian(env, "{\"wuxian\": 1}\n"));
    w = config::read_wuxian(env);
    ASSERT_TRUE(w && *w);
    EXPECT_EQ(**w, "{\"wuxian\": 1}\n");
    EXPECT_EQ(config::wuxian_file(env).parent_path(), config::state_file(env).parent_path());
    // Broken: why, with the file -- and the file is not touched.
    std::ofstream(config::state_file(env)) << "{\"arrangements\": [\"tile\",";
    s = config::read_state(env);
    ASSERT_FALSE(s);
    EXPECT_NE(s.error().find("state.json"), std::string::npos) << s.error();
    // keys.json: an action's chords, one or many.
    std::ofstream(config::keys_file(env)) << R"({"terminal": ["Super+Return", "Ctrl+Alt+t"], "close": "Super+w"})";
    auto k = config::read_keys(env);
    ASSERT_TRUE(k) << k.error();
    EXPECT_EQ((*k)["terminal"], (std::vector<std::string>{"Super+Return", "Ctrl+Alt+t"}));
    EXPECT_EQ((*k)["close"], (std::vector<std::string>{"Super+w"}));
    // The JSON itself: escapes and numbers round-trip.
    auto j = config::Json::parse(R"({"a": "x\"yé", "n": [1, 2.5, true, null]})");
    ASSERT_TRUE(j);
    EXPECT_EQ(j->get("a")->string_or(""), "x\"y\xc3\xa9");
    EXPECT_EQ(config::Json::parse(j->dump())->dump(), j->dump());
    EXPECT_FALSE(config::Json::parse("{\"a\": }"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

XTEST(XdeskConfig, APagesSettingsGoBackToTheProfilesAndCanBePutBack, .area = "luban", .covers = {"XDESK-SETTINGS"}) {
    const auto root = fs::temp_directory_path() / std::format("xdesk-unset-{}", std::random_device{}());
    const std::map<std::string, std::string> env{{"XDG_CONFIG_HOME", root.string()}};
    ASSERT_TRUE(config::set_value("xdesk", env, "desktop", "inner_gap", "12"));
    ASSERT_TRUE(config::set_value("xdesk", env, "desktop", "terminal", "foot"));
    ASSERT_TRUE(config::set_value("theme", env, "theme", "accent", "#ff0000"));
    ASSERT_TRUE(config::set_value("xdesk", env, "theme", "accent", "#ff0000"));
    const auto removed = config::unset_values("xdesk", env, {{"desktop", "inner_gap"}, {"theme", "accent"}, {"desktop", "nothing"}});
    ASSERT_TRUE(removed) << removed.error();
    EXPECT_EQ(*removed, (std::vector<config::Setting>{{"desktop", "inner_gap", "12"}, {"theme", "accent", "#ff0000"}}));
    const auto now = config::load("xdesk", env);
    EXPECT_EQ(now.get("desktop", "terminal"), "foot") << "another key of the page stays";
    EXPECT_EQ(now.get("desktop", "inner_gap", "default"), "default");
    // Put back (undo): as it was.
    for (const auto& s : *removed) ASSERT_TRUE(config::set_value("xdesk", env, s.section, s.key, s.value));
    EXPECT_EQ(config::load("xdesk", env).get("desktop", "inner_gap"), "12");
    std::error_code ec;
    fs::remove_all(root, ec);
}
