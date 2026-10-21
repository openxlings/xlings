// xdesk's interactions, headless (design 2026-10-21 §8): the desktop under
// the Luban OS window, driven through its control -- input given as the
// window gives it, where to point read from its state -- and what it did
// read back from the state. Each test is one session and several scenes.
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

namespace {

constexpr std::string_view kBlueTitle = "4040c0";   // the colour client's title is its colour; blue is the newest

// Three windows there, and none still on its way.
Json settled(const XdeskSession& d) {
    (void)d.until([](const Json& s) {
        if (s["windows"].size() != 3) return false;
        return std::ranges::all_of(s["windows"], [](const Json& w) { return w.contains("rect"); });
    }, std::chrono::seconds(20));
    return d.still();
}

Json press_drag(double x0, double y0, double x1, double y1, std::string button = "left") {
    Json steps = Json::array({Json{{"move", {x0, y0}}}, Json{{"button", button}, {"down", true}}});
    for (int k = 1; k <= 4; ++k) steps.push_back(Json{{"move", {x0 + (x1 - x0) * k / 4, y0 + (y1 - y0) * k / 4}}});
    steps.push_back(Json{{"button", button}, {"down", false}});
    return steps;
}

}  // namespace

// A stacked workspace: a window dragged far off keeps its title bar on the
// screen (§8.8); a maximized one covers the others, each gets a bookmark on
// it; pointing at one lights it, clicking it raises and focuses its window (§8.4).
XTEST(XdeskInteraction, StackedWindowsKeepTheirTitleBarsAndCoveredOnesAreBookmarked, .area = "luban",
      .covers = {"XDESK-BOUNDS", "WUXIAN-BOOKMARKS", "XDESK-HEADLESS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-stacked");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    tk::write_file(home.root() / ".config" / "xdesk" / "wuxian.json", R"({"wuxian": 1, "workspaces": [{"id": 1, "rule": "stack"}]})");
    XdeskSession d(home, "classic");
    ASSERT_TRUE(d.ready());
    auto s = settled(d);
    ASSERT_TRUE(s.is_object()) << d.last_failure;
    ASSERT_EQ(s["windows"].size(), 3u) << s.dump();
    const double out_w = s["output"]["size"][0].get<double>();
    // The newest (blue) by its title bar, far up and to the left.
    ASSERT_TRUE(window_titled(s, kBlueTitle).is_object()) << s["windows"].dump();
    auto blue = rect_of(window_titled(s, kBlueTitle));
    ASSERT_TRUE(d.input(press_drag(blue[0] + 40, blue[1] + 8, -2000, -2000)));
    s = d.still();
    ASSERT_TRUE(window_titled(s, kBlueTitle).is_object()) << s.dump().substr(0, 600) << "\n" << d.last_failure << "\n" << d.finish().transcript();
    blue = rect_of(window_titled(s, kBlueTitle));
    EXPECT_GE(blue[1], 30) << "not above the top, nor under the status bar";
    EXPECT_GE(blue[0] + blue[2], std::min(64.0, blue[2] / 4) - 1) << "some of its title bar on the output";
    // And far down and to the right.
    ASSERT_TRUE(d.input(press_drag(blue[0] + 40, blue[1] + 8, 9000, 9000)));
    s = d.still();
    blue = rect_of(window_titled(s, kBlueTitle));
    EXPECT_LE(blue[0], out_w - std::min(64.0, blue[2] / 4) + 1);
    EXPECT_LE(blue[1], s["output"]["size"][1].get<double>() - 26 + 1);
    // Maximized: the two others under it, each a bookmark on it.
    // (Taken to an edge, it may have been maximized already: the edge's snap.)
    if (rect_of(window_titled(d.still(), kBlueTitle))[2] < out_w - 20) ASSERT_TRUE(d.key("Super+f"));
    s = d.until([](const Json& j) { return j["bookmarks"].size() == 2; });
    ASSERT_EQ(s["bookmarks"].size(), 2u) << s["windows"].dump();
    const auto tab = s["bookmarks"][1]["tab"];
    const double tx = tab[0].get<double>() + tab[2].get<double>() / 2, ty = tab[1].get<double>() + tab[3].get<double>() / 2;
    ASSERT_TRUE(d.move(tx, ty));
    s = d.still();
    EXPECT_TRUE(s["bookmarks"][1].value("lit", false)) << "pointed at: lit";
    const double chosen = s["bookmarks"][1]["window"].get<double>();
    ASSERT_TRUE(d.click(tx, ty));
    s = d.until([&](const Json& j) {
        return std::ranges::any_of(j["windows"], [&](const Json& w) { return w.value("focused", false) && w["id"].get<double>() == chosen; });
    });
    EXPECT_TRUE(std::ranges::any_of(s["windows"], [&](const Json& w) { return w.value("focused", false) && w["id"].get<double>() == chosen; }))
        << s["windows"].dump();
    const auto run = d.finish();
    EXPECT_EQ(run.exit_code, 0) << run.transcript();
}

// The WuXian view (§8.5-§8.7): a free window's border resizes it; a
// double-click on its title bar collapses it to the title bar and back;
// zoomed far out a window is a picture, moved by any part of it.
XTEST(XdeskInteraction, AFreeWindowResizesCollapsesAndMovesAsAPicture, .area = "luban",
      .covers = {"XDESK-WUXIAN-REACH", "WUXIAN-COLLAPSE", "XDESK-HEADLESS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-free");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    XdeskSession d(home, "classic");
    ASSERT_TRUE(d.ready());
    auto s = settled(d);
    ASSERT_EQ(s["windows"].size(), 3u);
    // The focused one (blue, the newest) onto the plane, then the WuXian view.
    ASSERT_TRUE(d.key("Super+Shift+0"));
    ASSERT_TRUE(d.key("Super+o"));
    s = d.until([](const Json& j) {
        const auto b = window_titled(j, kBlueTitle);
        return j["view"] == "wuxian" && b.is_object() && b.value("workspace", -1) == 0;
    });
    s = d.still();
    auto blue = rect_of(window_titled(s, kBlueTitle));
    ASSERT_EQ(s["view"], "wuxian");
    // At 100 %, in the middle of the view: its borders within reach.
    ASSERT_TRUE(d.key("Super+0"));
    s = d.until([](const Json& j) { return j["camera"]["zoom"].get<double>() >= 0.99; });
    s = d.still();
    blue = rect_of(window_titled(s, kBlueTitle));
    // Its right border, 6 screen pixels: pressed there, a resize.
    ASSERT_TRUE(d.input(Json::array({Json{{"move", {blue[0] + blue[2] - 3, blue[1] + blue[3] / 2}}}, Json{{"button", "left"}, {"down", true}}})));
    {
        const auto pressed = d.state();
        ASSERT_TRUE(d.input(Json::array({Json{{"button", "left"}, {"down", false}}})));
        EXPECT_EQ(pressed["drag"], "resize") << pressed["pointer"].dump() << " " << window_titled(pressed, kBlueTitle).dump() << d.ask(Json{{"telemetry", "recent 6"}}).dump();
    }
    // Dragged 80 to the right.
    ASSERT_TRUE(d.input(press_drag(blue[0] + blue[2] - 3, blue[1] + blue[3] / 2, blue[0] + blue[2] - 3 + 80, blue[1] + blue[3] / 2)));
    s = d.still();
    const auto wider = rect_of(window_titled(s, kBlueTitle));
    EXPECT_NEAR(wider[2], blue[2] + 80, 3) << "its border resized it\n" << s["camera"].dump() << "\n" << s["windows"].dump();
    EXPECT_NEAR(wider[0], blue[0], 1);
    // Twice on its title bar: collapsed to it; twice again: back.
    const double tx = wider[0] + wider[2] / 2, ty = wider[1] + 6;
    ASSERT_TRUE(d.click(tx, ty, 2));
    s = d.until([](const Json& j) { return window_titled(j, kBlueTitle).value("collapsed", false); });
    auto c = window_titled(s, kBlueTitle);
    ASSERT_TRUE(c.value("collapsed", false)) << c.dump();
    EXPECT_LT(rect_of(c)[3], 40) << "its title bar only";
    ASSERT_TRUE(d.click(tx, ty, 2));
    s = d.until([](const Json& j) { return !window_titled(j, kBlueTitle).value("collapsed", true); });
    EXPECT_NEAR(rect_of(window_titled(s, kBlueTitle))[3], wider[3], 1);
    // Zoomed far out: pictures; the middle of one moves it.
    for (int k = 0; k < 12 && s["camera"]["zoom"].get<double>() >= 0.45; ++k) {
        ASSERT_TRUE(d.key("Super+minus"));
        s = d.still();
    }
    ASSERT_LT(s["camera"]["zoom"].get<double>(), 0.5);
    blue = rect_of(window_titled(s, kBlueTitle));
    const double cx = blue[0] + blue[2] / 2, cy = blue[1] + blue[3] / 2;
    ASSERT_TRUE(d.input(press_drag(cx, cy, cx + 60, cy + 40)));
    s = d.still();
    const auto moved = rect_of(window_titled(s, kBlueTitle));
    EXPECT_NEAR(moved[0], blue[0] + 60, 3);
    EXPECT_NEAR(moved[1], blue[1] + 40, 3);
    const auto run = d.finish();
    EXPECT_EQ(run.exit_code, 0) << run.transcript();
}

// The minimap (§8.9): clicking a workspace there puts it in the middle of
// the view; dragging the view's box pans; dragging a free window there moves it.
XTEST(XdeskInteraction, TheMinimapCentresPansAndMoves, .area = "luban", .covers = {"XDESK-MINIMAP", "XDESK-HEADLESS"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-minimap");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    XdeskSession d(home, "classic");
    ASSERT_TRUE(d.ready());
    auto s = settled(d);
    ASSERT_TRUE(d.key("Super+Shift+0"));
    ASSERT_TRUE(d.key("Super+o"));
    // Zoomed in until the plane holds more than the view: the minimap shows.
    for (int k = 0; k < 20; ++k) {
        s = d.still();
        if (s.contains("minimap")) break;
        ASSERT_TRUE(d.key("Super+equal"));
    }
    ASSERT_TRUE(s.contains("minimap")) << s["camera"].dump();
    auto pixel = [&](const Json& st, double wx, double wy) {
        const auto& m = st["minimap"];
        const double pp = m["per_pixel"].get<double>();
        return std::pair{m["rect"][0].get<double>() + (wx - m["origin"][0].get<double>()) / pp,
                         m["rect"][1].get<double>() + (wy - m["origin"][1].get<double>()) / pp};
    };
    // The view's box dragged by its edge: the camera moves by what the box moved, in the world.
    const auto& view = s["minimap"]["view"];
    const auto at = s["camera"]["at"];
    const double pp = s["minimap"]["per_pixel"].get<double>();
    const auto [ex, ey] = pixel(s, view[0].get<double>(), view[1].get<double>() + view[3].get<double>() / 2);
    ASSERT_TRUE(d.input(press_drag(ex, ey, ex + 12, ey + 8)));
    s = d.still();
    EXPECT_NEAR(s["camera"]["at"][0].get<double>() - at[0].get<double>(), 12 * pp, 2 * pp) << s["camera"].dump();
    EXPECT_NEAR(s["camera"]["at"][1].get<double>() - at[1].get<double>(), 8 * pp, 2 * pp) << s["camera"].dump();
    // A workspace clicked there: in the middle of the view.
    const auto frame = s["workspaces"][0]["frame"];
    ASSERT_TRUE(frame.is_array());
    const auto cam = s["camera"];
    const double zoom = cam["zoom"].get<double>();
    // The workspace's centre on the plane, from its frame on the screen.
    const double top = 30;   // the status bar
    const double wcx = cam["at"][0].get<double>() + (frame[0].get<double>() + frame[2].get<double>() / 2) / zoom;
    const double wcy = cam["at"][1].get<double>() + (frame[1].get<double>() - top + frame[3].get<double>() / 2) / zoom;
    const auto [px, py] = pixel(s, wcx, wcy);
    ASSERT_TRUE(d.click(px, py));
    s = d.still();
    const auto f2 = s["workspaces"][0]["frame"];
    const double out_w = s["output"]["size"][0].get<double>(), out_h = s["output"]["size"][1].get<double>();
    EXPECT_NEAR(f2[0].get<double>() + f2[2].get<double>() / 2, out_w / 2, out_w * 0.1) << "in the middle across";
    EXPECT_NEAR(f2[1].get<double>() + f2[3].get<double>() / 2, top + (out_h - top) / 2, out_h * 0.1) << "and down";
    const auto run = d.finish();
    EXPECT_EQ(run.exit_code, 0) << run.transcript();
}

// The keys (design 2026-10-20 §12.2): in a tiled workspace the focus goes
// each way, a window moves, joins a column and leaves it, takes its column's
// widths, maximizes, goes full screen and floats; workspaces are made, gone
// to, and a window sent between them.
XTEST(XdeskInteraction, TheKeysMoveTheFocusTheWindowsAndTheWorkspaces, .area = "luban", .covers = {"XDESK-KEYS", "XDESK-HEADLESS"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-keys");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    XdeskSession d(home, "classic");
    ASSERT_TRUE(d.ready());
    auto s = settled(d);
    ASSERT_EQ(s["windows"].size(), 3u);
    const double out_w = s["output"]["size"][0].get<double>();
    auto focused = [](const Json& j) {
        for (const auto& w : j["windows"])
            if (w.value("focused", false)) return w;
        return Json();
    };
    auto key = [&](std::string chord) {
        EXPECT_TRUE(d.key(chord)) << chord;
        s = d.still();
        return focused(s);
    };
    EXPECT_EQ(focused(s)["title"], kBlueTitle) << "the newest has the focus";
    EXPECT_EQ(key("Super+Left")["title"], "40c040") << "the focus to the left";
    EXPECT_EQ(key("Super+Right")["title"], kBlueTitle);
    // Into the column on its left, and out again.
    auto in = key("Super+bracketleft");
    const auto green = window_titled(s, "40c040");
    EXPECT_NEAR(rect_of(in)[0], rect_of(green)[0], 1) << "one column: the same left edge";
    EXPECT_GT(rect_of(in)[1], rect_of(green)[1]) << "under it";
    auto out = key("Super+bracketright");
    EXPECT_GT(rect_of(out)[0], rect_of(window_titled(s, "40c040"))[0]) << "a column of its own again";
    // Its column's width: another of the widths.
    const double before = rect_of(out)[2];
    EXPECT_NE(rect_of(key("Super+r"))[2], before);
    // Maximized and back; full screen and back; floating and back.
    const double width = rect_of(focused(s))[2];
    EXPECT_GT(rect_of(key("Super+f"))[2], out_w - 20) << "maximized: the whole width";
    EXPECT_NEAR(rect_of(key("Super+f"))[2], width, 1) << "and back";
    auto f = key("Super+Shift+f");
    EXPECT_TRUE(f.value("fullscreen", false));
    EXPECT_EQ(rect_of(f)[1], 0) << "over the status bar too";
    EXPECT_FALSE(key("Super+Shift+f").value("fullscreen", true));
    EXPECT_TRUE(key("Super+Shift+space").value("floating", false));
    EXPECT_FALSE(key("Super+Shift+space").value("floating", true));
    // Workspace 2: made by going to it; the window sent there; back to 1.
    (void)key("Super+2");
    auto current = [&] {
        for (const auto& w : s["workspaces"])
            if (w.value("current", false)) return w["id"].get<int>();
        return 0;
    };
    EXPECT_EQ(current(), 2);
    EXPECT_EQ(s["workspaces"].size(), 2u);
    (void)key("Super+1");
    EXPECT_EQ(current(), 1);
    (void)key("Super+Shift+2");
    EXPECT_FALSE(window_titled(s, kBlueTitle).contains("rect")) << "sent away: not in 1";
    (void)key("Super+2");
    EXPECT_EQ(window_titled(s, kBlueTitle).value("workspace", 0), 2) << "in 2";
    const auto run = d.finish();
    EXPECT_EQ(run.exit_code, 0) << run.transcript();
}

// The WuXian view's gestures (design 2026-10-20 WuXian §3): a workspace
// moved by its label and scaled by its corner; the background dragged pans;
// Super and the wheel zoom about the pointer.
XTEST(XdeskInteraction, InTheWuXianViewWorkspacesMoveAndScaleAndTheViewPansAndZooms, .area = "luban",
      .covers = {"WUXIAN-WORKSPACES", "WUXIAN-VIEWS", "XDESK-HEADLESS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("xdesk-wuxian");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    XdeskSession d(home, "classic");
    ASSERT_TRUE(d.ready());
    auto s = settled(d);
    ASSERT_TRUE(d.key("Super+o"));
    s = d.until([](const Json& j) { return j["view"] == "wuxian"; });
    s = d.still();
    auto frame = [&] {
        const auto& f = s["workspaces"][0]["frame"];
        return std::array<double, 4>{f[0].get<double>(), f[1].get<double>(), f[2].get<double>(), f[3].get<double>()};
    };
    auto f0 = frame();
    // Its label, above its top-left: dragged 50 right and 30 down, the frame with it.
    ASSERT_TRUE(d.input(press_drag(f0[0] + 12, f0[1] - 14, f0[0] + 62, f0[1] + 16)));
    s = d.still();
    auto f1 = frame();
    EXPECT_NEAR(f1[0] - f0[0], 50, 2);
    EXPECT_NEAR(f1[1] - f0[1], 30, 2);
    // Its corner: dragged inwards, smaller.
    ASSERT_TRUE(d.input(press_drag(f1[0] + f1[2] - 4, f1[1] + f1[3] - 4, f1[0] + f1[2] * 0.7, f1[1] + f1[3] * 0.7)));
    s = d.still();
    auto f2 = frame();
    EXPECT_LT(f2[2], f1[2] * 0.9) << "scaled by its corner";
    // The background (outside every workspace): dragged, the view pans.
    const auto cam = s["camera"];
    const double bx = f2[0] + f2[2] + 40, by = f2[1] + f2[3] + 40;
    ASSERT_TRUE(d.input(press_drag(bx, by, bx - 100, by - 50)));
    s = d.still();
    EXPECT_NEAR((s["camera"]["at"][0].get<double>() - cam["at"][0].get<double>()) * cam["zoom"].get<double>(), 100, 2);
    // Super and the wheel: zoomed about the pointer.
    const double z0 = s["camera"]["zoom"].get<double>();
    ASSERT_TRUE(d.input(Json::array({Json{{"move", {400, 300}}}, Json{{"keydown", "Super"}}, Json{{"wheel", {0, 1}}},
                                     Json{{"keyup", "Super"}}})));
    s = d.still();
    EXPECT_NEAR(s["camera"]["zoom"].get<double>(), z0 * 1.1, 1e-6);
    const auto run = d.finish();
    EXPECT_EQ(run.exit_code, 0) << run.transcript();
}
