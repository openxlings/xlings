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

XTEST(XdeskLayout, TheOverviewFitsEveryWindowAndLeavingItIsBackWhereItWas, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::canvas, .gap = 10});
    for (layout::Id id = 1; id <= 5; ++id) l.add(id, {800, 600});
    l.pan(-123, 45);
    const auto before = of(l.arrange(kOutput), 2).rect;
    l.overview(true);
    EXPECT_TRUE(l.in_overview());
    const auto ps = l.arrange(kOutput);                          // fitted as it is arranged
    EXPECT_LT(l.zoom(), 1.0);
    for (const auto& p : ps) {
        EXPECT_TRUE(p.visible);
        EXPECT_GE(p.rect.x, 0);
        EXPECT_LE(p.rect.x + p.rect.width, kOutput.width + 1e-6);
    }
    // Leaving it is back where it was, exactly.
    l.overview(false);
    EXPECT_EQ(of(l.arrange(kOutput), 2).rect, before);
    // Going in to a window from it: zoom 1, that window in the middle.
    l.overview(true);
    l.arrange(kOutput);
    l.center(3);
    EXPECT_FALSE(l.in_overview());
    EXPECT_EQ(l.zoom(), 1.0);
    EXPECT_EQ(l.focused(), 3u);
    const auto r = of(l.arrange(kOutput), 3).rect;
    EXPECT_NEAR(r.x + r.width / 2, kOutput.width / 2, 1e-6);
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

// ── space x arrangement (design 2026-10-19 §17) ──────────────────────

XTEST(XdeskLayout, AColumnHoldsWindowsOneAboveAnotherAndGivesThemBack, .area = "luban", .covers = {"XDESK-LAYOUT", "XDESK-ARRANGEMENTS"}) {
    Layout l({.mode = Mode::tile, .gap = 10});
    for (layout::Id id = 1; id <= 3; ++id) l.add(id, {600, 400});
    l.join();                                                   // Super+[: 3 into 2's column
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect, (Rect{605, 10, 585, 385}));       // two columns again, halves
    EXPECT_EQ(of(ps, 3).rect, (Rect{605, 405, 585, 385}));      // below it, an equal part
    EXPECT_EQ(l.group_of(3), (std::vector<layout::Id>{2, 3}));
    // Its height: 2/3, then 1/3, then an equal part again.
    l.cycle_height();
    EXPECT_NEAR(of(l.arrange(kOutput), 3).rect.height, 770.0 * 2 / 3, 1e-6);
    l.cycle_height();
    EXPECT_NEAR(of(l.arrange(kOutput), 3).rect.height, 770.0 / 3, 1e-6);
    l.cycle_height();
    EXPECT_NEAR(of(l.arrange(kOutput), 3).rect.height, 385, 1e-6);
    // Up and down: the focus, then the window, inside the column.
    l.move_focus(Direction::up);
    EXPECT_EQ(l.focused(), 2u);
    l.move_focus(Direction::down);
    l.move(Direction::up);
    ps = l.arrange(kOutput);
    EXPECT_LT(of(ps, 3).rect.y, of(ps, 2).rect.y);
    // Super+]: a column of its own, right of the one it left.
    l.split();
    ps = l.arrange(kOutput);
    EXPECT_EQ(l.group_of(2), (std::vector<layout::Id>{2}));
    EXPECT_GT(of(ps, 3).rect.x, of(ps, 2).rect.x);
    EXPECT_EQ(of(ps, 3).rect.height, 780);
    // Sharing a column, moving sideways leaves it for one on that side.
    l.join();
    l.move(Direction::left);
    ps = l.arrange(kOutput);
    EXPECT_LT(of(ps, 3).rect.x, of(ps, 2).rect.x);
    EXPECT_GT(of(ps, 3).rect.x, of(ps, 1).rect.x);
    EXPECT_EQ(of(ps, 2).rect.height, 780);
}

XTEST(XdeskLayout, SwitchingArrangementsComesBackToTheSameLayout, .area = "luban", .covers = {"XDESK-LAYOUT", "XDESK-ARRANGEMENTS"}) {
    Layout l({.mode = Mode::tile, .gap = 10});
    for (layout::Id id = 1; id <= 3; ++id) l.add(id, {600, 400});
    l.join();
    const auto tiled = l.arrange(kOutput);
    // The canvas for the first time: the columns become one group, where they were.
    l.set_mode(Mode::canvas);
    EXPECT_EQ(l.mode(), Mode::canvas);
    auto ps = l.arrange(kOutput);
    for (const auto& t : tiled) {
        EXPECT_NEAR(of(ps, t.id).rect.x, t.rect.x, 1e-6) << t.id;
        EXPECT_NEAR(of(ps, t.id).rect.y, t.rect.y, 1e-6) << t.id;
        EXPECT_NEAR(of(ps, t.id).rect.width, t.rect.width, 1e-6) << t.id;
        EXPECT_NEAR(of(ps, t.id).rect.height, t.rect.height, 1e-6) << t.id;
    }
    EXPECT_EQ(l.group_of(1), (std::vector<layout::Id>{1, 2, 3}));
    l.pan(-200, 0);
    const auto panned = of(l.arrange(kOutput), 1).rect;
    // Back to tile: exactly as it was; to the canvas again: as it was left.
    l.set_mode(Mode::tile);
    EXPECT_EQ(l.arrange(kOutput), tiled);
    l.set_mode(Mode::canvas);
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, panned);
    // Stack for the first time: each where it is seen, on the output.
    l.set_mode(Mode::stack);
    ps = l.arrange(kOutput);
    for (const auto& p : ps) {
        EXPECT_GE(p.rect.x, 0) << p.id;
        EXPECT_LE(p.rect.x + p.rect.width, kOutput.width) << p.id;
    }
    EXPECT_EQ(of(ps, 2).rect.y, panned.y);
    l.set_mode(Mode::tile);
    EXPECT_EQ(l.arrange(kOutput), tiled);
    // Super+m's order, and the names a state file keeps.
    EXPECT_EQ(layout::next(Mode::tile), Mode::stack);
    EXPECT_EQ(layout::next(Mode::stack), Mode::canvas);
    EXPECT_EQ(layout::next(Mode::canvas), Mode::tile);
    EXPECT_EQ(layout::mode_named(layout::name(Mode::stack)), Mode::stack);
    EXPECT_FALSE(layout::mode_named("cascade"));
}

XTEST(XdeskLayout, StackedWindowsOverlapOnTheOutputAndSnapToHalvesAndQuarters, .area = "luban", .covers = {"XDESK-ARRANGEMENTS"}) {
    Layout l({.mode = Mode::stack, .gap = 10});
    l.add(1, {600, 400});
    l.add(2, {600, 400});
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect, (Rect{300, 200, 600, 400}));      // the middle
    EXPECT_EQ(of(ps, 2).rect, (Rect{332, 232, 600, 400}));      // the next one down and right, so both show
    EXPECT_EQ(ps.back().id, 2u);                                // the newest above
    EXPECT_EQ(l.at(400, 300), 2u);
    l.focus(1);
    EXPECT_EQ(l.arrange(kOutput).back().id, 1u);                // focus raises
    // Super+Shift+arrows: the left half, its top quarter, the top half.
    l.move(Direction::left);
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{10, 10, 585, 780}));
    l.move(Direction::up);
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{10, 10, 585, 385}));
    l.move(Direction::right);
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{10, 10, 1180, 385}));
    // Super+f: the whole output, and back where it was before it snapped.
    l.toggle_maximized(1);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect, (Rect{10, 10, 1180, 780}));
    EXPECT_TRUE(of(ps, 1).maximized);
    l.toggle_maximized(1);
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{300, 200, 600, 400}));
    // Put off the output, it is kept on it.
    l.place(1, {-100, 700, 600, 400});
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{0, 400, 600, 400}));
    // A floating window is above the stack too.
    l.add(3, {300, 200}, true);
    ps = l.arrange(kOutput);
    EXPECT_EQ(ps.back().id, 3u);
    EXPECT_TRUE(ps.back().floating);
}

XTEST(XdeskLayout, OnTheCanvasWindowsMakeTiledGroupsThatMoveAsOne, .area = "luban", .covers = {"XDESK-ARRANGEMENTS"}) {
    Layout l({.mode = Mode::canvas, .gap = 10});
    l.add(1, {600, 400});
    l.add(2, {500, 300});
    l.arrange(kOutput);
    // 2 put at 1's right edge: a group, columns side by side, one height.
    l.group_with(2, 1, Direction::right);
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(l.group_of(1), (std::vector<layout::Id>{1, 2}));
    EXPECT_NEAR(of(ps, 2).rect.x - (of(ps, 1).rect.x + of(ps, 1).rect.width), 10, 1e-6);
    EXPECT_EQ(of(ps, 2).rect.y, of(ps, 1).rect.y);
    EXPECT_EQ(of(ps, 2).rect.height, 400);
    EXPECT_EQ(of(ps, 2).rect.width, 500);
    // Moving one moves the group.
    const auto a = of(ps, 1).rect, b = of(ps, 2).rect;
    l.place(1, {a.x + 100, a.y + 50, a.width, a.height});
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect, (Rect{b.x + 100, b.y + 50, b.width, b.height}));
    // A third, on its own beside them; Super+[ joins it to the group on its left.
    l.add(3, {400, 300});
    l.arrange(kOutput);
    EXPECT_EQ(l.group_of(3), (std::vector<layout::Id>{3}));
    l.join();
    EXPECT_EQ(l.group_of(3), (std::vector<layout::Id>{1, 2, 3}));
    // Super+]: free again, beside the group.
    l.split();
    ps = l.arrange(kOutput);
    EXPECT_EQ(l.group_of(1), (std::vector<layout::Id>{1, 2}));
    EXPECT_GT(of(ps, 3).rect.x, of(ps, 2).rect.x + of(ps, 2).rect.width);
    // Put at 1's bottom edge: in 1's column, below it.
    l.group_with(3, 1, Direction::down);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 3).rect.x, of(ps, 1).rect.x);
    EXPECT_NEAR(of(ps, 1).rect.height + 10 + of(ps, 3).rect.height, 400, 1e-6);
    // Tiled for the first time: the group's columns are columns.
    l.set_mode(Mode::tile);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 3).rect.x, of(ps, 1).rect.x);
    EXPECT_LT(of(ps, 1).rect.x, of(ps, 2).rect.x);
}

XTEST(XdeskLayout, EachWorkspaceKeepsItsOwnArrangement, .area = "luban", .covers = {"XDESK-ARRANGEMENTS"}) {
    Layout l({.mode = Mode::tile, .gap = 10, .workspaces = 2});
    l.add(1, {600, 400});
    l.arrange(kOutput);
    l.set_mode(Mode::stack);
    l.show(1);
    EXPECT_EQ(l.mode(), Mode::tile);                            // the default until it is set
    l.add(2, {600, 400});
    EXPECT_EQ(of(l.arrange(kOutput), 2).rect.width, 1180);
    l.show(0);
    EXPECT_EQ(l.mode(), Mode::stack);
    EXPECT_EQ(l.mode_of(1), Mode::tile);
    // Sent to a stacked workspace, a window gets a place there.
    l.send_to(2, 0);
    const auto ps = l.arrange(kOutput);
    EXPECT_EQ(ps.size(), 2u);
    EXPECT_LE(of(ps, 2).rect.width, 1200);
}
