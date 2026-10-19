// xdesk under the Luban OS window (Luban OS design 2026-10-19 §17): its
// arrangements, its pointer and keys, its menus and drags, its settings,
// driven headless through the app as test_luban_os.cpp drives the app.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;

namespace tk = xlings::testkit;
namespace platform = xlings::platform;
namespace fs = std::filesystem;
using Json = nlohmann::json;

#include "luban_os_support.hpp"
#include "xdesk_support.hpp"

using namespace luban_os_e2e;

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

XTEST(LubanOs, XdeskClassicTilesWindowsAndScrollsToTheFocusedOne, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-LAYOUT"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-tile");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto three = home.root() / "three.ppm", left = home.root() / "left.ppm";
    auto r = xdesk_with(home, "classic", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
screenshot {}
screenshot {}
key Super+Left
key Super+Left
sleep 1000
screenshot {}
)", three.string(), artifact("xdesk-classic-three.bmp").string(), left.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Three halves: the last two shown, the newest (focused) on the right.
    const auto a = read_ppm(three);
    EXPECT_EQ(a.at(275, 400), kGreen);
    EXPECT_EQ(a.at(825, 400), kBlue);
    // The focus two to the left: the strip scrolled back to the first.
    const auto b = read_ppm(left);
    EXPECT_EQ(b.at(275, 400), kRed);
    EXPECT_EQ(b.at(825, 400), kGreen);
}

// A stacked window is resized by Super with the right button anywhere, and
// by its border (design 2026-10-20 §8, G10: it could not be).
XTEST(LubanOs, XdeskStackedWindowsResizeBySuperRightDragAndByTheirBorders, .area = "luban",
      .covers = {"XDESK-RESIZE"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-resize");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    tk::write_file(home.root() / ".config" / "xdesk" / "state.json", R"({"arrangements":["stack"]})");
    const auto client = colour_client(home).string();
    const auto before = home.root() / "before.ppm", corner = home.root() / "corner.ppm", border = home.root() / "border.ppm";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
screenshot {}
keydown Super
press 600 400 right
move 650 410
move 700 420
release 700 420 right
keyup Super
sleep 800
screenshot {}
)", before.string(), corner.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "xdesk",
                         "--autostart", std::format("python3 {} c04040", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = box_of(read_ppm(before), kRed), b = box_of(read_ppm(corner), kRed);
    ASSERT_GE(a[0], 0) << "no red window";
    // Pressed right of and below its middle: the bottom-right corner follows.
    EXPECT_EQ(b[0], a[0]);
    EXPECT_EQ(b[1], a[1]);
    EXPECT_NEAR(b[2] - a[2], 100, 2);
    EXPECT_NEAR(b[3] - a[3], 20, 2);
    // Its left border, dragged 80 to the left: 80 wider, the right edge kept.
    r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
screenshot {}
press {} {}
move {} {}
move {} {}
release {} {}
sleep 800
screenshot {}
)", before.string(), a[0] + 2, (a[1] + a[3]) / 2, a[0] - 40, (a[1] + a[3]) / 2, a[0] - 78, (a[1] + a[3]) / 2, a[0] - 78,
                                          (a[1] + a[3]) / 2, border.string()),
                   {"--desktop-exec", xdesk_binary()->string(), "--profile", "xdesk",
                    "--autostart", std::format("python3 {} c04040", client)},
                   {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto c = box_of(read_ppm(before), kRed), d = box_of(read_ppm(border), kRed);
    EXPECT_NEAR(c[0] - d[0], 80, 2) << "the left border followed the pointer";
    EXPECT_EQ(d[2], c[2]) << "the right edge stayed";
}

// The canvas's zoom is a view (design 2026-10-20 §9.1, G11): at 91 % a
// click reaches the window, in its own coordinates, and the view stays.
XTEST(LubanOs, XdeskCanvasZoomedWindowsTakeClicksAndTheViewStays, .area = "luban",
      .covers = {"XDESK-CANVAS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-canvas-zoom");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto log = home.root() / "client.log", zoomed = home.root() / "zoomed.ppm", after = home.root() / "after.ppm";
    tk::write_file(log, "");
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
key Super+-
sleep 800
screenshot {}
click 550 400
sleep 800
screenshot {}
)", zoomed.string(), after.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "canvas",
                         "--autostart", std::format("python3 {} c04040 --log {}", client, log.string())},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = box_of(read_ppm(zoomed), kRed), b = box_of(read_ppm(after), kRed);
    ASSERT_GE(a[0], 0) << "no red window";
    EXPECT_EQ(a, b) << "a click changes nothing of the view";
    const auto said = tk::read_file(log);
    EXPECT_NE(said.find("button 272"), std::string::npos) << "the window got the click\n" << said;
    // In its own coordinates: the point clicked, unzoomed (1 / 0.909).
    std::istringstream in(said.substr(said.find("button 272")));
    std::string word;
    int code = 0;
    double x = 0, y = 0;
    in >> word >> code >> word >> x >> y;
    EXPECT_NEAR(x, (550 - a[0]) * 1.1, 6) << said;
}

// A client that draws to whole cells (foot) is told it is tiled and takes
// the size given (design 2026-10-20 §7.1, G4): side by side, two such windows
// are the gap apart -- 4 pixels -- with nothing of their own between.
XTEST(LubanOs, XdeskTiledClientsTakeTheirSizeAndSitTheGapApart, .area = "luban",
      .covers = {"XDESK-TILE-SIZES"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-cells");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto two = home.root() / "two.ppm";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
screenshot {}
)", two.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic",
                         "--autostart", std::format("python3 {} c04040 --cells 9x19", client),
                         "--autostart", std::format("sh -c 'sleep 0.6; exec python3 {} 40c040 --cells 9x19'", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto img = read_ppm(two);
    const int y = 400;
    int red_end = -1, green_start = -1;
    for (int x = 0; x < img.width; ++x) {
        if (img.at(x, y) == kRed) red_end = x;
        if (img.at(x, y) == kGreen && green_start < 0) green_start = x;
    }
    ASSERT_GE(red_end, 0) << "no red window at row " << y;
    ASSERT_GT(green_start, red_end) << "no green window right of it";
    EXPECT_EQ(green_start - red_end - 1, 4) << "between them only the gap (a short client left its own strip)";
}

XTEST(LubanOs, XdeskCanvasShowsEveryWindowInItsOverviewAClickSelectsTwiceGoesIn, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-LAYOUT", "XDESK-CANVAS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-canvas");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto all = home.root() / "overview.ppm", once = home.root() / "once.ppm", in = home.root() / "in.ppm";
    auto r = xdesk_with(home, "canvas", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
key Super+o
sleep 1000
screenshot {}
screenshot {}
click desktop
sleep 1000
screenshot {}
click desktop
click desktop
sleep 1000
screenshot {}
)", all.string(), artifact("xdesk-canvas-overview.bmp").string(), once.string(), in.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Zoomed out: the three side by side, the middle one in the middle.
    const auto a = read_ppm(all);
    std::set<std::array<int, 3>> seen;
    for (int x = 0; x < 1100; x += 10) seen.insert(a.at(x, 350));
    EXPECT_TRUE(seen.contains(kRed) && seen.contains(kGreen) && seen.contains(kBlue));
    EXPECT_EQ(a.at(550, 350), kGreen);
    // One click selects it: the view stays (design 2026-10-20 §9.1, G11).
    EXPECT_NE(read_ppm(once).at(150, 350), kGreen) << "a click does not zoom in";
    EXPECT_EQ(read_ppm(once).at(550, 350), kGreen);
    // Twice: back to 1, it in the middle of the view.
    const auto b = read_ppm(in);
    EXPECT_EQ(b.at(550, 350), kGreen);
    EXPECT_EQ(b.at(150, 350), kGreen) << "still zoomed out (it is as wide as it was made)";
    EXPECT_EQ(b.at(950, 350), kGreen);
    EXPECT_NE(a.at(150, 350), kGreen) << "the overview did not zoom out";
}

XTEST(LubanOs, XdeskSwitchesArrangementsAndComesBackToTheSameLayout, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-ARRANGEMENTS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-arrangements");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto split = home.root() / "split.ppm", whole = home.root() / "whole.ppm", back = home.root() / "back.ppm";
    // Tiled: blue joins green's column (Super+[); stacked (Super+m): blue the
    // whole output (Super+f), then free again; on to the canvas and back to
    // tiling: the columns as they were.
    auto r = xdesk_with(home, "classic", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
key Super+[
sleep 1000
screenshot {}
key Super+m
sleep 500
key Super+f
sleep 1000
screenshot {}
key Super+f
sleep 500
key Super+m
sleep 500
key Super+m
sleep 1000
screenshot {}
screenshot {}
)", split.string(), whole.string(), back.string(), artifact("xdesk-arrangements-back.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Two columns: red, then green above blue.
    const auto a = read_ppm(split);
    EXPECT_EQ(a.at(275, 400), kRed);
    EXPECT_EQ(a.at(825, 250), kGreen);
    EXPECT_EQ(a.at(825, 600), kBlue);
    EXPECT_EQ(read_ppm(whole).at(275, 400), kBlue) << "stacked, Super+f is the whole output";
    const auto c = read_ppm(back);
    EXPECT_EQ(c.at(275, 400), kRed);
    EXPECT_EQ(c.at(825, 250), kGreen);
    EXPECT_EQ(c.at(825, 600), kBlue);
}

XTEST(LubanOs, TheHostDrawsTheDesktopsPointerAsItSaysIt, .area = "luban",
      .covers = {"LUBAN-OS-POINTER", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-pointer");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto panel = home.root() / "panel.json", over = home.root() / "over.json", back = home.root() / "back.json";
    // Over xdesk's own panel: a shape by name; over a window that draws its
    // own pointer: that image; off the window again: the shape. The host
    // draws each (no frame in between) and nothing is drawn into the frames.
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
move 550 8
sleep 800
state {}
move 550 400
sleep 1000
state {}
move 550 8
sleep 800
state {}
)", panel.string(), over.string(), back.string()),
                        {"--desktop-session", "xdesk", "--desktop-exec", xdesk_binary()->string(), "--profile", "classic",
                         "--autostart", std::format("python3 {} c04040 --cursor 1", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = read_state(panel)["pointer"], b = read_state(over)["pointer"], c = read_state(back)["pointer"];
    EXPECT_EQ(a["said"], "default") << a.dump();
    EXPECT_EQ(a["shape"], "default") << a.dump();
    EXPECT_FALSE(a["host_arrow"].get<bool>()) << "an xdesk that asks draws no arrow of the host's own";
    EXPECT_EQ(b["shape"], "custom") << b.dump();
    EXPECT_EQ(b["size"], Json::array({8, 8})) << b.dump();
    EXPECT_EQ(c["shape"], "default") << c.dump();
}

XTEST(LubanOs, AWindowThatDrawsNoPointerGetsXdesksInItsFrames, .area = "luban",
      .covers = {"LUBAN-OS-POINTER", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-soft-pointer");
    // A pointer theme of one shape: an 8x8 green square, its hotspot the corner.
    const auto icons = home.root() / "icons";
    fs::create_directories(icons / "default" / "cursors");
    std::string xcur = "Xcur";
    auto u32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) xcur.push_back(static_cast<char>(v >> (8 * k) & 0xff)); };
    for (const std::uint32_t v : {16u, 0x10000u, 1u, 0xfffd0002u, 24u, 28u, 36u, 0xfffd0002u, 24u, 1u, 8u, 8u, 0u, 0u, 0u}) u32(v);
    for (int k = 0; k < 64; ++k) u32(0xff00ff00u);
    tk::write_file(icons / "default" / "cursors" / "default", xcur);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    // A window that does not draw pointers (an older Luban OS): xdesk draws
    // it into the frames, where the pointer is.
    const auto shot = home.root() / "soft.ppm";
    tk::RunOptions o;
    o.argv = {"python3", (fs::path(__FILE__).parent_path() / "luban_desktop_client.py").string(), "--exec", "{move:200,150}",
              shot.string(), "--", xdesk_binary()->string(), "--profile", "classic"};
    o.env = home.env();
    o.env["XDG_RUNTIME_DIR"] = runtime.string();
    o.env["XCURSOR_PATH"] = icons.string();
    const auto r = tk::run(o);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(read_ppm(shot).at(203, 153), (std::array<int, 3>{0x00, 0xff, 0x00})) << "the pointer, drawn where it is";
}

// Super+m held: the arrangements as cards, the next one lit; m again for the
// one after; Super let go chooses it -- and the session after starts as it
// was left (design 2026-10-19 §17.5, §17.12). A state.json that cannot be read
// is said and never written over.
XTEST(LubanOs, XdeskChoosesArrangementsHeldWithSuperAndKeepsThem, .area = "luban",
      .covers = {"XDESK-ARRANGEMENTS", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-chooser");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    auto session = [&](std::string_view script) {
        return run_script(home, script,
                          {"--desktop-exec", xdesk_binary()->string(), "--profile", "xdesk", "--autostart",
                           std::format("python3 {} c04040", client)},
                          {{"XDG_RUNTIME_DIR", runtime.string()}});
    };
    const auto state = home.root() / ".config" / "xdesk" / "state.json";
    auto arrangement = [&] { return Json::parse(tk::read_file(state))["arrangements"][0].get<std::string>(); };
    const auto held = home.root() / "held.ppm";
    auto r = session(std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
keydown Super
key m
sleep 800
screenshot {}
screenshot {}
key m
keyup Super
sleep 800
)", held.string(), artifact("xdesk-chooser.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(read_ppm(held).at(550, 380), kRed) << "the chooser over the desktop while Super is held";
    EXPECT_EQ(arrangement(), "canvas") << "tile, then stack, then canvas: chosen on letting Super go";
    EXPECT_EQ(Json::parse(tk::read_file(state))["last"], "canvas");
    // The next session starts as it was left: the next one from canvas is tile.
    r = session(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 1500
key Super+m
sleep 800
)");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(arrangement(), "tile");
    // Unreadable: started as new, and the file is the person's still.
    tk::write_file(state, "{\"arrangements\": [");
    r = session(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 1500
key Super+m
sleep 800
)");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(tk::read_file(state), "{\"arrangements\": [");
}

// A window's title bar, the other button: its menu (design 2026-10-19 §18.1).
XTEST(LubanOs, XdeskTitleBarMenuActsOnItsWindow, .area = "luban",
      .covers = {"XDESK-WINDOW-MENU", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-menu");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto menu = home.root() / "menu.ppm", closed = home.root() / "closed.ppm";
    // One window, its title bar under the panel; the menu's last item closes it
    // (4 items, a line, 4 workspaces, a line, Close: rows of 30, lines of 9).
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
click 300 50 right
sleep 600
screenshot {}
screenshot {}
click 420 331
sleep 1200
screenshot {}
)", menu.string(), artifact("xdesk-window-menu.bmp").string(), closed.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                         std::format("python3 {} c04040", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(read_ppm(menu).at(420, 120), kRed) << "the menu where the button was pressed";
    EXPECT_NE(read_ppm(closed).at(550, 400), kRed) << "Close closed it";

    // An application's rule (kept in state.json): it opens on workspace 2.
    tk::write_file(home.root() / ".config" / "xdesk" / "state.json",
                   R"({"rules": {"luban-test-red": {"workspace": 2}}, "hinted": true})");
    const auto one = home.root() / "one.ppm", two = home.root() / "two.ppm";
    r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
screenshot {}
key Super+2
sleep 1000
screenshot {}
)", one.string(), two.string()),
                   {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                    std::format("python3 {} c04040 --app-id luban-test-red", client)},
                   {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(read_ppm(one).at(550, 400), kRed) << "not on the first workspace";
    EXPECT_EQ(read_ppm(two).at(550, 400), kRed) << "on the second, as its rule says";
}

// A window dragged by its title (design 2026-10-19 §17.9): a preview where it
// would land, and there it goes when let go -- red over the top of green is
// red above green in green's column.
XTEST(LubanOs, XdeskDraggedWindowLandsWhereItsPreviewWas, .area = "luban",
      .covers = {"XDESK-DRAG-SNAP", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-drag");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto during = home.root() / "during.ppm", after = home.root() / "after.ppm";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
press 200 50
move 400 120
move 820 150
sleep 600
screenshot {}
screenshot {}
release 820 150
sleep 1200
screenshot {}
)", during.string(), artifact("xdesk-drag-preview.bmp").string(), after.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic",
                         "--autostart", std::format("python3 {} c04040", client),
                         "--autostart", std::format("sh -c 'sleep 0.6; exec python3 {} 40c040'", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // While dragged: green's top half shows where red would go (the accent over green).
    EXPECT_NE(read_ppm(during).at(1000, 250), kGreen) << "a preview over the top of green";
    // And green already where the drop leaves it: the whole width, below (design 2026-10-20 §7.5, G6).
    EXPECT_EQ(read_ppm(during).at(300, 600), kGreen) << "the others move to where the drop leaves them";
    // Let go: one column, red above green.
    const auto a = read_ppm(after);
    EXPECT_EQ(a.at(550, 200), kRed);
    EXPECT_EQ(a.at(550, 600), kGreen);
}

XTEST(LubanOs, XdeskLauncherStartsWhatIsTypedAndThePanelSwitchesWorkspaces, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-SHELL", "XDESK-APPS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-launcher");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", data = home.root() / "share";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    fs::create_directories(data / "applications");
    // The one application there: a green window.
    tk::write_file(data / "applications" / "greenery.desktop",
                   std::format("[Desktop Entry]\nType=Application\nName=Greenery\nExec=python3 {} 40c040\n",
                               colour_client(home).string()));
    const auto open = home.root() / "open.ppm", started = home.root() / "started.ppm", other = home.root() / "other.ppm";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 500
key Super+space
sleep 300
type gree
sleep 300
screenshot {}
screenshot {}
key Return
sleep 2500
screenshot {}
screenshot {}
click 109 15
sleep 500
screenshot {}
)", open.string(), artifact("xdesk-launcher.bmp").string(), started.string(), artifact("xdesk-launched.bmp").string(),
                                          other.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic"},
                        {{"XDG_RUNTIME_DIR", runtime.string()}, {"XDG_DATA_DIRS", data.string()}, {"HOME", home.root().string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // The launcher is up, its first row chosen (the accent's blue).
    const auto a = read_ppm(open);
    EXPECT_NE(a.at(550, 210), (std::array<int, 3>{0x1b, 0x1e, 0x25})) << "no launcher over the desktop";
    // Enter started it: green fills the output under the panel.
    const auto b = read_ppm(started);
    EXPECT_EQ(b.at(550, 400), kGreen);
    // Workspace 2 from the panel: empty.
    const auto c = read_ppm(other);
    EXPECT_NE(c.at(550, 400), kGreen);
}

XTEST(LubanOs, XdeskGivesTheHostsInputMethodAndClipboardToItsWindows, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-TEXT-CLIPBOARD"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-text");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", got = home.root() / "client.log", st = home.root() / "text.json";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    auto r = run_script(home, std::format(R"(clipboard from-the-host
wait desktop
waitfor desktop.texture.0 1100
waitfor desktop_text true
commit 你好
commit abc
waitfor clipboard from-the-guest
state {}
)", st.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                         std::format("python3 {} c04040 --log {} --copy from-the-guest", colour_client(home).string(), got.string())},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript() << tk::read_file(got);
    const auto log = tk::read_file(got);
    // The host's clipboard reached the window as its selection...
    EXPECT_NE(log.find("paste from-the-host"), std::string::npos) << log;
    // ...what the input method committed reached it as text (a caret was asked for)...
    EXPECT_NE(log.find("commit 你好"), std::string::npos) << log;
    EXPECT_NE(log.find("commit abc"), std::string::npos) << log;
    EXPECT_EQ(log.find("commit a\n"), std::string::npos) << "a character arrived twice (as a key too)\n" << log;
    // ...and what it copied is the host's clipboard.
    EXPECT_EQ(read_state(st)["clipboard"], "from-the-guest");
}


XTEST(LubanOs, XdeskTakesAPersonsSettingsOverItsProfiles, .area = "luban", .covers = {"XDESK-CONFIG"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-settings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", config = home.root() / "config", shot = home.root() / "set.ppm";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    fs::create_directories(config / "xdesk");
    tk::write_file(config / "xdesk" / "classic.ini", "[theme]\nbackground = #203040\n[desktop]\ngap = 0\n");
    auto r = run_script(home, std::format("wait desktop\nwaitfor desktop.texture.0 1100\nsleep 500\nframes 2\nscreenshot {}\n", shot.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic"},
                        {{"XDG_RUNTIME_DIR", runtime.string()}, {"XDG_CONFIG_HOME", config.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(read_ppm(shot).at(550, 400), (std::array<int, 3>{0x20, 0x30, 0x40}));
}

XTEST(LubanOs, XdeskDrawsForTheScreensDensityAndThePointerLandsWhereItIs, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "LUBAN-OS-INTERACTION"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-density");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", st = home.root() / "density.json";
    const auto open = home.root() / "open.ppm", closed = home.root() / "closed.ppm";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    // A screen of density 2: the desktop is asked for twice the pixels and
    // shown at the window's size; a click on a title bar's close button
    // (logical 1079, 51) closes that window.
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 2200
sleep 2000
frames 2
state {}
screenshot {}
screenshot {}
click 1079 51
sleep 1000
frames 2
screenshot {}
)", st.string(), open.string(), artifact("xdesk-density-2.bmp").string(), closed.string()),
                        {"--density", "2", "--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                         std::format("python3 {} c04040", colour_client(home).string())},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(read_state(st)["desktop"]["texture"], Json::array({2200, 1400}));
    const auto a = read_ppm(open);
    EXPECT_EQ(a.at(550, 400), kRed);                                 // the window, at the window's size
    EXPECT_NE(a.at(550, 15), (std::array<int, 3>{0x1b, 0x1e, 0x25})) << "no panel across the top";
    EXPECT_NE(read_ppm(closed).at(550, 400), kRed) << "the click did not reach the close button";
}
