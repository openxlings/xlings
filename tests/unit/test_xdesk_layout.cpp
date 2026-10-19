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

bool overlap_(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

const layout::Placement& of(const std::vector<layout::Placement>& ps, layout::Id id) {
    for (const auto& p : ps)
        if (p.id == id) return p;
    throw std::runtime_error(std::format("no placement for {}", id));
}

}  // namespace

XTEST(XdeskLayout, ColumnsShareTheOutputUntilTooNarrowThenScrollWithTheFocus, .area = "luban", .covers = {"XDESK-LAYOUT", "XDESK-TILE-SIZES"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10, .min_column_width = 300});
    l.add(1, {600, 400});
    auto ps = l.arrange(kOutput);
    ASSERT_EQ(ps.size(), 1u);
    EXPECT_EQ(ps[0].rect, (Rect{10, 10, 1180, 780}));          // all of it, inside the gaps
    EXPECT_TRUE(ps[0].tiled) << "the client is told it is tiled (it draws to the size given)";
    l.add(2, {600, 400});
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect, (Rect{10, 10, 585, 780}));       // halves, side by side
    EXPECT_EQ(of(ps, 2).rect, (Rect{605, 10, 585, 780}));
    EXPECT_TRUE(of(ps, 2).focused);                            // the new one has the focus
    // A third: thirds -- every window on the screen (G5: one was off it).
    l.add(3, {600, 400});
    ps = l.arrange(kOutput);
    for (const auto id : {1u, 2u, 3u}) {
        EXPECT_NEAR(of(ps, id).rect.width, (1180 - 20) / 3.0, 1e-6) << id;
        EXPECT_TRUE(of(ps, id).visible) << id;
    }
    // A fourth would be narrower than 300: each is 300 and the strip scrolls,
    // the focused one shown whole.
    l.add(4, {600, 400});
    ps = l.arrange(kOutput);
    EXPECT_NEAR(of(ps, 4).rect.width, 300, 1e-6);
    EXPECT_GE(of(ps, 4).rect.x, 0);
    EXPECT_LE(of(ps, 4).rect.x + of(ps, 4).rect.width, kOutput.width);
    l.remove(4);
    // Back to the first: it is shown again.
    l.move_focus(Direction::left);
    l.move_focus(Direction::left);
    EXPECT_EQ(l.focused(), 1u);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect.x, 10);
    // A new window opens beside the focused one, not at the end.
    l.add(4, {600, 400});
    ps = l.arrange(kOutput);
    EXPECT_LT(of(ps, 4).rect.x, of(ps, 2).rect.x);
    // Closing the focused one gives the focus to the column now in its place.
    l.remove(4);
    EXPECT_EQ(l.focused(), 2u);
}

XTEST(XdeskLayout, ColumnsGivenWidthsFillTheOutputInProportionAndScrollPastIt, .area = "luban",
      .covers = {"XDESK-LAYOUT", "XDESK-TILE-SIZES"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 4, .outer_gap = 0});
    l.add(1, {600, 400});
    l.cycle_width();                                            // one column fills whatever its width says
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(ps[0].rect, (Rect{0, 0, 1200, 800}));
    l.add(2, {600, 400});
    l.move(Direction::left);
    ps = l.arrange(kOutput);
    EXPECT_LT(of(ps, 2).rect.x, of(ps, 1).rect.x);              // columns swap
    // A third and a third, less than the screen: still filled, in
    // proportion (G5: blanks at the sides).
    l.focus(2);
    l.cycle_width();                                            // 2/3 (what it has) -> the whole
    l.cycle_width();                                            // -> 1/3
    ps = l.arrange(kOutput);
    EXPECT_NEAR(of(ps, 2).rect.width + of(ps, 1).rect.width + 4, 1200, 1e-6);
    EXPECT_NEAR(of(ps, 2).rect.width, of(ps, 1).rect.width, 1e-6);
    EXPECT_EQ(of(ps, 2).rect.x, 0);
    // Wider than the screen in all: it scrolls, the focused column in view.
    l.focus(1);
    l.cycle_width();                                            // 1/2 as shown -> 2/3
    l.cycle_width();                                            // -> the whole
    ps = l.arrange(kOutput);
    EXPECT_NEAR(of(ps, 1).rect.width, 1200, 1e-6);
    ps = l.arrange(kOutput);
    EXPECT_GE(of(ps, 1).rect.x, 0);
    EXPECT_LE(of(ps, 1).rect.x + of(ps, 1).rect.width, kOutput.width + 1e-6);
    // The gaps: 4 between windows, none at the edges.
    l.even_widths();
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect.x, 0);
    EXPECT_NEAR(of(ps, 1).rect.x - (of(ps, 2).rect.x + of(ps, 2).rect.width), 4, 1e-6);
    EXPECT_EQ(of(ps, 1).rect.height, 800);
}

XTEST(XdeskLayout, AVerticalFlowStacksColumnsAsRowsAndScrollsDown, .area = "luban", .covers = {"XDESK-TILE-SIZES"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 4, .outer_gap = 0, .min_column_width = 300, .flow = layout::Flow::vertical});
    constexpr layout::Size portrait { 800, 1200 };
    for (layout::Id id = 1; id <= 3; ++id) l.add(id, {600, 400});
    auto ps = l.arrange(portrait);
    for (const auto id : {1u, 2u, 3u}) {
        EXPECT_EQ(of(ps, id).rect.width, 800) << id;
        EXPECT_NEAR(of(ps, id).rect.height, (1200 - 8) / 3.0, 1e-6) << id;
    }
    EXPECT_LT(of(ps, 1).rect.y, of(ps, 2).rect.y);
    // Directions turn with it: down is the next row.
    l.focus(1);
    l.move_focus(Direction::down);
    EXPECT_EQ(l.focused(), 2u);
    l.add(4, {600, 400});
    l.add(5, {600, 400});
    ps = l.arrange(portrait);
    EXPECT_NEAR(of(ps, 5).rect.height, 300, 1e-6);              // five would be too short: they scroll
    EXPECT_LE(of(ps, 5).rect.y + of(ps, 5).rect.height, 1200 + 1e-6);
    EXPECT_EQ(l.flow(), layout::Flow::vertical);
}

XTEST(XdeskLayout, AFloatingWindowIsCentredAboveAndStaysWhereItIsPut, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.mode = Mode::canvas, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.mode = Mode::canvas, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10, .workspaces = 3});
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
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.mode = Mode::stack, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.mode = Mode::canvas, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10, .workspaces = 2});
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

XTEST(XdeskLayout, AlwaysOnTopIsAboveEverythingAndOnEveryWorkspaceGoesAlong, .area = "luban", .covers = {"XDESK-WINDOW-MENU"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10, .workspaces = 2});
    l.add(1, {600, 400});
    l.add(2, {400, 300}, true);
    l.add(3, {300, 200});
    l.arrange(kOutput);
    // Always on top: floating, drawn last -- above another floating window.
    l.toggle_pinned(3);
    EXPECT_TRUE(l.pinned(3));
    EXPECT_TRUE(l.floating(3));
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(ps.back().id, 3u);
    EXPECT_TRUE(ps.back().pinned);
    // On every workspace: shown on the other one too, where it was.
    l.toggle_sticky(2);
    const auto where = of(l.arrange(kOutput), 2).rect;
    l.show(1);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect, where);
    EXPECT_EQ(l.workspace_of(2), 1);
    EXPECT_FALSE(std::ranges::contains(ps, layout::Id{1}, &layout::Placement::id));
    // Back to one workspace: the one shown.
    l.toggle_sticky(2);
    l.show(0);
    EXPECT_FALSE(std::ranges::contains(l.arrange(kOutput), layout::Id{2}, &layout::Placement::id));
    // A session's arrangement given to an empty workspace before anything is on it.
    l.preset(1, Mode::canvas);
    EXPECT_EQ(l.mode_of(1), Mode::tile) << "a workspace with windows keeps its own";
    Layout fresh({.mode = Mode::tile, .workspaces = 2});
    fresh.preset(1, Mode::canvas);
    EXPECT_EQ(fresh.mode_of(1), Mode::canvas);
}

// Dragging (design 2026-10-19 §17.9): where it lands is read off the
// arrangement, and the preview is that very drop.
XTEST(XdeskLayout, ADraggedWindowsPreviewIsWhereItLands, .area = "luban", .covers = {"XDESK-DRAG-SNAP"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10});
    for (layout::Id id = 1; id <= 2; ++id) l.add(id, {600, 400});
    auto ps = l.arrange(kOutput);
    const auto two = of(ps, 2).rect;
    // Window 1 dragged over the top third of 2: into 2's column, above it.
    auto d = l.drop_at(1, two.x + two.width / 2, two.y + 30);
    EXPECT_EQ(d.kind, layout::Drop::Kind::above);
    const auto seen = l.preview(1, d);
    ASSERT_TRUE(seen);
    l.drop(1, d);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect, *seen) << "the preview is where it went";
    EXPECT_EQ(l.group_of(1), (std::vector<layout::Id>{1, 2}));
    // At the screen's left edge: a column of its own, first; with a size chosen.
    d = l.drop_at(1, 5, 400);
    EXPECT_EQ(d.kind, layout::Drop::Kind::column);
    EXPECT_EQ(d.column, 0u);
    const auto narrow = l.preview(1, d, 1.0 / 3);
    l.drop(1, d, 1.0 / 3);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect, *narrow);
    EXPECT_LT(of(ps, 1).rect.x, of(ps, 2).rect.x);
    EXPECT_NEAR(of(ps, 1).rect.width, (1200 - 10) / 3.0 - 10, 1e-6);
    // The middle of another: they change places.
    const auto mid = of(ps, 2).rect;
    d = l.drop_at(1, mid.x + mid.width / 2, mid.y + mid.height / 2);
    EXPECT_EQ(d.kind, layout::Drop::Kind::swap);
    l.drop(1, d);
    ps = l.arrange(kOutput);
    EXPECT_GT(of(ps, 1).rect.x, of(ps, 2).rect.x);
    // The gap between them: dragged, one wider and the other narrower; twice, equal again.
    const auto gap = l.gap_at(of(ps, 2).rect.x + of(ps, 2).rect.width + 5, 300);
    ASSERT_TRUE(gap);
    const double before = of(ps, 2).rect.width;
    l.drag_gap(*gap, 100);
    ps = l.arrange(kOutput);
    EXPECT_NEAR(of(ps, 2).rect.width, before + 100, 1.5);
    l.even_widths();
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect.width, of(ps, 2).rect.width);
    // Shift: floating.
    EXPECT_EQ(l.drop_at(1, 600, 400, true).kind, layout::Drop::Kind::floating);
}

XTEST(XdeskLayout, StackedEdgesSnapAndCanvasEdgesGroup, .area = "luban", .covers = {"XDESK-DRAG-SNAP"}) {
    Layout s({.mode = Mode::stack, .inner_gap = 10, .outer_gap = 10});
    s.add(1, {600, 400});
    s.arrange(kOutput);
    auto d = s.drop_at(1, 3, 400);
    EXPECT_EQ(d.kind, layout::Drop::Kind::snap);
    EXPECT_EQ(d.snap_x, -1);
    EXPECT_EQ(*s.preview(1, d), (Rect{10, 10, 585, 780})) << "the left half";
    d = s.drop_at(1, 1197, 3);
    EXPECT_EQ(std::pair(d.snap_x, d.snap_y), std::pair(1, -1)) << "a corner: its quarter";
    EXPECT_EQ(s.drop_at(1, 600, 3).snap_x, 0) << "the top: the whole";
    EXPECT_EQ(s.drop_at(1, 600, 400).kind, layout::Drop::Kind::free);

    Layout c({.mode = Mode::canvas, .inner_gap = 10, .outer_gap = 10});
    c.add(1, {600, 400});
    c.add(2, {500, 300});
    auto ps = c.arrange(kOutput);
    const auto r1 = of(ps, 1).rect;
    d = c.drop_at(2, r1.x + r1.width - 10, r1.y + r1.height / 2);
    EXPECT_EQ(d.kind, layout::Drop::Kind::right);
    const auto seen = c.preview(2, d);
    c.drop(2, d);
    EXPECT_EQ(of(c.arrange(kOutput), 2).rect, *seen);
    EXPECT_EQ(c.group_of(1), (std::vector<layout::Id>{1, 2}));
    // Let go in the open: out of the group.
    c.drop(2, {.kind = layout::Drop::Kind::free});
    EXPECT_EQ(c.group_of(1), (std::vector<layout::Id>{1}));
}

XTEST(XdeskLayout, TheMaximizeButtonsLayoutsLayOutTheRecentWindows, .area = "luban", .covers = {"XDESK-DRAG-SNAP"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 10, .outer_gap = 10});
    for (layout::Id id = 1; id <= 3; ++id) l.add(id, {600, 400});
    l.arrange(kOutput);
    l.arrange_as(layout::Pattern::one_and_two, {3, 2, 1});
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(l.group_of(2), (std::vector<layout::Id>{2, 1}));
    EXPECT_LT(of(ps, 3).rect.x, of(ps, 2).rect.x);
    l.arrange_as(layout::Pattern::two_thirds, {3, 2});
    ps = l.arrange(kOutput);
    EXPECT_GT(of(ps, 3).rect.width, of(ps, 2).rect.width);
    Layout s({.mode = Mode::stack, .inner_gap = 10, .outer_gap = 10});
    s.add(1, {600, 400});
    s.add(2, {600, 400});
    s.arrange(kOutput);
    s.arrange_as(layout::Pattern::halves, {2, 1});
    ps = s.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect.x, 10);
    EXPECT_GT(of(ps, 1).rect.x, 590);
}

// Dragging in tile (design 2026-10-20 §7.5): the zones of a window, the
// others where the drop leaves them, and a hand's scroll kept.
XTEST(XdeskLayout, ADragsZonesAreQuartersAndThirdsAndThePreviewIsEveryWindow, .area = "luban", .covers = {"XDESK-DRAG-SNAP"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 4, .outer_gap = 0});
    l.add(1, {600, 400});
    l.add(2, {600, 400});
    auto ps = l.arrange(kOutput);
    const auto g = of(ps, 2).rect;   // the right half
    using K = layout::Drop::Kind;
    auto at = [&](double fx, double fy) { return l.drop_at(1, g.x + fx * g.width, g.y + fy * g.height).kind; };
    EXPECT_EQ(at(0.5, 0.1), K::above);
    EXPECT_EQ(at(0.5, 0.9), K::below);
    EXPECT_EQ(at(0.5, 0.5), K::swap);
    EXPECT_EQ(at(0.2, 0.5), K::column);
    EXPECT_EQ(at(0.8, 0.5), K::column);
    // The preview is every window: green below red's place, the whole width.
    const auto all = l.preview_all(1, l.drop_at(1, g.x + g.width / 2, g.y + 10), 0);
    EXPECT_EQ(of(all, 2).rect.width, 1200);
    EXPECT_GT(of(all, 2).rect.y, of(all, 1).rect.y);
    EXPECT_EQ(of(l.arrange(kOutput), 2).rect, g) << "a preview changes nothing";
}

XTEST(XdeskLayout, TheGapBetweenTwoWindowsOfAColumnResizesThem, .area = "luban", .covers = {"XDESK-RESIZE"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 4, .outer_gap = 0});
    l.add(1, {600, 400});
    l.add(2, {600, 400});
    l.join();                                   // 2 under 1, one column
    auto ps = l.arrange(kOutput);
    const auto top = of(ps, 1).rect;
    const auto gap = l.row_gap_at(top.x + 100, top.y + top.height + 2);
    ASSERT_TRUE(gap);
    l.drag_row_gap(gap->first, gap->second, 100);
    ps = l.arrange(kOutput);
    EXPECT_NEAR(of(ps, 1).rect.height, top.height + 100, 1);
    EXPECT_NEAR(of(ps, 1).rect.height + of(ps, 2).rect.height + 4, 800, 1e-6);
}

XTEST(XdeskLayout, ATiledStripScrolledByHandStaysUntilTheFocusMoves, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.mode = Mode::tile, .inner_gap = 0, .outer_gap = 0, .min_column_width = 600});
    for (layout::Id id = 1; id <= 3; ++id) l.add(id, {600, 400});
    auto ps = l.arrange(kOutput);
    ASSERT_FALSE(of(ps, 1).visible);            // the focused third shown, the first off the screen
    l.pan(600, 0);
    ps = l.arrange(kOutput);
    EXPECT_TRUE(of(ps, 1).visible) << "a hand's scroll is kept";
    l.focus(3);
    ps = l.arrange(kOutput);
    EXPECT_TRUE(of(ps, 3).visible);
}

// The canvas (design 2026-10-20 §9): a new window in the free place nearest
// the focused one, in view; a moved frame snaps to an edge; the minimap.
XTEST(XdeskLayout, OnTheCanvasANewWindowTakesTheNearestFreePlaceAndFramesSnap, .area = "luban", .covers = {"XDESK-CANVAS"}) {
    Layout l({.mode = Mode::canvas, .inner_gap = 4, .outer_gap = 0});
    l.add(1, {400, 300});
    auto ps = l.arrange(kOutput);
    const auto first = of(ps, 1).rect;
    l.add(2, {400, 300});
    ps = l.arrange(kOutput);
    const auto second = of(ps, 2).rect, now = of(ps, 1).rect;
    (void)first;
    EXPECT_FALSE(overlap_(now, second));
    EXPECT_GE(second.x, 0);
    EXPECT_LE(second.x + second.width, kOutput.width) << "in view (the view panned to it)";
    EXPECT_NEAR(second.x, now.x + now.width + 12, 1e-6) << "beside the focused one";
    EXPECT_EQ(l.zoom(), 1.0) << "a new window never zooms the view";
    // A third: beside isn't in view any more -- below, or the other side.
    l.add(3, {400, 300});
    ps = l.arrange(kOutput);
    for (const auto id : {1u, 2u}) EXPECT_FALSE(overlap_(of(ps, 3).rect, of(ps, id).rect)) << id;
    // A frame moved to within 16 pixels of another's edge sits against it
    // (where the window is now: the view may have panned for the third).
    const auto two = of(ps, 2).rect;
    const auto sn = l.snap(3, {two.x + 7, two.y + two.height + 10, 400, 300});
    EXPECT_EQ(sn.rect.x, two.x);
    EXPECT_NEAR(sn.rect.y, two.y + two.height + 4, 1e-6);
    EXPECT_TRUE(sn.guide_x && sn.guide_y);
    const auto far = l.snap(3, {two.x + 40, two.y + two.height + 60, 400, 300});
    EXPECT_FALSE(far.guide_x || far.guide_y) << "out of reach: where it was put";
    const auto m = l.minimap();
    EXPECT_EQ(m.boxes.size(), 3u);
    l.look_at(5000, 5000);
    EXPECT_NEAR(l.camera().first, 5000 - kOutput.width / 2, 1e-6);
}

// The overview's picture (design 2026-10-20 §10.3): windows where they are
// to one another, fitted and never enlarged; a long strip folded into rows.
XTEST(XdeskLayout, TheOverviewKeepsWhereWindowsAreAndFoldsALongStrip, .area = "luban", .covers = {"XDESK-SWITCHERS"}) {
    using xdesk::layout::Rect;
    const Rect area{0, 0, 1000, 600};
    // Two that fit: as they are, centred.
    const auto small = xdesk::layout::spread({{0, 0, 200, 100}, {300, 0, 200, 100}}, area);
    ASSERT_EQ(small.size(), 2u);
    EXPECT_EQ(small[0].width, 200) << "never enlarged";
    EXPECT_NEAR(small[0].x, 250, 1e-9);
    EXPECT_NEAR(small[1].x - small[0].x, 300, 1e-9) << "where they are to one another";
    // A window off to the right comes in: everything scaled to fit.
    const auto off = xdesk::layout::spread({{0, 0, 1000, 600}, {1200, 0, 800, 600}}, area);
    EXPECT_LE(off[1].x + off[1].width, area.width + 1e-9);
    EXPECT_NEAR(off[0].width / 1000, off[1].width / 800, 1e-9) << "one scale for all";
    EXPECT_LT(off[0].x + off[0].width, off[1].x) << "the gap between them kept";
    // Eight full-height columns, eight screens wide: folded into rows, each
    // window bigger than the strip scaled as one row would make it.
    std::vector<Rect> strip;
    for (int k = 0; k < 8; ++k) strip.push_back({k * 1000.0, 0, 1000, 600});
    const auto folded = xdesk::layout::spread(strip, area);
    ASSERT_EQ(folded.size(), 8u);
    EXPECT_GT(folded[0].width, 1000.0 * area.width / 8000 * 1.5);
    EXPECT_GT(folded[7].y, folded[0].y) << "a later column on a lower row";
    for (const auto& r : folded) {
        EXPECT_GE(r.x, -1e-9);
        EXPECT_LE(r.y + r.height, area.height + 1e-9);
    }
    for (std::size_t a = 0; a < folded.size(); ++a)
        for (std::size_t b = 0; b < a; ++b) EXPECT_FALSE(overlap_(folded[a], folded[b])) << a << " " << b;
    EXPECT_TRUE(xdesk::layout::spread({}, area).empty());
}
