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

// ── interaction, layout and what is drawn ───────────────────────────────
//
// A script (luban-os --script) drives the app with input as SDL delivers a
// person's -- keys, text, the pointer, the window's close and size -- and
// checks its state between frames; the screenshots are read here (pixels)
// and kept in the artifacts directory for people to look at.

namespace {

struct Image {
    int width { 0 }, height { 0 };
    std::string rgb;
    std::array<int, 3> at(int x, int y) const {
        const auto k = (std::size_t(y) * width + x) * 3;
        return {static_cast<unsigned char>(rgb[k]), static_cast<unsigned char>(rgb[k + 1]), static_cast<unsigned char>(rgb[k + 2])};
    }
};

Image read_ppm(const fs::path& file) {
    const auto bytes = tk::read_file(file);
    Image img;
    std::istringstream head(bytes);
    std::string magic;
    int maxval = 0;
    head >> magic >> img.width >> img.height >> maxval;
    const auto at = static_cast<std::size_t>(head.tellg()) + 1;
    img.rgb = bytes.substr(at);
    return img;
}

fs::path artifact(std::string_view name) {
    const auto dir = tk::artifacts_dir() / "luban-os";
    fs::create_directories(dir);
    return dir / name;
}

tk::RunResult run_script(const tk::Home& home, std::string_view script, std::vector<std::string> args = {},
                         std::map<std::string, std::string> env = {}) {
    const auto file = home.root() / "script.txt";
    tk::write_file(file, script);
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "--headless", "--script", file.string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    // The renderer a machine without a GPU has, here as in CI.
    o.env["SDL_RENDER_DRIVER"] = "software";
    for (auto& [k, v] : env) o.env[k] = v;
    o.timeout = std::chrono::seconds(240);
    return tk::run(o);
}

Json read_state(const fs::path& file) { return Json::parse(tk::read_file(file)); }

std::array<float, 4> rect(const Json& state, const std::string& anchor) {
    const auto& r = state.at("anchors").at(anchor);
    return {r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()};
}

tk::Home home_with_two(std::string_view tag) {
    auto home = tk::Home::isolated(tag);
    EXPECT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    EXPECT_EQ(home.xlings({"subos", "new", "alpha"}).exit_code, 0);
    EXPECT_EQ(home.xlings({"subos", "new", "beta"}).exit_code, 0);
    return home;
}

}  // namespace

XTEST(LubanOs, KeysAndThePointerReachEveryViewAndTheHighlightFollows, .area = "luban",
      .covers = {"LUBAN-OS-APP", "LUBAN-OS-INTERACTION"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-keys");
    const auto shot = home.root() / "down.ppm", st = home.root() / "down.json";
    auto r = run_script(home, std::format(R"(wait launcher
expect countdown_to alpha
key Down
expect selected 1
expect countdown_to absent
frames 2
screenshot {}
screenshot {}
state {}
key Down
expect selected 2
key Down
expect selected 0
key Up
expect selected 2
key Return
wait quick-start
click card:nano
expect edition nano
expect name nano-1
click card:tiny
expect edition tiny
expect name tiny-1
click more
waitfor anchors.mode present 5
frames 2
screenshot {}
click back
wait launcher
click gear
wait admin
click tab:activity
expect admin_tab activity
frames 2
screenshot {}
click tab:settings
expect admin_tab settings
frames 2
screenshot {}
click tab:systems
click admin:beta
expect admin_selected beta
click admin:back
wait launcher
key F2
wait admin
)", shot.string(), artifact("launcher-down.bmp").string(), st.string(), artifact("quick-start-more.bmp").string(),
                                          artifact("admin-activity.bmp").string(), artifact("admin-settings.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // The highlight is on the row the keys chose, not on the other.
    const auto img = read_ppm(shot);
    const auto s = read_state(st);
    const auto chosen = rect(s, "row:beta"), other = rect(s, "row:alpha");
    const auto in_chosen = img.at(static_cast<int>(chosen[2]) - 60, static_cast<int>((chosen[1] + chosen[3]) / 2));
    const auto in_other = img.at(static_cast<int>(other[2]) - 60, static_cast<int>((other[1] + other[3]) / 2));
    EXPECT_NE(in_chosen, in_other) << "the chosen row looks like the others";
    EXPECT_GT(in_chosen[2], in_other[2]) << "the highlight is the accent (blue)";
}

XTEST(LubanOs, ADesktopIsDrawnAndGivenTheWindowsInputAndSize, .area = "luban",
      .covers = {"LUBAN-OS-APP", "LUBAN-OS-INTERACTION", "LUBAN-DESKTOP-WIRE"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-desktop");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // A desktop of the test's own: one colour, and everything it is sent, logged.
    const auto fake = home.root() / "desktop.py", log = home.root() / "desktop.log";
    tk::write_file(fake, R"(import struct, sys
log = open(sys.argv[1], "w", buffering=1)
W, H = 320, 200
out = sys.stdout.buffer
def send(kind, payload):
    out.write(kind.encode() + struct.pack(">I", len(payload)) + payload)
    out.flush()
send("h", b"15")
send("s", b"%d %d" % (W, H))
send("f", b"0 0 %d %d %d\n" % (W, H, W * 4) + bytes([0x40, 0x80, 0xC0, 0xFF]) * (W * H))
inp = sys.stdin.buffer
while True:
    head = inp.read(5)
    if len(head) < 5:
        break
    body = inp.read(struct.unpack(">I", head[1:])[0])
    log.write("%s %s\n" % (chr(head[0]), body.decode(errors="replace")))
log.write("eof\n")
)");
    const auto shot = home.root() / "desktop.ppm", st = home.root() / "desktop.json";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 320
frames 2
screenshot {}
screenshot {}
state {}
click desktop
type ab
resize 900 600
sleep 800
key Ctrl+Alt
waitfor menu true
screenshot {}
click menu:back-to-the-list
wait launcher
sleep 300
)", shot.string(), artifact("desktop.bmp").string(), st.string(), artifact("desktop-menu.bmp").string()),
                        {"--desktop-exec", "python3", fake.string(), log.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Its picture, where the window put it (1:1, centred).
    const auto img = read_ppm(shot);
    const auto d = rect(read_state(st), "desktop");
    EXPECT_FLOAT_EQ(d[2] - d[0], 320.0f);
    EXPECT_EQ(img.at(static_cast<int>(d[0]) + 20, static_cast<int>(d[1]) + 20), (std::array<int, 3>{0xC0, 0x80, 0x40}));
    const auto sent = tk::read_file(log);
    EXPECT_NE(sent.find("H 15 1100 700 1"), std::string::npos) << "the window's size, asked first\n" << sent;
    // The click, in the desktop's own pixels (its middle), down and up.
    EXPECT_NE(sent.find("E motion 160 100"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E button 1 1 0"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E button 1 0 0"), std::string::npos) << sent;
    // Keys as keysyms ('a' is 97), down and up.
    EXPECT_NE(sent.find("E key 1 97 0 0"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E key 0 98 0 0"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E resize 900 600"), std::string::npos) << "the window's new size\n" << sent;
    // Leaving it ends its stream.
    EXPECT_NE(sent.find("eof"), std::string::npos) << sent;
}

XTEST(LubanOs, EveryControlIsOnScreenApartAndBigEnoughAtEverySizeInBothLanguages, .area = "luban",
      .covers = {"LUBAN-OS-APP", "LUBAN-OS-LAYOUT"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-layout");
    for (const auto& [w, h] : {std::pair{820, 520}, std::pair{1100, 700}, std::pair{1600, 1000}}) {
        for (const std::string lang : {"en", "zh"}) {
            const auto tag = std::format("{}x{}-{}", w, h, lang);
            std::vector<std::string> views{"launcher", "quick-start", "admin"};
            auto r = run_script(home, std::format(R"(resize {0} {1}
wait launcher
key Down
frames 3
state {2}/launcher.json
screenshot {3}
click row:new
wait quick-start
frames 3
state {2}/quick-start.json
screenshot {4}
click back
wait launcher
key F2
wait admin
frames 3
state {2}/admin.json
screenshot {5}
)", w, h, home.root().string(), artifact(tag + "-launcher.bmp").string(), artifact(tag + "-quick-start.bmp").string(),
                                                  artifact(tag + "-admin.bmp").string()),
                                {}, {{"LUBAN_OS_LANG", lang == "zh" ? "zh_CN.UTF-8" : "en_US.UTF-8"}});
            ASSERT_EQ(r.exit_code, 0) << tag << "\n" << r.transcript();
            for (const auto& view : views) {
                const auto s = read_state(home.root() / (view + ".json"));
                std::vector<std::pair<std::string, std::array<float, 4>>> controls;
                for (auto it = s["anchors"].begin(); it != s["anchors"].end(); ++it)
                    controls.emplace_back(it.key(), std::array<float, 4>{it.value()[0].get<float>(), it.value()[1].get<float>(),
                                                                         it.value()[2].get<float>(), it.value()[3].get<float>()});
                ASSERT_FALSE(controls.empty()) << tag << " " << view;
                for (const auto& [name, c] : controls) {
                    EXPECT_GE(c[0], 0) << tag << " " << view << " " << name << " starts off the window";
                    EXPECT_GE(c[1], 0) << tag << " " << view << " " << name;
                    EXPECT_LE(c[2], w) << tag << " " << view << " " << name << " runs off the window";
                    EXPECT_LE(c[3], h) << tag << " " << view << " " << name << " runs off the window";
                    EXPECT_GE(c[3] - c[1], 20) << tag << " " << view << " " << name << " is too small to hit";
                }
                for (std::size_t i = 0; i < controls.size(); ++i)
                    for (std::size_t k = i + 1; k < controls.size(); ++k) {
                        const auto& a = controls[i].second;
                        const auto& b = controls[k].second;
                        const bool apart = a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1];
                        EXPECT_TRUE(apart) << tag << " " << view << ": " << controls[i].first << " and " << controls[k].first
                                           << " overlap";
                    }
            }
        }
    }
}

XTEST(LubanOs, TheCountdownMovesFrameByFrame, .area = "luban", .covers = {"LUBAN-OS-APP", "LUBAN-OS-LAYOUT"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-countdown");
    const auto a = home.root() / "c1.ppm", b = home.root() / "c2.ppm";
    auto r = run_script(home, std::format("wait launcher\nscreenshot {}\nsleep 1200\nscreenshot {}\nexpect countdown_to alpha\n",
                                          a.string(), b.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(tk::read_file(a), tk::read_file(b)) << "the countdown did not change what is drawn";
}

XTEST(LubanOs, AConsoleShowsColourAndWideTextAndTakesWhatIsTyped, .area = "luban",
      .covers = {"LUBAN-OS-INTERACTION", "LUBAN-TERM"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-console");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto shot = home.root() / "console.ppm", st = home.root() / "console.json";
    auto r = run_script(home, std::format(R"(wait console
waitfor console.rows.1 ready 10
type hello
key Return
waitfor console.rows.3 hello 10
frames 2
screenshot {}
screenshot {}
state {}
)", shot.string(), artifact("console.bmp").string(), st.string()),
                        {"--console-exec", "/bin/sh", "-c", R"(printf '\033[31mred\033[0m 中文 wide\nready\n'; exec cat)"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto s = read_state(st);
    const auto rows = s["console"]["rows"];
    EXPECT_EQ(rows[0], "red 中文 wide") << s.dump(2);
    EXPECT_EQ(rows[2], "hello") << "the terminal's echo of what was typed\n" << s.dump(2);
    EXPECT_EQ(rows[3], "hello") << "the program's answer\n" << s.dump(2);
    // The red is drawn red: a reddish pixel in the first line.
    const auto img = read_ppm(shot);
    const auto c = rect(s, "console");
    bool red = false;
    for (int y = static_cast<int>(c[1]); y < static_cast<int>(c[1]) + 18 && !red; ++y)
        for (int x = static_cast<int>(c[0]); x < static_cast<int>(c[0]) + 40 && !red; ++x) {
            const auto p = img.at(x, y);
            red = p[0] > 150 && p[1] < 130 && p[2] < 130;
        }
    EXPECT_TRUE(red) << "no red in the first line";
}

// xdesk (apps/xdesk), built beside the app; XDESK_BIN names another.
std::optional<fs::path> xdesk_binary() {
    if (const char* forced = std::getenv("XDESK_BIN"); forced && *forced) return fs::path(forced);
    const auto app = app_binary();
    if (!app) return std::nullopt;
    for (const auto& c : {app->parent_path() / "xdesk", app->parent_path().parent_path() / "xdesk-app" / "xdesk"})
        if (fs::is_regular_file(c)) return c;
    return std::nullopt;
}

XTEST(LubanOs, XdeskIsADesktopAtTheWindowsSize, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "LUBAN-OS-INTERACTION"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    const auto xdesk = xdesk_binary();
    if (!xdesk) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto shot = home.root() / "xdesk.ppm", small = home.root() / "xdesk-small.ppm";
    const auto st = home.root() / "xdesk.json", st_small = home.root() / "xdesk-small.json";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 500
frames 2
screenshot {}
screenshot {}
state {}
resize 900 600
waitfor desktop.texture.0 900
sleep 500
frames 2
screenshot {}
state {}
)", shot.string(), artifact("xdesk-classic.bmp").string(), st.string(), small.string(), st_small.string()),
                        {"--desktop-exec", xdesk->string(), "--profile", "classic"},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Its own picture -- the desktop's background -- at the size it was asked.
    EXPECT_EQ(read_state(st)["desktop"]["texture"], Json::array({1100, 700}));
    for (const auto& [img, state] : {std::pair{read_ppm(shot), read_state(st)}, std::pair{read_ppm(small), read_state(st_small)}}) {
        const auto d = rect(state, "desktop");
        const auto px = img.at(static_cast<int>(d[0]) + 40, static_cast<int>(d[1]) + 40);
        EXPECT_NEAR(px[0], 0x1b, 2) << state.dump();
        EXPECT_NEAR(px[1], 0x1e, 2);
        EXPECT_NEAR(px[2], 0x25, 2);
    }
}
