// xdesk.layout (Luban OS desktop part 2 §2.3): where windows go, in tile and
// on the canvas, floating, on workspaces -- and the spring that moves them.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xdesk.layout;

namespace layout = xdesk::layout;
using layout::Direction;
using layout::Layout;
using layout::Mode;
using layout::Rect;

namespace {

constexpr layout::Size kOutput { 1200, 800 };

const layout::Placement& of(const std::vector<layout::Placement>& ps, layout::Id id) {
    for (const auto& p : ps)
        if (p.id == id) return p;
    throw std::runtime_error(std::format("no placement for {}", id));
}

}  // namespace

XTEST(XdeskLayout, OneOrTwoWindowsShareTheOutputMoreScrollWithTheFocus, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::tile, .gap = 10});
    l.add(1, {600, 400});
    auto ps = l.arrange(kOutput);
    ASSERT_EQ(ps.size(), 1u);
    EXPECT_EQ(ps[0].rect, (Rect{10, 10, 1180, 780}));          // all of it, inside the gaps
    l.add(2, {600, 400});
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect, (Rect{10, 10, 585, 780}));       // halves, side by side
    EXPECT_EQ(of(ps, 2).rect, (Rect{605, 10, 585, 780}));
    EXPECT_TRUE(of(ps, 2).focused);                            // the new one has the focus
    // A third: halves still, the strip scrolled so the focused one shows whole.
    l.add(3, {600, 400});
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 3).rect.width, 585);
    EXPECT_GE(of(ps, 3).rect.x, 0);
    EXPECT_LE(of(ps, 3).rect.x + of(ps, 3).rect.width, kOutput.width);
    EXPECT_FALSE(of(ps, 1).visible);
    // Back to the first: it is shown again.
    l.move_focus(Direction::left);
    l.move_focus(Direction::left);
    EXPECT_EQ(l.focused(), 1u);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect.x, 10);
    EXPECT_FALSE(of(ps, 3).visible);
    // A new window opens beside the focused one, not at the end.
    l.add(4, {600, 400});
    ps = l.arrange(kOutput);
    EXPECT_LT(of(ps, 4).rect.x, of(ps, 2).rect.x);
    // Closing the focused one gives the focus to the column now in its place.
    l.remove(4);
    EXPECT_EQ(l.focused(), 2u);
}

XTEST(XdeskLayout, AColumnsWidthCyclesAndColumnsSwap, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::tile, .gap = 10});
    l.add(1, {600, 400});
    l.cycle_width();                                            // the whole -> a third
    auto ps = l.arrange(kOutput);
    EXPECT_NEAR(ps[0].rect.width, (1200 - 10) / 3.0 - 10, 1e-6);
    EXPECT_NEAR(ps[0].rect.x, (1200 - ps[0].rect.width) / 2, 1e-6);   // a narrow strip is centred
    l.add(2, {600, 400});
    l.move(Direction::left);
    ps = l.arrange(kOutput);
    EXPECT_LT(of(ps, 2).rect.x, of(ps, 1).rect.x);
}

XTEST(XdeskLayout, AFloatingWindowIsCentredAboveAndStaysWhereItIsPut, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::tile, .gap = 10});
    l.add(1, {600, 400});
    l.add(2, {400, 300}, true);
    auto ps = l.arrange(kOutput);
    ASSERT_EQ(ps.size(), 2u);
    EXPECT_EQ(ps.back().id, 2u);                                // drawn last: above
    EXPECT_TRUE(ps.back().floating);
    EXPECT_EQ(ps.back().rect, (Rect{400, 250, 400, 300}));
    EXPECT_EQ(of(ps, 1).rect.width, 1180);                      // the tiles do not count it
    EXPECT_EQ(l.at(500, 300), 2u);                              // the one above is hit first
    EXPECT_EQ(l.at(50, 300), 1u);
    l.place(2, {20, 30, 400, 300});
    EXPECT_EQ(of(l.arrange(kOutput), 2).rect, (Rect{20, 30, 400, 300}));
    // Tiled again, then floating again.
    l.toggle_floating(2);
    EXPECT_FALSE(l.floating(2));
    EXPECT_EQ(l.arrange(kOutput).size(), 2u);
    EXPECT_EQ(of(l.arrange(kOutput), 2).rect.width, 585);
}

XTEST(XdeskLayout, TheCanvasPlacesBesideAndPansAndZoomsAroundAPoint, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::canvas, .gap = 10});
    l.add(1, {600, 400});
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(ps[0].rect, (Rect{300, 200, 600, 400}));          // the middle of the view
    l.add(2, {500, 300});
    ps = l.arrange(kOutput);
    EXPECT_NEAR(of(ps, 2).rect.x - (of(ps, 1).rect.x + 600), 30, 1e-6);   // right of it, a gap between
    EXPECT_EQ(of(ps, 2).rect.y, of(ps, 1).rect.y);
    EXPECT_LE(of(ps, 2).rect.x + 500, kOutput.width);           // panned so it shows whole
    // Panning moves everything by the same pixels.
    const auto before = of(l.arrange(kOutput), 1).rect;
    l.pan(-100, 50);
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{before.x - 100, before.y + 50, 600, 400}));
    // Zooming out around a point keeps that point still.
    const auto r1 = of(l.arrange(kOutput), 1).rect;
    l.zoom_at(0.5, r1.x, r1.y);
    const auto r2 = of(l.arrange(kOutput), 1).rect;
    EXPECT_NEAR(r2.x, r1.x, 1e-6);
    EXPECT_NEAR(r2.y, r1.y, 1e-6);
    EXPECT_NEAR(r2.width, 300, 1e-6);
    l.zoom_at(10, 0, 0);
    EXPECT_EQ(l.zoom(), 1.0);                                   // never past 1
    // Focus to the left finds window 1.
    l.focus(2);
    l.move_focus(Direction::left);
    EXPECT_EQ(l.focused(), 1u);
}

XTEST(XdeskLayout, TheOverviewFitsEveryWindowAndLeavingItReturnsToTheFocus, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::canvas, .gap = 10});
    for (layout::Id id = 1; id <= 5; ++id) l.add(id, {800, 600});
    l.overview(true);
    EXPECT_TRUE(l.in_overview());
    const auto ps = l.arrange(kOutput);                          // fitted as it is arranged
    EXPECT_LT(l.zoom(), 1.0);
    for (const auto& p : ps) {
        EXPECT_TRUE(p.visible);
        EXPECT_GE(p.rect.x, 0);
        EXPECT_LE(p.rect.x + p.rect.width, kOutput.width + 1e-6);
    }
    l.focus(3);
    l.overview(false);
    EXPECT_EQ(l.zoom(), 1.0);
    const auto r = of(l.arrange(kOutput), 3).rect;
    EXPECT_NEAR(r.x + r.width / 2, kOutput.width / 2, 1e-6);   // centred on the one chosen
}

XTEST(XdeskLayout, EachWorkspaceIsALayoutOfItsOwn, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::tile, .gap = 10, .workspaces = 3});
    l.add(1, {600, 400});
    l.add(2, {600, 400});
    l.send_to(2, 1);
    EXPECT_EQ(l.workspace_of(2), 1);
    EXPECT_EQ(l.arrange(kOutput).size(), 1u);
    EXPECT_EQ(l.focused(), 1u);
    l.show(1);
    auto ps = l.arrange(kOutput);
    ASSERT_EQ(ps.size(), 1u);
    EXPECT_EQ(ps[0].id, 2u);
    EXPECT_EQ(ps[0].rect.width, 1180);
    l.show(7);                                                  // no such workspace: nothing changes
    EXPECT_EQ(l.workspace(), 1);
}

XTEST(XdeskLayout, ASpringArrivesWithoutOvershootingAndBendsToANewTarget, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    layout::Spring s { .value = 0, .target = 100 };
    double last = 0;
    int steps = 0;
    while (!s.settled() && steps < 200) {
        s.step(1.0 / 60);
        EXPECT_LE(s.value, 100 + 1e-9);                         // never past the target
        EXPECT_GE(s.value, last - 1e-9);                        // never back
        last = s.value;
        ++steps;
    }
    EXPECT_EQ(s.value, 100);
    EXPECT_LT(steps, 40);                                        // well under a second at 60 Hz
    // A new target mid-way: it turns, it does not jump.
    s = {.value = 0, .target = 100};
    for (int i = 0; i < 5; ++i) s.step(1.0 / 60);
    const double mid = s.value;
    s.target = 0;
    s.step(1.0 / 60);
    EXPECT_LT(std::abs(s.value - mid), 20);
}
