// xdesk under the Luban OS window (Luban OS design 2026-10-19 §17): its
// arrangements, its pointer and keys, its menus and drags, its settings,
// driven headless through the app as test_luban_os.cpp drives the app.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;
import xbus.router;
import xbus.node;

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
    tk::write_file(home.root() / ".config" / "xdesk" / "wuxian.json", R"({"wuxian": 1, "workspaces": [{"id": 1, "rule": "stack"}]})");
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

// The WuXian view's zoom is a view (design 2026-10-20 §9.1, G11; WuXian
// §3.2): zoomed out to 77 % a click reaches the window, in its own
// coordinates, and the view stays. The canvas session of before starts there.
XTEST(LubanOs, XdeskCanvasZoomedWindowsTakeClicksAndTheViewStays, .area = "luban",
      .covers = {"XDESK-CANVAS", "WUXIAN-VIEWS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-canvas-zoom");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto log = home.root() / "client.log", zoomed = home.root() / "zoomed.ppm", after = home.root() / "after.ppm",
               whole = home.root() / "whole.ppm";
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
key Super+o
sleep 1500
screenshot {}
)", zoomed.string(), after.string(), whole.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "canvas",
                         "--autostart", std::format("python3 {} c04040 --log {}", client, log.string())},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = box_of(read_ppm(zoomed), kRed), b = box_of(read_ppm(after), kRed), w = box_of(read_ppm(whole), kRed);
    ASSERT_GE(a[0], 0) << "no red window";
    ASSERT_GE(w[0], 0) << "no red window in the workspace view";
    EXPECT_EQ(a, b) << "a click changes nothing of the view";
    const double zoom = double(a[2] - a[0]) / double(w[2] - w[0]);
    EXPECT_LT(zoom, 0.95) << "the WuXian view shows the workspace smaller";
    EXPECT_GE(zoom, 0.5) << "and its windows still take the pointer";
    const auto said = tk::read_file(log);
    EXPECT_NE(said.find("button 272"), std::string::npos) << "the window got the click\n" << said;
    // In its own coordinates: the point clicked, unzoomed.
    std::istringstream in(said.substr(said.find("button 272")));
    std::string word;
    int code = 0;
    double x = 0, y = 0;
    in >> word >> code >> word >> x >> y;
    EXPECT_NEAR(x, (550 - a[0]) / zoom, 6) << said;
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

// Super+O (design 2026-10-20 WuXian §3.2): the WuXian view -- the workspace
// smaller on the plane, its three windows in it; zoomed out below half,
// windows are pictures: a click selects one and the view stays, twice goes
// in at 100 %; Super+O again: the workspace view, as it was.
XTEST(LubanOs, XdeskCanvasShowsEveryWindowInItsOverviewAClickSelectsTwiceGoesIn, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-LAYOUT", "XDESK-CANVAS", "WUXIAN-VIEWS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-canvas");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto start = home.root() / "start.ppm", all = home.root() / "wuxian.ppm", small = home.root() / "small.ppm",
               once = home.root() / "once.ppm", in = home.root() / "in.ppm", back = home.root() / "back.ppm";
    std::string out;
    for (int k = 0; k < 6; ++k) out += "key Super+-\nsleep 200\n";
    auto r = xdesk_with(home, "xdesk", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
screenshot {}
key Super+o
sleep 1500
screenshot {}
screenshot {}
{}sleep 800
screenshot {}
click desktop
sleep 1000
screenshot {}
click desktop
click desktop
sleep 1000
screenshot {}
key Super+o
sleep 1500
screenshot {}
)", start.string(), all.string(), artifact("xdesk-wuxian-view.bmp").string(), out, small.string(), once.string(), in.string(),
                                                     back.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto s0 = box_of(read_ppm(start), kGreen);
    ASSERT_GE(s0[0], 0) << "no green window";
    // The WuXian view: the three side by side, green in the middle, the
    // workspace smaller than the screen (the plane around it).
    const auto a = read_ppm(all);
    std::set<std::array<int, 3>> seen;
    for (int x = 0; x < 1100; x += 6) seen.insert(a.at(x, 350));
    EXPECT_TRUE(seen.contains(kRed) && seen.contains(kGreen) && seen.contains(kBlue));
    EXPECT_EQ(a.at(550, 350), kGreen);
    EXPECT_NE(a.at(3, 350), kRed) << "the workspace is smaller than the screen";
    // Below half: a click selects -- the view stays.
    const auto g = box_of(read_ppm(small), kGreen);
    EXPECT_LT(g[2] - g[0], (s0[2] - s0[0]) / 2) << "zoomed out below half";
    EXPECT_EQ(box_of(read_ppm(once), kGreen), g) << "a click does not move the view";
    // Twice: in at 100 %, green in the middle.
    const auto i = box_of(read_ppm(in), kGreen);
    EXPECT_EQ(read_ppm(in).at(550, 350), kGreen);
    EXPECT_NEAR(i[2] - i[0], s0[2] - s0[0], 3) << "its own size";
    // Back to the workspace view: as it was.
    EXPECT_EQ(box_of(read_ppm(back), kGreen), s0);
}

// Alt+Tab in the WuXian view (design 2026-10-21 §8.3, J14): every window
// gathered on the switchers' own layer, over a dimmed output -- the
// workspace's plate and label under them, not over them.
XTEST(LubanOs, XdeskAltTabInWuxianGathersOverADimmedPlane, .area = "luban",
      .covers = {"XDESK-HIGHLIGHT", "WUXIAN-VIEWS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-wuxian-tab");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto plane = home.root() / "plane.ppm", held = home.root() / "held.ppm";
    auto r = xdesk_with(home, "xdesk", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
key Super+o
sleep 1500
screenshot {}
keydown Alt
key Tab
sleep 1200
screenshot {}
screenshot {}
keyup Alt
sleep 1500
)", plane.string(), held.string(), artifact("xdesk-wuxian-alt-tab.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = read_ppm(plane), b = read_ppm(held);
    for (const auto& c : {kRed, kGreen, kBlue}) EXPECT_GE(box_of(b, c)[0], 0) << "every window shown whole, as it is";
    // Away from the windows: darker than the plane was.
    const auto sum = [](const std::array<int, 3>& p) { return p[0] + p[1] + p[2]; };
    EXPECT_LT(sum(b.at(4, 4)), sum(a.at(4, 4))) << "the output dimmed under the gathered windows";
}

XTEST(LubanOs, XdeskSwitchesArrangementsAndComesBackToTheSameLayout, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-ARRANGEMENTS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-arrangements");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto split = home.root() / "split.ppm", whole = home.root() / "whole.ppm", back = home.root() / "back.ppm";
    // Tiled: blue joins green's column (Super+[); stacked (Super+m): blue the
    // whole output (Super+f), then free again; back to tiling (Super+m): the
    // columns as they were.
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

XTEST(LubanOs, AShakenPointerGrowsInTheDesktopsAccentAndShrinksBack, .area = "luban",
      .covers = {"CURSOR-LOCATE", "CURSOR-SHAKE"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-pointer-shake");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto still = home.root() / "still.json", shaken = home.root() / "shaken.json", after = home.root() / "after.json";
    // Over xdesk's panel, back and forth 500 px a dozen times as fast as the
    // script goes: shaken. Then still: back to its size within a second.
    std::string strokes;
    for (int k = 0; k < 12; ++k) strokes += std::format("move {} 8\n", k % 2 ? 800 : 300);
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
move 550 8
sleep 800
state {}
{}state {}
sleep 1800
state {}
)", still.string(), strokes, shaken.string(), after.string()),
                        {"--desktop-session", "xdesk", "--desktop-exec", xdesk_binary()->string(), "--profile", "xdesk"},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = read_state(still)["pointer"], b = read_state(shaken)["pointer"], c = read_state(after)["pointer"];
    // xdesk said how (its settings' defaults: grow and colour, 4 times, the accent).
    EXPECT_EQ(a["locate"]["mode"], "grow-color") << a.dump();
    EXPECT_EQ(a["locate"]["max"], 4) << a.dump();
    EXPECT_EQ(a["locate"]["color"], "5e9cff") << a.dump();
    EXPECT_EQ(a["drawn"], "default@1") << a.dump();
    const auto drawn = b["drawn"].get<std::string>();
    ASSERT_TRUE(drawn.starts_with("default@")) << b.dump();
    EXPECT_GT(std::stod(drawn.substr(8)), 1.0) << "shaken, it grows: " << b.dump();
    EXPECT_EQ(c["drawn"], "default@1") << "still, it is its size again: " << c.dump();
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

// Super+m held: the rules as cards, the other one lit; Super let go chooses
// it -- and the session after starts as it was left (design 2026-10-19
// §17.5; 2026-10-20 WuXian §3.14). A wuxian.json that cannot be read is kept
// beside, and WuXian starts anew.
XTEST(LubanOs, XdeskChoosesArrangementsHeldWithSuperAndKeepsThem, .area = "luban",
      .covers = {"XDESK-ARRANGEMENTS", "XDESK-COMPOSITOR", "WUXIAN-STATE"}, .requires_ = {"linux", "xlings-bin"}) {
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
    // The workspace's rule as WuXian keeps it (wuxian.json, design 2026-10-20 WuXian §3.14).
    const auto kept = home.root() / ".config" / "xdesk" / "wuxian.json";
    auto rule = [&] { return Json::parse(tk::read_file(kept))["workspaces"][0]["rule"].get<std::string>(); };
    const auto held = home.root() / "held.ppm";
    auto r = session(std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
keydown Super
key m
sleep 800
screenshot {}
screenshot {}
keyup Super
sleep 2000
)", held.string(), artifact("xdesk-chooser.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(read_ppm(held).at(550, 380), kRed) << "the chooser over the desktop while Super is held";
    EXPECT_EQ(rule(), "stack") << "tile's other: chosen on letting Super go";
    // The next session starts as it was left: stack's other is tile.
    r = session(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 1500
key Super+m
sleep 2000
)");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(rule(), "tile");
    // Unreadable: started as new, the file it could not read kept beside it.
    tk::write_file(kept, "{\"wuxian\": [");
    r = session(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
)");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    bool aside = false;
    for (const auto& f : fs::directory_iterator(kept.parent_path()))
        if (f.path().filename().string().starts_with("wuxian.json.before-") && tk::read_file(f.path()) == "{\"wuxian\": [") aside = true;
    EXPECT_TRUE(aside) << "the file it could not read is kept";
    EXPECT_TRUE(Json::parse(tk::read_file(kept)).is_object()) << "and WuXian is kept anew";
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
    // (4 items, a line, the one workspace and WuXian, a line, Close: rows of
    // 30, lines of 9).
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
click 300 50 right
sleep 600
screenshot {}
screenshot {}
click 420 271
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

// A window dragged by its title (design 2026-10-20 WuXian §3.8): the
// arrangement frozen, a preview where it would land, and there it goes when
// let go -- red over the top of green is red above green in green's column.
XTEST(LubanOs, XdeskDraggedWindowLandsWhereItsPreviewWas, .area = "luban",
      .covers = {"XDESK-DRAG-SNAP", "XDESK-COMPOSITOR", "WUXIAN-DRAG"}, .requires_ = {"linux", "xlings-bin"}) {
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
    // While dragged: green's top half shows where red would go (the accent
    // over green, see-through); nothing moves -- green where it is, red's own
    // place kept (design 2026-10-20 WuXian §3.8).
    const auto d = read_ppm(during);
    EXPECT_NE(d.at(1000, 250), kGreen) << "a preview over the top of green";
    EXPECT_NE(d.at(300, 600), kGreen) << "green does not move while red is dragged";
    EXPECT_NE(d.at(1000, 600), kRed) << "green's bottom half is green's still";
    EXPECT_NE(d.at(150, 600), kGreen) << "red's place is kept";
    // Let go: one column, red above green.
    const auto a = read_ppm(after);
    EXPECT_EQ(a.at(550, 200), kRed);
    EXPECT_EQ(a.at(550, 600), kGreen);
}

XTEST(LubanOs, XdeskLauncherStartsWhatIsTypedAndThePanelSwitchesWorkspaces, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-SHELL", "XDESK-APPS", "WUXIAN-WORKSPACES"}, .requires_ = {"linux", "xlings-bin"}) {
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
    const std::vector<std::string> args{"--desktop-exec", xdesk_binary()->string(), "--profile", "classic"};
    const std::map<std::string, std::string> env{{"XDG_RUNTIME_DIR", runtime.string()}, {"XDG_DATA_DIRS", data.string()},
                                                 {"HOME", home.root().string()}};
    // A second workspace (Super+Ctrl+N), back to the first: the status bar
    // shows it -- its shape in its colour (design 2026-10-20 WuXian §3.13).
    const auto bar = home.root() / "bar.ppm";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 500
key Super+Ctrl+n
sleep 500
key Super+1
sleep 1500
screenshot {}
screenshot {}
)", bar.string(), artifact("xdesk-status-bar.bmp").string()), args, env);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto teal = std::array<int, 3>{0x3f, 0xbf, 0xb0};   // workspace 2's colour (openwx::color_for(2))
    int chip = -1;
    {
        const auto img = read_ppm(bar);
        for (int x = 0; x < img.width / 2 && chip < 0; ++x)
            for (int y = 6; y < 24; ++y)
                if (img.at(x, y) == teal) { chip = x + 3; break; }
    }
    ASSERT_GE(chip, 0) << "no shape of workspace 2's colour on the status bar";
    const auto open = home.root() / "open.ppm", started = home.root() / "started.ppm", other = home.root() / "other.ppm";
    r = run_script(home, std::format(R"(wait desktop
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
click {} 15
sleep 800
screenshot {}
)", open.string(), artifact("xdesk-launcher.bmp").string(), started.string(), artifact("xdesk-launched.bmp").string(), chip,
                                     other.string()), args, env);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // The launcher is up, its first row chosen (the accent's blue).
    const auto a = read_ppm(open);
    EXPECT_NE(a.at(550, 210), (std::array<int, 3>{0x1b, 0x1e, 0x25})) << "no launcher over the desktop";
    // Enter started it: green fills the output under the panel.
    const auto b = read_ppm(started);
    EXPECT_EQ(b.at(550, 400), kGreen);
    // Workspace 2 from the status bar (kept from the session before): empty.
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
    // The person's xdesk.ini, which every session reads (the classic one of before too).
    tk::write_file(config / "xdesk" / "xdesk.ini", "[theme]\nbackground = #203040\n[desktop]\ngap = 0\n");
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

namespace {
// A window of a colour on the picture, dimmed or not: a good part of it
// with its hue's channel well above the others (not a ring or a tile of the accent).
bool tinted_(const Image& img, int channel) {
    int n = 0;
    for (int y = 0; y < img.height; y += 4)
        for (int x = 0; x < img.width; x += 4) {
            const auto c = img.at(x, y);
            const int other = std::max(c[(channel + 1) % 3], c[(channel + 2) % 3]);
            if (c[channel] > other + 60) ++n;
        }
    return n > 1500;
}
}  // namespace

// Alt+Tab (design 2026-10-20 §10.2): a quick press is the previous window,
// nothing shown; held, every window of the workspace where it is to the
// others, fitted, the chosen one as it is and the rest dimmed; Tab by
// recency, and letting Alt go focuses it, the windows back in their places.
XTEST(LubanOs, XdeskAltTabGoesBackQuicklyAndHeldShowsEveryWindowWhereItIs, .area = "luban",
      .covers = {"XDESK-SWITCHERS", "XDESK-HIGHLIGHT"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-alt-tab");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto quick = home.root() / "quick.ppm", spread = home.root() / "spread.ppm", lit = home.root() / "lit.ppm",
               chosen = home.root() / "chosen.ppm";
    auto r = xdesk_with(home, "classic", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
key Alt+Tab
sleep 2000
key Super+Left
sleep 1000
screenshot {}
keydown Alt
key Tab
sleep 1000
screenshot {}
screenshot {}
key Tab
keyup Alt
sleep 300
screenshot {}
sleep 2000
screenshot {}
)", quick.string(), spread.string(), artifact("xdesk-alt-tab.bmp").string(), lit.string(), chosen.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Blue was focused; Alt+Tab went to green, and left of green is red.
    const auto a = read_ppm(quick);
    EXPECT_EQ(a.at(275, 400), kRed);
    EXPECT_EQ(a.at(825, 400), kGreen);
    // Held: every window as it is, fitted, over a dimmed output; green (the
    // one before red) chosen: its own outline lit, nothing drawn around it
    // (design 2026-10-21 §8.2, J15).
    const auto b = read_ppm(spread);
    const auto g = box_of(b, kGreen);
    ASSERT_GE(g[0], 0) << "the chosen window is shown as it is";
    EXPECT_LT(g[2] - g[0], 540) << "fitted: smaller than its column";
    EXPECT_GE(box_of(b, kRed)[0], 0) << "red shown as it is, not dimmed";
    EXPECT_GE(box_of(b, kBlue)[0], 0) << "blue shown as it is, not dimmed";
    EXPECT_NE(b.at(g[0] - 1, (g[1] + g[3]) / 2), kGreen) << "the chosen one's outline on its own edge";
    // Tab: blue; Alt let go: blue focused, its outline lit where it is and
    // faded -- the others never dimmed and lit again.
    const auto l = read_ppm(lit);
    EXPECT_EQ(l.at(825, 400), kBlue) << "the one chosen";
    EXPECT_EQ(l.at(275, 400), kGreen) << "the others as they are while it is lit";
    const auto c = read_ppm(chosen);
    EXPECT_EQ(c.at(275, 400), kGreen);
    EXPECT_EQ(c.at(825, 400), kBlue);
    EXPECT_GT(box_of(l, kBlue)[0], box_of(c, kBlue)[0]) << "the outline lit over its edge, then faded";
}

// Super+Tab by application (design 2026-10-20 §10.1): a press goes to the
// application used before, its most recent window; Super+` to the
// application's other window; held on one, its windows are spread below,
// the others not shown, and an arrow chooses among them.
XTEST(LubanOs, XdeskSuperTabGoesByApplicationAndSpreadsItsWindows, .area = "luban",
      .covers = {"XDESK-SWITCHERS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-super-tab");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto byapp = home.root() / "byapp.ppm", other = home.root() / "other.ppm", open = home.root() / "open.ppm",
               chosen = home.root() / "chosen.ppm";
    // Red and green are one application's, blue another's.
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
key Super+Tab
sleep 600
key Super+Left
sleep 1000
screenshot {}
key Super+`
sleep 400
key Super+Right
sleep 1000
screenshot {}
keydown Super
key Tab
sleep 1800
screenshot {}
screenshot {}
key Left
keyup Super
sleep 1200
screenshot {}
)", byapp.string(), other.string(), open.string(), artifact("xdesk-super-tab.bmp").string(), chosen.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic",
                         "--autostart", std::format("python3 {} c04040 --app-id term", client),
                         "--autostart", std::format("sh -c 'sleep 0.6; exec python3 {} 40c040 --app-id term'", client),
                         "--autostart", std::format("sh -c 'sleep 1.2; exec python3 {} 4040c0 --app-id files'", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // From blue (files) to term's most recent, green; left of it, red.
    const auto a = read_ppm(byapp);
    EXPECT_TRUE(near_colour(a.at(275, 400), kRed)) << "kRed at (275, 400)";
    EXPECT_TRUE(near_colour(a.at(825, 400), kGreen)) << "kGreen at (825, 400)";
    // Super+`: from red to term's other window, green; right of it, blue.
    const auto b = read_ppm(other);
    EXPECT_TRUE(near_colour(b.at(275, 400), kGreen)) << "kGreen at (275, 400)";
    EXPECT_TRUE(near_colour(b.at(825, 400), kBlue)) << "kBlue at (825, 400)";
    // Held on term: green and red spread below the row, blue not shown.
    const auto c = read_ppm(open);
    EXPECT_GE(box_of(c, kGreen)[0], 0) << "term's most recent window, as it is";
    EXPECT_TRUE(tinted_(c, 0)) << "term's other window";
    EXPECT_FALSE(tinted_(c, 2)) << "files' window is not shown";
    // Left: red, chosen on letting Super go.
    const auto d = read_ppm(chosen);
    EXPECT_TRUE(near_colour(d.at(275, 400), kRed)) << "kRed at (275, 400)";
    EXPECT_TRUE(near_colour(d.at(825, 400), kGreen)) << "kGreen at (825, 400)";
}

// A second window on a running desktop is another display of it (design
// 2026-10-20 §11): it mirrors the first -- the same picture at the first's
// size, whatever its own, said in its title -- its input is the desktop's;
// when the first goes the desktop goes on, this window the primary.
XTEST(LubanOs, ASecondWindowMirrorsTheDesktopAndGoesOnWhenTheFirstCloses, .area = "luban",
      .covers = {"LUBAN-DISPLAYS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto first = tk::Home::isolated("luban-os-display-1");
    auto second = tk::Home::isolated("luban-os-display-2");
    ASSERT_EQ(first.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(second.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = first.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto socket = runtime / "displays.sock";
    // What run_session does for a second window: its stream joined to the socket.
    const auto relay = second.root() / "relay.py";
    tk::write_file(relay, R"PY(import os, select, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
while True:
    r, _, _ = select.select([0, s], [], [])
    for f in r:
        data = os.read(0, 65536) if f == 0 else s.recv(65536)
        if not data: sys.exit(0)
        if f == 0: s.sendall(data)
        else: os.write(1, data)
)PY");
    const auto client = colour_client(first).string();
    auto one = std::async(std::launch::async, [&] {
        return run_script(first, R"(wait desktop
waitfor desktop.texture.0 1100
sleep 7000
)", {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart", std::format("python3 {} c04040", client)},
                          {{"XDG_RUNTIME_DIR", runtime.string()}, {"OPENSAL_DISPLAYS", socket.string()}});
    });
    for (int k = 0; k < 100 && !fs::exists(socket); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(fs::exists(socket)) << "xdesk listens for other displays";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto mirror = second.root() / "mirror.json", shot = second.root() / "mirror.ppm", primary = second.root() / "primary.json";
    auto two = run_script(second, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 1000
resize 800 500
sleep 800
state {}
screenshot {}
screenshot {}
keydown Super
key m
keyup Super
sleep 6000
resize 900 600
sleep 1500
state {}
)", mirror.string(), shot.string(), artifact("xdesk-mirror.bmp").string(), primary.string()),
                          {"--desktop-exec", "python3", relay.string(), socket.string()});
    const auto r1 = one.get();
    ASSERT_EQ(two.exit_code, 0) << two.transcript();
    ASSERT_EQ(r1.exit_code, 0) << r1.transcript();
    // A mirror: the first window's picture (its red window), at its size, not this one's.
    const auto m = Json::parse(tk::read_file(mirror));
    EXPECT_EQ(m["desktop"]["texture"][0], 1100) << m.dump();
    EXPECT_NE(m["window"]["title"].get<std::string>().find("mirroring display 1"), std::string::npos) << m["window"]["title"];
    EXPECT_GE(box_of(read_ppm(shot), kRed)[0], 0) << "the desktop's window, shown in the second";
    // Its keys are the desktop's: Super+m there arranged the workspace (WuXian keeps it).
    const auto state = Json::parse(tk::read_file(first.root() / ".config" / "xdesk" / "wuxian.json"));
    EXPECT_EQ(state["workspaces"][0]["rule"], "stack") << state.dump();
    // The first closed: the desktop went on, this window its primary at its own size.
    const auto p = Json::parse(tk::read_file(primary));
    EXPECT_NE(p["desktop"]["texture"][0], 1100) << p.dump();
    EXPECT_EQ(p["window"]["title"].get<std::string>().find("mirroring"), std::string::npos) << p["window"]["title"];
}

// The keys the documents list are the desktop's own table (design
// 2026-10-20 §12.4): generated, so they are what the desktop does.
// XDESK_KEYS_WRITE=1 writes them again.
XTEST(LubanOs, XdesksKeysDocumentIsItsOwnTable, .area = "luban", .covers = {"XDESK-KEYS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    for (const auto zh : {false, true}) {
        tk::RunOptions o;
        o.argv = {xdesk_binary()->string(), "--keys-markdown", zh ? "zh" : "en"};
        const auto r = tk::run(o);
        ASSERT_EQ(r.exit_code, 0) << r.transcript();
        const auto doc = zh ? std::string("# xdesk 的快捷键\n\n<!-- 由 `xdesk --keys-markdown zh` 生成，请勿手改。键位测试在 XDESK_KEYS_WRITE=1 时重写它。 -->\n\n")
                            : std::string("# xdesk's keys\n\n<!-- Generated from `xdesk --keys-markdown en`; do not edit by hand. The keys test rewrites it with XDESK_KEYS_WRITE=1. -->\n\n");
        const auto file = fs::current_path() / "docs" / "generated" / (zh ? "xdesk-keys.zh.md" : "xdesk-keys.md");
        if (const char* w = std::getenv("XDESK_KEYS_WRITE"); w && std::string_view(w) == "1") tk::write_file(file, doc + r.out);
        EXPECT_EQ(tk::read_file(file), doc + r.out) << file << " is not the desktop's table: XDESK_KEYS_WRITE=1 writes it again";
    }
}

// The settings and the control socket (design 2026-10-20 §11.4, §12.3,
// §12.4): a key given to an action at the keys page, or by `xdesk msg`,
// is what the desktop answers to and what keys.json keeps; a page's choice
// takes effect at once.
XTEST(LubanOs, XdesksSettingsAndControlSocketChangeWhatItDoes, .area = "luban", .covers = {"XDESK-KEYS", "XDESK-SETTINGS"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-settings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    const auto settings = home.root() / "settings.ppm", maximized = home.root() / "maximized.ppm";
    auto session = std::async(std::launch::async, [&] {
        return xdesk_with(home, "classic", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 6000
key Super+Shift+m
sleep 1000
screenshot {}
key Super+Shift+m
sleep 600
key Super+i
sleep 800
screenshot {}
screenshot {}
key Tab
key Right
sleep 600
key Escape
sleep 300
key Super+/
sleep 800
type maxim
sleep 1000
key Down
key Down
key Down
key Return
sleep 600
key Super+Shift+x
sleep 1000
key Escape
sleep 600
)", maximized.string(), settings.string(), artifact("xdesk-settings.bmp").string()));
    });
    // The control socket, as `xdesk msg` uses it.
    fs::path control;
    for (int k = 0; k < 100 && control.empty(); ++k) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(runtime, ec))
            if (e.path().filename().string().starts_with("xdesk-") && e.path().extension() == ".sock") control = e.path();
        if (control.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_FALSE(control.empty()) << "no control socket in " << runtime;
    const auto display = control.stem().string().substr(6);
    auto msg = [&](std::vector<std::string> args) {
        tk::RunOptions o;
        o.argv = {xdesk_binary()->string(), "msg"};
        o.argv.insert(o.argv.end(), args.begin(), args.end());
        o.env = {{"XDG_RUNTIME_DIR", runtime.string()}, {"WAYLAND_DISPLAY", display}};
        return tk::run(o);
    };
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto bound = msg({"keys", "bind", "maximize", "Super+Shift+m"});
    EXPECT_EQ(bound.exit_code, 0) << bound.transcript();
    const auto bad = msg({"keys", "bind", "nothing", "Super+Shift+m"});
    EXPECT_NE(bad.exit_code, 0);
    EXPECT_NE(bad.out.find("no action 'nothing'"), std::string::npos) << bad.out;
    const auto list = msg({"keys", "list"});
    ASSERT_EQ(list.exit_code, 0) << list.transcript();
    const auto listed = Json::parse(list.out, nullptr, false);
    ASSERT_TRUE(listed.is_object()) << list.out;
    bool found = false;
    for (const auto& a : listed["actions"])
        if (a["action"] == "maximize") {
            found = true;
            EXPECT_EQ(a["keys"], Json::array({"Super+Shift+M"})) << a.dump();
            EXPECT_TRUE(a["changed"].get<bool>());
        }
    EXPECT_TRUE(found) << list.out;
    const auto r = session.get();
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Super+Shift+M, as bound: blue (focused) over the whole width.
    const auto m = read_ppm(maximized);
    EXPECT_EQ(m.at(100, 400), kBlue);
    EXPECT_EQ(m.at(1000, 400), kBlue);
    // The settings over the desktop.
    const auto s = read_ppm(settings);
    EXPECT_NE(s.at(550, 400), kBlue) << "a card over the windows";
    // The layout page's first choice: this workspace stacked, kept (wuxian.json).
    const auto state = Json::parse(tk::read_file(home.root() / ".config" / "xdesk" / "wuxian.json"));
    EXPECT_EQ(state["workspaces"][0]["rule"], "stack") << state.dump();
    // The keys page: maximize given Super+Shift+X, kept in keys.json.
    const auto keys = Json::parse(tk::read_file(home.root() / ".config" / "xdesk" / "keys.json"));
    EXPECT_EQ(keys["maximize"], Json::array({"Super+Shift+x"})) << keys.dump();
}

// What the desktop is (design 2026-10-21 §8.11): `xdesk msg state` -- its
// output, view, workspaces and windows, the overlays (the hint where it is
// drawn: inside the output after a resize, J10), the launcher's
// applications, the frames' parts; `debug hud`; `subscribe` tells what
// happens, a line each.
XTEST(LubanOs, XdeskSaysWhatItIsAndWhatHappens, .area = "luban", .covers = {"XDESK-STATE"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-state");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    auto session = std::async(std::launch::async, [&] {
        return xdesk_with(home, "classic", R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
resize 900 520
sleep 5000
key Alt+Tab
sleep 2500
)");
    });
    fs::path control;
    for (int k = 0; k < 100 && control.empty(); ++k) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(runtime, ec))
            if (e.path().filename().string().starts_with("xdesk-") && e.path().extension() == ".sock") control = e.path();
        if (control.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_FALSE(control.empty()) << "no control socket in " << runtime;
    const auto display = control.stem().string().substr(6);
    auto msg = [&](std::vector<std::string> args, std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
        tk::RunOptions o;
        o.argv = {xdesk_binary()->string(), "msg"};
        o.argv.insert(o.argv.end(), args.begin(), args.end());
        o.env = {{"XDG_RUNTIME_DIR", runtime.string()}, {"WAYLAND_DISPLAY", display}};
        o.timeout = timeout;
        return tk::run(o);
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(4500));
    auto subscribed = std::async(std::launch::async, [&] { return msg({"subscribe"}, std::chrono::seconds(6)); });
    const auto st = msg({"state"});
    ASSERT_EQ(st.exit_code, 0) << st.transcript();
    const auto s = Json::parse(st.out, nullptr, false);
    ASSERT_TRUE(s.is_object()) << st.out;
    EXPECT_EQ(s["output"]["size"], Json::array({900, 520})) << s["output"].dump();
    EXPECT_EQ(s["view"], "workspace");
    EXPECT_EQ(s["windows"].size(), 3u) << s["windows"].dump();
    EXPECT_EQ(std::ranges::count_if(s["windows"], [](const Json& w) { return w.value("focused", false); }), 1) << s["windows"].dump();
    EXPECT_TRUE(s["apps"].is_array());
    EXPECT_GT(s["frames"]["count"].get<double>(), 0) << s["frames"].dump();
    const auto& hint = s["overlays"]["hint"];
    ASSERT_TRUE(hint.value("shown", false)) << "the first session's hint\n" << s["overlays"].dump();
    const auto r = hint["rect"];
    EXPECT_LE(r[1].get<double>() + r[3].get<double>(), 520) << "the hint inside the output after the resize (J10)\n" << hint.dump();
    EXPECT_LE(r[0].get<double>() + r[2].get<double>(), 900) << hint.dump();
    const auto on = msg({"debug", "hud", "on"});
    EXPECT_EQ(on.exit_code, 0) << on.transcript();
    EXPECT_TRUE(Json::parse(msg({"state"}).out, nullptr, false)["hud"].get<bool>());
    const auto run = session.get();
    ASSERT_EQ(run.exit_code, 0) << run.transcript();
    const auto events = subscribed.get().out;
    EXPECT_NE(events.find(R"("event":"focus")"), std::string::npos) << "Alt+Tab focused another window\n" << events;
}

// The desktop on the bus (design 2026-10-22 §7, B6): its state and its
// control as the socket's, what happens as a topic, and its process's
// telemetry -- the frames' parts as metrics, a profile of a frame's spans.
// A patient node: the router here comes after the desktop has started.
XTEST(LubanOs, XdeskIsOnTheBus, .area = "luban", .covers = {"XDESK-BUS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-bus");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const xbus::Address at{"local", (home.root() / "bus.sock").string()};
    auto session = std::async(std::launch::async, [&] {
        return xdesk_with(home, "classic", R"(wait desktop
waitfor desktop.texture.0 1100
sleep 9000
key Alt+Tab
sleep 4000
)", {{"OPENBUS_ROUTER", at.text()}});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    xbus::Router router({.root = "host/test", .listen = {at}});
    ASSERT_TRUE(router.listen().has_value());
    std::atomic<bool> stop{false};
    std::jthread loop([&] { router.run(stop); });
    auto n = xbus::Node::connect({.name = "test", .router = at, .telemetry = false});
    ASSERT_TRUE(n.has_value()) << n.error();
    auto& bus = **n;
    std::vector<xbus::Resolved> desk;
    for (int k = 0; k < 80 && desk.empty(); ++k) {
        desk = bus.resolve("**", "xdesk.state@1").value_or(std::vector<xbus::Resolved>{});
        if (desk.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(desk.size(), 1u) << "xdesk joined the bus that came after it";
    EXPECT_EQ(desk.front().path, "host/test/service/xdesk/state");
    std::mutex m;
    std::string events;
    bus.subscribe("**/topic/xdesk/events", [&](const xbus::Message& msg) {
        std::lock_guard l(m);
        events += msg.body + "\n";
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const auto st = bus.call("service/xdesk/state");
    ASSERT_TRUE(st.has_value()) << st.error().message;
    const auto s = Json::parse(st->front(), nullptr, false);
    ASSERT_TRUE(s.is_object()) << st->front();
    EXPECT_EQ(s["windows"].size(), 3u) << s.dump();
    // The socket's requests, the same answers.
    const auto hud = bus.call("service/xdesk/control", R"({"debug": "hud", "on": true})");
    ASSERT_TRUE(hud.has_value()) << hud.error().message;
    EXPECT_NE(hud->front().find("true"), std::string::npos) << hud->front();
    EXPECT_TRUE(Json::parse(bus.call("service/xdesk/state")->front())["hud"].get<bool>());
    // Its telemetry: the frames by their parts, a profile, the state in a dump.
    std::vector<xbus::Resolved> told;
    for (const auto& e : bus.resolve("**", "openbus.telemetry@0.1").value_or(std::vector<xbus::Resolved>{}))
        if (e.name == "xdesk") told.push_back(e);
    ASSERT_EQ(told.size(), 1u);
    auto ask = [&](std::string_view request) {
        const auto r = bus.call(told.front().path, request);
        std::string text;
        if (r) for (const auto& p : *r) text += p;
        return Json::parse(text, nullptr, false);
    };
    EXPECT_EQ(ask("profile on")["profiling"], true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const auto metrics = ask("metrics")["metrics"];
    EXPECT_GT(metrics["counters"].value("xdesk.frames", 0u), 0u) << metrics.dump();
    EXPECT_GT(metrics["histograms"]["xdesk.frame.compose_ns"].value("count", 0u), 0u) << metrics.dump();
    const auto trace = ask("trace");
    EXPECT_TRUE(std::ranges::any_of(trace["traceEvents"], [](const Json& e) { return e["name"] == "compose" && e["cat"] == "xdesk"; }))
        << "a frame's spans while profiling";
    (void)ask("profile off");
    const auto dump = ask("dump");
    EXPECT_EQ(dump["program"], "xdesk");
    EXPECT_EQ(dump["state"]["xdesk.state"]["windows"].size(), 3u) << dump["state"].dump().substr(0, 400);
    const auto run = session.get();
    stop = true;
    ASSERT_EQ(run.exit_code, 0) << run.transcript();
    std::lock_guard l(m);
    EXPECT_NE(events.find(R"("event":"focus")"), std::string::npos) << "Alt+Tab focused another window, told on the topic\n" << events;
}

// The renderer (design 2026-10-20 WuXian §7.2): the GPU when one draws, else
// software -- and every step that fails says why, as the log and the About
// page say it. CI has no GPU: each failure is made to happen.
XTEST(LubanOs, XdeskDrawsInSoftwareWhenNoGpuDrawsAndSaysWhy, .area = "luban", .covers = {"GPU-SELECT"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-renderer");
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    auto start = [&](std::map<std::string, std::string> extra) {
        tk::RunOptions o;
        o.argv = {xdesk_binary()->string(), "--profile", "xdesk"};
        o.env = {{"PATH", "/usr/bin:/bin"}, {"HOME", home.root().string()}, {"XDG_RUNTIME_DIR", runtime.string()},
                 {"XDG_CONFIG_HOME", (home.root() / "config").string()}};
        o.env.insert(extra.begin(), extra.end());
        // Its window's hello, then the stream ends: xdesk starts, draws, ends.
        const std::string hello = "65535 800 600 1 0";
        o.stdin_data = std::string("H") + char(0) + char(0) + char(0) + char(hello.size()) + hello;
        o.timeout = std::chrono::seconds(30);
        return tk::run(o);
    };
    auto said = [](const tk::RunResult& r) {
        const auto at = r.err.find("xdesk: renderer: ");
        return at == std::string::npos ? std::string{} : r.err.substr(at, r.err.find('\n', at) - at);
    };
    const auto chosen = start({{"XDESK_RENDERER", "pixman"}});
    EXPECT_NE(said(chosen).find("software (pixman): XDESK_RENDERER=pixman"), std::string::npos) << chosen.transcript();
    const auto none = start({{"XDESK_RENDER_NODES", (home.root() / "no-such-node").string()}});
    EXPECT_NE(said(none).find("no render node"), std::string::npos) << none.transcript();
    // A node that opens and is no GPU: EGL and GLES do not start on it.
    const auto not_gpu = start({{"XDESK_RENDER_NODES", "/dev/null"}});
    EXPECT_NE(said(not_gpu).find("EGL and GLES did not start on /dev/null"), std::string::npos) << not_gpu.transcript();
    // One that cannot be opened.
    const auto closed = home.root() / "closed";
    tk::write_file(closed, "");
    fs::permissions(closed, fs::perms::none, fs::perm_options::replace);
    const auto refused = start({{"XDESK_RENDER_NODES", closed.string()}});
    if (!std::ifstream(closed)) EXPECT_NE(said(refused).find("cannot be opened"), std::string::npos) << refused.transcript();
}
