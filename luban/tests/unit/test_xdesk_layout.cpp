// xdesk.layout (design 2026-10-20 WuXian §3): where windows go -- tiled and
// stacked workspaces on one WuXian, free windows, the workspace view and the
// WuXian view, dragging on a frozen arrangement, the state -- and the spring
// that moves them. openwx's conformance cases run against it.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import openwx;
import openwx.conformance;
import xdesk.layout;

namespace layout = xdesk::layout;
namespace wx = openwx;
using layout::Direction;
using layout::Layout;
using layout::Rect;
using layout::Rule;
using layout::View;
using K = layout::Drop::Kind;

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

bool shown_(const std::vector<layout::Placement>& ps, layout::Id id) { return std::ranges::contains(ps, id, &layout::Placement::id); }

layout::Match match_(layout::Id id) { return {.app_id = std::format("app{}", id), .title = std::format("t{}", id)}; }

}  // namespace

XTEST(XdeskLayout, ColumnsShareTheOutputUntilTooNarrowThenScrollWithTheFocus, .area = "luban", .covers = {"XDESK-LAYOUT", "XDESK-TILE-SIZES"}) {
    Layout l({.rule = Rule::tile, .inner_gap = 10, .outer_gap = 10, .min_column_width = 300});
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
    Layout l({.rule = Rule::tile, .inner_gap = 4, .outer_gap = 0});
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
    Layout l({.rule = Rule::tile, .inner_gap = 4, .outer_gap = 0, .min_column_width = 300, .flow = layout::Flow::vertical});
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
    Layout l({.rule = Rule::tile, .inner_gap = 10, .outer_gap = 10});
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
    Layout l({.rule = Rule::tile, .inner_gap = 10, .outer_gap = 10});
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

XTEST(XdeskLayout, StackedWindowsOverlapOnTheOutputAndSnapToHalvesAndQuarters, .area = "luban", .covers = {"XDESK-ARRANGEMENTS", "XDESK-STACK-OFFSCREEN"}) {
    Layout l({.rule = Rule::stack, .inner_gap = 10, .outer_gap = 10});
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
    // Put partly off the output, it stays there (J61): its title bar on it,
    // and 64 pixels of it across; further, it is kept to that.
    l.place(1, {-100, 700, 600, 400});
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{-100, 700, 600, 400}));
    l.place(1, {-900, 900, 600, 400});
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{-536, 774, 600, 400}));
    l.place(1, {1190, -50, 600, 400});
    EXPECT_EQ(of(l.arrange(kOutput), 1).rect, (Rect{1136, 0, 600, 400}));
    EXPECT_EQ(layout::kept_on({-900, 900, 600, 400}, {0, 0, 1200, 800}, 26), (Rect{-536, 774, 600, 400})) << "the drag's rule is the same";
    l.place(1, {300, 200, 600, 400});
    // A floating window is above the stack too.
    l.add(3, {300, 200}, true);
    ps = l.arrange(kOutput);
    EXPECT_EQ(ps.back().id, 3u);
    EXPECT_TRUE(ps.back().floating);
}

XTEST(XdeskLayout, TheMaximizeButtonsLayoutsLayOutTheRecentWindows, .area = "luban", .covers = {"XDESK-DRAG-SNAP"}) {
    Layout l({.rule = Rule::tile, .inner_gap = 10, .outer_gap = 10});
    for (layout::Id id = 1; id <= 3; ++id) l.add(id, {600, 400});
    l.arrange(kOutput);
    l.arrange_as(layout::Pattern::one_and_two, {3, 2, 1});
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(l.group_of(2), (std::vector<layout::Id>{2, 1}));
    EXPECT_LT(of(ps, 3).rect.x, of(ps, 2).rect.x);
    l.arrange_as(layout::Pattern::two_thirds, {3, 2});
    ps = l.arrange(kOutput);
    EXPECT_GT(of(ps, 3).rect.width, of(ps, 2).rect.width);
    Layout s({.rule = Rule::stack, .inner_gap = 10, .outer_gap = 10});
    s.add(1, {600, 400});
    s.add(2, {600, 400});
    s.arrange(kOutput);
    s.arrange_as(layout::Pattern::halves, {2, 1});
    ps = s.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect.x, 10);
    EXPECT_GT(of(ps, 1).rect.x, 590);
}

XTEST(XdeskLayout, TheGapBetweenTwoWindowsOfAColumnResizesThem, .area = "luban", .covers = {"XDESK-RESIZE"}) {
    Layout l({.rule = Rule::tile, .inner_gap = 4, .outer_gap = 0});
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
    Layout l({.rule = Rule::tile, .inner_gap = 0, .outer_gap = 0, .min_column_width = 600});
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

XTEST(XdeskLayout, AFullScreenWindowTakesTheWholeOutputAboveTheRest, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    xdesk::layout::Layout l;
    l.add(1, {400, 300});
    l.add(2, {400, 300});
    l.toggle_fullscreen(1);
    auto ps = l.arrange(kOutput, {.top = 30});
    ASSERT_FALSE(ps.empty());
    EXPECT_EQ(ps.back().id, 1u) << "above the rest";
    EXPECT_TRUE(ps.back().fullscreen);
    EXPECT_EQ(ps.back().rect, (xdesk::layout::Rect{0, 0, kOutput.width, kOutput.height})) << "the panel's part too";
    l.toggle_fullscreen(2);
    EXPECT_FALSE(l.fullscreen(1)) << "one on a workspace";
    l.remove(2);
    ps = l.arrange(kOutput, {.top = 30});
    for (const auto& p : ps) EXPECT_FALSE(p.fullscreen);
}

// ── workspaces on WuXian (design 2026-10-20 WuXian §3.1, §3.9) ───────

XTEST(XdeskLayout, WorkspacesAreNumberedRegionsOfThePlaneMadeAndRemovedAsAsked, .area = "luban", .covers = {"XDESK-LAYOUT", "WUXIAN-WORKSPACES"}) {
    Layout l({.inner_gap = 10, .outer_gap = 10});
    EXPECT_EQ(l.workspaces(), (std::vector<int>{1})) << "a first start has one workspace";
    l.add(1, {600, 400});
    l.arrange(kOutput);
    // Made on purpose: the smallest free number, right of the others on the plane.
    EXPECT_EQ(l.add_workspace(Rule::stack), 2);
    const auto* one = l.workspace_info(1);
    const auto* two = l.workspace_info(2);
    ASSERT_TRUE(one && two);
    EXPECT_GT(two->origin.x, one->origin.x + kOutput.width) << "right of the first";
    EXPECT_NE(two->color, one->color);
    EXPECT_GT(two->z, one->z);
    EXPECT_EQ(l.add_workspace(Rule::tile, 2), 0) << "a number taken";
    EXPECT_EQ(l.add_workspace(Rule::tile, 12), 0) << "past the nine Super reaches";
    // Gone to by a number it does not have: made with it; left as made -- empty, unnamed -- it goes again.
    l.show(7);
    EXPECT_EQ(l.current(), 7);
    l.show(1);
    EXPECT_EQ(l.workspaces(), (std::vector<int>{1, 2})) << "made by going to it, left empty: removed";
    l.show(7);
    l.rename(7, "notes");
    l.show(1);
    EXPECT_EQ(l.workspaces(), (std::vector<int>{1, 2, 7})) << "a named one stays";
    // Removed: its windows free on the plane, where they were.
    l.send_to(1, 7);
    EXPECT_EQ(l.workspace_of(1), 7);
    l.set_view(View::wuxian);
    auto ps = l.arrange(kOutput);
    const auto where = of(ps, 1).rect;
    EXPECT_TRUE(l.remove_workspace(7));
    EXPECT_TRUE(l.is_free(1));
    EXPECT_FALSE(l.workspace_of(1));
    ps = l.arrange(kOutput);
    EXPECT_NEAR(of(ps, 1).rect.x, where.x, 1e-6) << "where it was";
    EXPECT_NEAR(of(ps, 1).rect.width, where.width, 1e-6);
    EXPECT_TRUE(l.remove_workspace(2));
    EXPECT_FALSE(l.remove_workspace(1)) << "the last one stays";
    // Super+M: its other rule; its colour, flow and scale.
    l.set_rule(1, layout::other(Rule::tile));
    EXPECT_EQ(l.rule(), Rule::stack);
    l.set_scale(1, 5);
    EXPECT_DOUBLE_EQ(l.workspace_info(1)->scale, wx::kMaxScale);
}

XTEST(XdeskLayout, TheWuXianViewShowsEveryWorkspaceScaledAndTheFreeWindowsBesideThem, .area = "luban",
      .covers = {"XDESK-LAYOUT", "WUXIAN-VIEWS"}) {
    Layout l({.inner_gap = 10, .outer_gap = 10});
    l.add(1, {600, 400});
    ASSERT_EQ(l.add_workspace(Rule::tile), 2);
    l.show(2);
    l.add(2, {600, 400});
    l.set_scale(2, 0.5);
    auto ps = l.arrange(kOutput);
    EXPECT_TRUE(shown_(ps, 2) && !shown_(ps, 1)) << "the workspace view: the current workspace's only";
    const auto full = of(ps, 2).rect;
    EXPECT_EQ(of(ps, 2).zoom, 1.0) << "1:1, whatever its scale";
    // The WuXian view: framed on the current workspace, every workspace
    // there, each drawn at its scale times the camera's zoom.
    l.set_view(View::wuxian);
    ps = l.arrange(kOutput);
    ASSERT_EQ(l.frames().size(), 2u);
    EXPECT_TRUE(shown_(ps, 1) && shown_(ps, 2));
    const auto cam = l.camera();
    EXPECT_NEAR(of(ps, 2).zoom, 0.5 * cam.zoom, 1e-9);
    EXPECT_NEAR(of(ps, 2).rect.width, full.width * 0.5 * cam.zoom, 1e-6);
    EXPECT_EQ(l.workspace_at(of(ps, 2).rect.x + 5, of(ps, 2).rect.y + 5), 2);
    // A new window with the view's centre on nothing: free on the plane, near it.
    l.look({.at = {5000, 0}, .zoom = 1});
    l.add(3, {500, 300});
    EXPECT_TRUE(l.is_free(3));
    ps = l.arrange(kOutput);
    EXPECT_TRUE(of(ps, 3).visible) << "in view";
    EXPECT_EQ(of(ps, 3).workspace, 0);
    // Focused there, then back to the workspace view: the workspace it was,
    // the window it had; the free window not shown.
    l.focus(3);
    l.set_view(View::workspace);
    EXPECT_EQ(l.current(), 2);
    EXPECT_EQ(l.focused(), 2u);
    ps = l.arrange(kOutput);
    EXPECT_FALSE(shown_(ps, 3));
    // The workspace view stays on its workspace: another's window is what it focuses when shown.
    l.focus(1);
    EXPECT_EQ(l.current(), 2);
    l.show(1);
    EXPECT_EQ(l.focused(), 1u);
    // The camera: panned, zoomed about a point, everything shown.
    l.set_view(View::wuxian);
    const auto before = l.camera();
    l.pan(100, 0);
    EXPECT_NEAR(l.camera().at.x, before.at.x - 100 / before.zoom, 1e-9);
    l.zoom_at(2, 600, 400);
    EXPECT_NEAR(l.zoom(), std::min(wx::kMaxZoom, before.zoom * 2), 1e-9);
    l.show_all();
    ps = l.arrange(kOutput);
    for (const auto id : {1u, 2u, 3u}) EXPECT_TRUE(of(ps, id).visible) << id;
    const auto m = l.minimap();
    EXPECT_EQ(m.boxes.size(), 3u) << "two workspaces and a free window";
}

// Collapsed (design 2026-10-21 §8.5, openwx §5.2): a workspace is its label
// strip, its windows neither shown nor hit; a free window is its title bar.
// Both are kept by the state.
XTEST(XdeskLayout, ACollapsedWorkspaceIsItsLabelAndAFreeWindowItsTitleBar, .area = "luban", .covers = {"WUXIAN-COLLAPSE"}) {
    Layout l({.inner_gap = 4, .outer_gap = 0, .title = 26});
    l.add(1, {600, 400}, false, match_(1));
    ASSERT_EQ(l.add_workspace(Rule::stack), 2);
    l.show(2);
    l.add(2, {600, 400}, false, match_(2));
    l.set_view(View::wuxian);
    l.show_all();
    l.look({.at = {l.camera().at.x, l.camera().at.y}, .zoom = 0.5});
    l.add(3, {500, 300}, false, match_(3));
    l.to_wuxian(3);
    ASSERT_TRUE(l.is_free(3));
    auto ps = l.arrange(kOutput);
    const auto whole = *std::ranges::find(l.frames(), 2, &layout::Frame::id);
    const auto two = of(ps, 2).rect;
    ASSERT_TRUE(shown_(ps, 2));
    l.toggle_workspace_collapsed(2);
    EXPECT_TRUE(l.workspace_collapsed(2));
    ps = l.arrange(kOutput);
    const auto f2 = *std::ranges::find(l.frames(), 2, &layout::Frame::id);
    EXPECT_TRUE(f2.collapsed);
    EXPECT_EQ(f2.screen.height, 26) << "its label strip, in screen pixels";
    EXPECT_LE(f2.screen.width, whole.screen.width);
    EXPECT_FALSE(shown_(ps, 2)) << "its windows are not placed";
    EXPECT_FALSE(l.at(two.x + two.width / 2, two.y + two.height / 2).has_value()) << "nor hit";
    // A free window: its title bar, at the camera's zoom; below it is not it.
    const auto three = of(ps, 3).rect;
    l.toggle_collapsed(3);
    ps = l.arrange(kOutput);
    EXPECT_TRUE(of(ps, 3).collapsed);
    EXPECT_NEAR(of(ps, 3).rect.height, 26 * 0.5, 1e-9);
    EXPECT_EQ(of(ps, 3).rect.width, three.width);
    EXPECT_EQ(l.at(three.x + 5, three.y + 5), 3u);
    EXPECT_NE(l.at(three.x + 5, three.y + three.height - 5), std::optional<layout::Id>(3u));
    // The state keeps both; read back, both are as they were.
    const auto st = l.state(match_);
    EXPECT_TRUE(std::ranges::find(st.workspaces, 2, &wx::Workspace::id)->collapsed);
    const auto read = wx::parse(wx::format(st));
    ASSERT_TRUE(read) << read.error();
    Layout b({.inner_gap = 4, .outer_gap = 0, .title = 26});
    b.restore(*read);
    for (const layout::Id id : {1u, 2u, 3u}) b.add(id, {600, 400}, false, match_(id));
    EXPECT_TRUE(b.workspace_collapsed(2));
    EXPECT_TRUE(b.collapsed(3));
    // Expanded: as before.
    l.toggle_workspace_collapsed(2);
    l.toggle_collapsed(3);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect, two);
    EXPECT_EQ(of(ps, 3).rect, three);
}

// Covered items (design 2026-10-21 §8.4): nothing of it on the screen --
// under one, or under several together -- and the top-most over its centre
// carries its bookmark; one off the screen is elsewhere, not covered.
XTEST(XdeskLayout, AnItemOfWhichNothingShowsIsCoveredAndTheOneOverItsCentreCarriesItsBookmark, .area = "luban",
      .covers = {"WUXIAN-BOOKMARKS"}) {
    const Rect screen{0, 0, 1200, 800};
    // 0 under 2 entirely; 1 partly under 2; 3 under 4 and 5 together; 6 off the screen.
    const std::vector<Rect> items{{100, 100, 200, 150}, {500, 100, 300, 200}, {50, 50, 500, 300},
                                  {700, 400, 200, 200}, {650, 380, 160, 260}, {800, 380, 150, 260}, {5000, 0, 100, 100}};
    const auto c = layout::covered(items, screen);
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0], (layout::Covered{0, 2}));
    EXPECT_EQ(c[1].item, 3u);
    EXPECT_EQ(c[1].host, 5u) << "the top-most over its centre (800, 500)";
    // A sliver (under 4 pixels) is not showing; a strip of 10 is.
    EXPECT_EQ(layout::covered(std::vector<Rect>{{100, 100, 200, 200}, {50, 50, 300, 248}}, screen).size(), 1u);
    EXPECT_TRUE(layout::covered(std::vector<Rect>{{100, 100, 200, 200}, {50, 50, 300, 240}}, screen).empty());
    // Nothing above: nothing covered.
    EXPECT_TRUE(layout::covered(std::vector<Rect>{{0, 0, 100, 100}}, screen).empty());
    // A hundred windows, as many as a desktop has: well within a frame's time.
    std::vector<Rect> many;
    for (int k = 0; k < 100; ++k) many.push_back({double(k * 7 % 900), double(k * 13 % 500), 300, 200});
    const auto t0 = std::chrono::steady_clock::now();
    (void)layout::covered(many, screen);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(20));
}

// A frame's damage in a few rectangles (design 2026-10-21 §5.6a): the
// nearest merged until eight are left, every damaged pixel still in one, and
// far less than the whole output for a handful of small changes.
XTEST(XdeskLayout, ManyDamagedRectanglesAreMergedNotSentWhole, .area = "luban", .covers = {"XDESK-DAMAGE"}) {
    std::vector<Rect> rs;
    for (int k = 0; k < 20; ++k) rs.push_back({double(30 + (k % 5) * 200), double(40 + (k / 5) * 150), 12, 10});
    const auto merged = layout::merge_damage(rs, 8);
    EXPECT_LE(merged.size(), 8u);
    for (const auto& r : rs)
        EXPECT_TRUE(std::ranges::any_of(merged, [&](const Rect& m) {
            return r.x >= m.x && r.y >= m.y && r.x + r.width <= m.x + m.width && r.y + r.height <= m.y + m.height;
        })) << "every damaged rectangle in one";
    double area = 0;
    for (const auto& m : merged) area += m.width * m.height;
    EXPECT_LT(area, 1200.0 * 800 / 4) << "not the whole output";
    EXPECT_EQ(layout::merge_damage(rs, 30).size(), rs.size()) << "few enough: as they are";
}

// The views' change starts and ends where the workspace view is: the
// locked camera shows the workspace's content exactly as it does.
XTEST(XdeskLayout, TheLockedCameraShowsTheWorkspaceAsTheWorkspaceViewDoes, .area = "luban", .covers = {"WUXIAN-VIEWS"}) {
    Layout l({.inner_gap = 4, .outer_gap = 0, .min_column_width = 500});
    for (layout::Id id = 1; id <= 4; ++id) l.add(id, {600, 400});
    l.move_workspace(1, {300, -200});
    l.set_scale(1, 0.75);
    const auto shown = l.arrange(kOutput, {.top = 30});
    const auto cam = l.locked_camera();
    const auto through = l.arrange(kOutput, {.top = 30}, cam);
    for (const auto& p : shown) {
        EXPECT_NEAR(of(through, p.id).rect.x, p.rect.x, 1e-6) << p.id;
        EXPECT_NEAR(of(through, p.id).rect.y, p.rect.y, 1e-6) << p.id;
        EXPECT_NEAR(of(through, p.id).rect.width, p.rect.width, 1e-6) << p.id;
        EXPECT_NEAR(of(through, p.id).zoom, 1, 1e-9) << p.id;
    }
}

// ── dragging (design 2026-10-20 WuXian §3.7, §3.8) ───────────────────

XTEST(XdeskLayout, ATiledDragIsReadOffTheFrozenArrangementItsOwnPlaceKept, .area = "luban", .covers = {"XDESK-DRAG-SNAP", "WUXIAN-DRAG"}) {
    Layout l({.inner_gap = 4, .outer_gap = 0, .min_column_width = 300});
    l.add(1, {600, 400});
    l.add(2, {600, 400});
    l.add(3, {600, 400});
    auto ps = l.arrange(kOutput);
    const auto one = of(ps, 1).rect, two = of(ps, 2).rect;
    // Picked up on its own title: nothing (H19: it was a new last column).
    const auto d = l.begin_drag(1, one.x + 50, one.y + 10);
    EXPECT_EQ(l.drop_at(d, one.x + 50, one.y + 10).kind, K::none);
    EXPECT_EQ(l.drop_at(d, one.x + one.width / 2, one.y + one.height / 2).kind, K::none) << "anywhere on its own place";
    // Another's zones: quarters across, thirds along.
    auto at = [&](double fx, double fy) { return l.drop_at(d, two.x + fx * two.width, two.y + fy * two.height); };
    EXPECT_EQ(at(0.5, 0.1).kind, K::above);
    EXPECT_EQ(at(0.5, 0.9).kind, K::below);
    EXPECT_EQ(at(0.5, 0.5).kind, K::swap);
    EXPECT_EQ(at(0.2, 0.5).kind, K::column);
    EXPECT_EQ(at(0.8, 0.5).kind, K::column);
    EXPECT_EQ(l.drop_at(d, 5, 400).kind, K::column) << "the screen's edge";
    EXPECT_EQ(l.drop_at(d, 5, 400).column, 0u);
    EXPECT_EQ(l.drop_at(d, 600, 400, true).kind, K::floating) << "Shift";
    // The preview: its own place kept, the window it swaps with, and nothing moved.
    const auto swap = at(0.5, 0.5);
    const auto pv = l.preview(d, swap);
    EXPECT_EQ(pv.placeholder, one);
    EXPECT_EQ(pv.target, two);
    ASSERT_TRUE(pv.box);
    EXPECT_EQ(*pv.box, two) << "it would take the other's place";
    EXPECT_EQ(l.arrange(kOutput), ps) << "a preview changes nothing";
    const auto split = l.preview(d, at(0.5, 0.9));
    ASSERT_TRUE(split.half);
    EXPECT_NEAR(split.half->y, two.y + two.height / 2, 1e-6) << "the half below";
    const auto col = l.preview(d, l.drop_at(d, 5, 400));
    ASSERT_TRUE(col.line);
    EXPECT_LT(col.line->x, one.x + 4) << "a line where the new column goes";
    // Let go: once, to where the drop leaves everything.
    l.drop(d, swap);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect, two);
    EXPECT_EQ(of(ps, 2).rect, one);
    // Into a column, with a size chosen for a new one.
    const auto d2 = l.begin_drag(3, of(ps, 3).rect.x + 20, 10);
    const auto narrow = l.drop_at(d2, 5, 400);
    const auto seen = l.preview(d2, narrow, 1.0 / 3);
    l.drop(d2, narrow, 1.0 / 3);
    ps = l.arrange(kOutput);
    ASSERT_TRUE(seen.box);
    EXPECT_EQ(of(ps, 3).rect, *seen.box) << "the preview's frame is where it went";
    EXPECT_NEAR(of(ps, 3).rect.width, (1200 + 4) / 3.0 - 4, 1e-6);
    // The gap between two: dragged, one wider; twice, equal again.
    const auto gap = l.gap_at(of(ps, 3).rect.x + of(ps, 3).rect.width + 2, 300);
    ASSERT_TRUE(gap);
    l.drag_gap(*gap, 100);
    EXPECT_NEAR(of(l.arrange(kOutput), 3).rect.width, (1200 + 4) / 3.0 - 4 + 100, 1.5);
    l.even_widths();
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect.width, of(ps, 3).rect.width);
}

XTEST(XdeskLayout, StackedEdgesSnapAndAWindowLetGoStaysWhereItIs, .area = "luban", .covers = {"XDESK-DRAG-SNAP"}) {
    Layout s({.rule = Rule::stack, .inner_gap = 10, .outer_gap = 10});
    s.add(1, {600, 400});
    const auto ps = s.arrange(kOutput);
    const auto r = of(ps, 1).rect;
    const auto d = s.begin_drag(1, r.x + 100, r.y + 10);
    auto drop = s.drop_at(d, 3, 400);
    EXPECT_EQ(drop.kind, K::snap);
    EXPECT_EQ(drop.snap_x, -1);
    EXPECT_EQ(s.preview(d, drop).box, (Rect{10, 10, 585, 780})) << "the left half";
    drop = s.drop_at(d, 1197, 3);
    EXPECT_EQ(std::pair(drop.snap_x, drop.snap_y), std::pair(1, -1)) << "a corner: its quarter";
    EXPECT_EQ(s.drop_at(d, 600, 3).snap_x, 0) << "the top: the whole";
    drop = s.drop_at(d, 500, 300);
    EXPECT_EQ(drop.kind, K::free);
    s.drop(d, drop);
    EXPECT_EQ(of(s.arrange(kOutput), 1).rect, (Rect{400, 290, 600, 400})) << "where the pointer let it go";
}

// Snapped, picked up (design 2026-10-24 §9.2, J62): free again at the size
// it had before, under the pointer at the same fraction of its width.
XTEST(XdeskLayout, ASnappedWindowPickedUpHasItsOwnSizeBack, .area = "luban", .covers = {"XDESK-STACK-EDGE-SNAP"}) {
    Layout s({.rule = Rule::stack, .inner_gap = 10, .outer_gap = 10});
    s.add(1, {600, 400});
    auto r = of(s.arrange(kOutput), 1).rect;
    auto d = s.begin_drag(1, r.x + 100, r.y + 10);
    s.drop(d, s.drop_at(d, 3, 400));
    r = of(s.arrange(kOutput), 1).rect;
    EXPECT_EQ(r, (Rect{10, 10, 585, 780})) << "the left half";
    // Held at its middle, carried away.
    const auto back = s.unsnap(1, r.x + r.width / 2, r.y + 10);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->width, 600);
    EXPECT_EQ(back->height, 400);
    EXPECT_DOUBLE_EQ(back->x, r.x + r.width / 2 - 300) << "the pointer at the same fraction of it";
    EXPECT_EQ(of(s.arrange(kOutput), 1).rect.width, 600);
    EXPECT_FALSE(s.unsnap(1, 10, 10)) << "not snapped: nothing to do";
}

// A collapsed free window moved keeps the size it unfolds to (openwx 0.2
// §5.2, J63): only where it is changes.
XTEST(XdeskLayout, ACollapsedWindowMovedUnfoldsToItsSize, .area = "luban", .covers = {"XDESK-COLLAPSE-KEEPS-SIZE"}) {
    Layout l({.inner_gap = 4, .outer_gap = 0, .title = 26});
    l.add(1, {500, 300}, false, match_(1));
    l.set_view(View::wuxian);
    l.to_wuxian(1);
    auto ps = l.arrange(kOutput);
    const auto open = of(ps, 1).rect;
    l.toggle_collapsed(1);
    ps = l.arrange(kOutput);
    const auto bar = of(ps, 1).rect;
    ASSERT_LT(bar.height, open.height);
    // Dragged by hand (place), and dropped on the plane (a drag in the WuXian view).
    l.place(1, {bar.x + 120, bar.y + 80, bar.width, bar.height});
    const auto moved = of(l.arrange(kOutput), 1).rect;
    EXPECT_DOUBLE_EQ(moved.x, bar.x + 120);
    // And let go on the plane, as a drag in the WuXian view does.
    const auto d = l.begin_drag(1, moved.x + 10, moved.y + 5);
    l.drop(d, layout::Drop{.kind = K::plane, .at = {-2000, -1500}});
    l.toggle_collapsed(1);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect.width, open.width);
    EXPECT_EQ(of(ps, 1).rect.height, open.height) << "unfolded to its own size, not its title bar's";
    EXPECT_NE(of(ps, 1).rect.x, open.x) << "where it was moved";
}

XTEST(XdeskLayout, InTheWuXianViewAWindowGoesToAnotherWorkspaceOrThePlane, .area = "luban", .covers = {"WUXIAN-DRAG"}) {
    Layout l({.inner_gap = 4, .outer_gap = 0});
    l.add(1, {600, 400});
    l.add(2, {600, 400});
    ASSERT_EQ(l.add_workspace(Rule::stack), 2);
    l.set_view(View::wuxian);
    l.show_all();
    auto ps = l.arrange(kOutput);
    const auto f2 = *std::ranges::find(l.frames(), 2, &layout::Frame::id);
    const auto one = of(ps, 1).rect;
    // Onto the stacked workspace: in it, where it is let go.
    auto d = l.begin_drag(1, one.x + 5, one.y + 5);
    auto drop = l.drop_at(d, f2.screen.x + f2.screen.width / 2, f2.screen.y + f2.screen.height / 2);
    EXPECT_EQ(drop.kind, K::free);
    EXPECT_EQ(drop.workspace, 2);
    const auto two_before = of(ps, 2).rect;
    EXPECT_EQ(of(l.arrange(kOutput), 2).rect, two_before) << "nothing moves while it is dragged";
    l.drop(d, drop);
    EXPECT_EQ(l.workspace_of(1), 2);
    EXPECT_EQ(l.windows_in(1), (std::vector<layout::Id>{2}));
    // Onto the plane: free, its own size, its top-left where the pointer had it.
    ps = l.arrange(kOutput);
    const auto r = of(ps, 1).rect;
    d = l.begin_drag(1, r.x + 5, r.y + 5);
    drop = l.drop_at(d, f2.screen.x + f2.screen.width / 2, f2.screen.y + f2.screen.height + 300);
    EXPECT_EQ(drop.kind, K::plane);
    const auto box = l.preview(d, drop).box;
    l.drop(d, drop);
    EXPECT_TRUE(l.is_free(1));
    ps = l.arrange(kOutput);
    ASSERT_TRUE(box);
    EXPECT_NEAR(of(ps, 1).rect.x, box->x, 1e-6);
    EXPECT_NEAR(of(ps, 1).rect.width, box->width, 1e-6);
    // From the plane onto the tiled workspace: a column of its own beside the window there.
    const auto f1 = *std::ranges::find(l.frames(), 1, &layout::Frame::id);
    d = l.begin_drag(1, of(ps, 1).rect.x + 5, of(ps, 1).rect.y + 5);
    drop = l.drop_at(d, f1.screen.x + f1.screen.width / 2, f1.screen.y + f1.screen.height / 2);
    EXPECT_EQ(drop.kind, K::column) << "no place to swap into: a column";
    l.drop(d, drop);
    EXPECT_EQ(l.workspace_of(1), 1);
    EXPECT_EQ(l.windows_in(1).size(), 2u);
    // Moving between workspaces and the plane by keys.
    l.to_wuxian(2);
    EXPECT_TRUE(l.is_free(2));
    l.send_to(2, 2);
    EXPECT_EQ(l.workspace_of(2), 2);
}

// ── the state (design 2026-10-20 WuXian §3.14) ───────────────────────

XTEST(XdeskLayout, TheStateReadBackPutsEachWindowWhereItWasAsItComes, .area = "luban", .covers = {"WUXIAN-STATE", "XDESK-CONFIG"}) {
    Layout a({.inner_gap = 4, .outer_gap = 0});
    for (layout::Id id = 1; id <= 3; ++id) a.add(id, {600, 400}, false, match_(id));
    a.join();                                   // 3 under 2
    a.focus(1);
    a.cycle_width();
    ASSERT_EQ(a.add_workspace(Rule::stack), 2);
    a.rename(2, "notes");
    a.set_scale(2, 0.5);
    a.set_view(View::wuxian);
    a.look({.at = {90000, 0}, .zoom = 1});
    a.add(4, {500, 300}, false, match_(4));
    a.set_view(View::workspace);
    const auto before = a.arrange(kOutput);
    const auto st = a.state(match_);
    EXPECT_TRUE(wx::validate(st).empty()) << wx::validate(st).front();
    const auto text = wx::format(st);
    const auto read = wx::parse(text);
    ASSERT_TRUE(read) << read.error();
    Layout b({.inner_gap = 4, .outer_gap = 0});
    b.restore(*read);
    EXPECT_EQ(b.workspaces(), (std::vector<int>{1, 2}));
    EXPECT_EQ(b.workspace_info(2)->name, "notes");
    // The windows come in another order: each goes where it was.
    for (const layout::Id id : {4u, 3u, 1u, 2u}) b.add(id, {600, 400}, false, match_(id));
    b.focus(1);
    const auto after = b.arrange(kOutput);
    for (const auto& p : before) EXPECT_EQ(of(after, p.id).rect, p.rect) << p.id;
    EXPECT_TRUE(b.is_free(4));
    // A window nobody named: a new one, as ever.
    b.add(9, {600, 400}, false, match_(9));
    EXPECT_EQ(b.workspace_of(9), 1);
}

// ── the wheel (design 2026-10-20 WuXian §4.4) ────────────────────────

XTEST(XdeskLayout, TheWheelGoesAColumnAStepWhenSlowAndFurtherWhenFast, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    const layout::WheelSpeed s;
    EXPECT_DOUBLE_EQ(layout::wheel_distance(s, 1, 400), 60);
    EXPECT_DOUBLE_EQ(layout::wheel_distance(s, 1, 0), 60) << "the first step";
    EXPECT_GT(layout::wheel_distance(s, 1, 50), 60);
    EXPECT_DOUBLE_EQ(layout::wheel_distance(s, 1, 1), 360) << "at most six times";
    EXPECT_DOUBLE_EQ(layout::wheel_distance({.factor = 1.6}, -1, 400), -96);
}

// ── openwx: every rule of the specification ──────────────────────────

namespace {

class Adapter : public wx::conformance::Adapter {
public:
    void resize(wx::Size o) override { output_ = o; arrange_(); }
    int create_workspace(Rule r, std::optional<int> id) override { const int n = l_.add_workspace(r, id); arrange_(); return n; }
    bool remove_workspace(int id, wx::Removal how) override {
        const auto ws = l_.windows_in(id);
        if (!l_.remove_workspace(id)) return false;
        // Asked to close, they close.
        if (how == wx::Removal::close)
            for (const auto w : ws) { closed_.push_back(w); l_.remove(w); }
        arrange_();
        return true;
    }
    void show(int id) override { l_.show(id); arrange_(); }
    void set_view(View v) override { l_.set_view(v); arrange_(); }
    void set_scale(int id, double s) override { l_.set_scale(id, s); arrange_(); }
    void move_workspace(int id, wx::Point p) override { l_.move_workspace(id, p); arrange_(); }
    void raise(int id) override { l_.raise(id); arrange_(); }
    void look(const wx::Camera& c) override { l_.look(c); arrange_(); }
    void add_window(wx::Id id, wx::Size s) override { l_.add(id, s, false, {std::to_string(id)}); arrange_(); }
    std::vector<wx::Id> closed() override { return std::exchange(closed_, {}); }
    void begin_drag(wx::Id id) override { dragging_ = id; session_.reset(); }
    void drag_to(wx::Point p) override {
        if (!session_) session_ = l_.begin_drag(dragging_, p.x, p.y);
        pointer_ = p;
    }
    void end_drag() override {
        if (!session_) session_ = l_.begin_drag(dragging_, pointer_.x, pointer_.y);
        l_.drop(*session_, l_.drop_at(*session_, pointer_.x, pointer_.y));
        session_.reset();
        arrange_();
    }
    std::vector<std::pair<wx::Id, wx::Rect>> shown() override {
        std::vector<std::pair<wx::Id, wx::Rect>> out;
        for (const auto& p : l_.arrange(output_))
            if (p.visible) out.push_back({p.id, p.rect});
        return out;
    }
    std::optional<wx::Rect> frame(int id) override {
        for (const auto& f : l_.frames())
            if (f.id == id) return f.screen;
        return std::nullopt;
    }
    wx::State state() override { return l_.state([](layout::Id id) { return layout::Match{std::to_string(id)}; }); }

private:
    void arrange_() { (void)l_.arrange(output_); }
    Layout l_{{.inner_gap = 4, .outer_gap = 0}};
    wx::Size output_{1200, 800};
    std::vector<wx::Id> closed_;
    wx::Id dragging_ { 0 };
    std::optional<layout::DragSession> session_;
    wx::Point pointer_;
};

}  // namespace

XTEST(XdeskLayout, ItKeepsEveryRuleOfOpenwx, .area = "luban", .covers = {"WUXIAN-CONFORMANCE"}) {
    const auto r = wx::conformance::run([] { return std::make_unique<Adapter>(); });
    for (const auto& f : r.failed) ADD_FAILURE() << f;
    EXPECT_GE(r.passed, 9);
}

XTEST(XdeskLayout, TidyingUpPutsTheWorkspacesInARowAndTheViewGoesIntoTheOneAsked, .area = "luban", .covers = {"WUXIAN-WORKSPACES", "WUXIAN-VIEWS"}) {
    Layout l({.inner_gap = 4, .outer_gap = 0});
    l.add(1, {600, 400});
    ASSERT_EQ(l.add_workspace(Rule::stack, std::nullopt, wx::Point{-5000, 3000}), 2);
    ASSERT_EQ(l.add_workspace(Rule::tile, std::nullopt, wx::Point{900, -700}), 3);
    l.arrange(kOutput);
    l.line_up();
    const auto *a = l.workspace_info(1), *b = l.workspace_info(2), *c = l.workspace_info(3);
    EXPECT_EQ(b->origin.y, a->origin.y);
    EXPECT_EQ(c->origin.y, a->origin.y);
    EXPECT_GT(b->origin.x, a->origin.x + kOutput.width) << "in a row by number, apart";
    EXPECT_GT(c->origin.x, b->origin.x + kOutput.width);
    // The camera that ends the views' change is the workspace it goes into's.
    l.set_view(View::wuxian);
    l.focus(1);
    EXPECT_EQ(l.focused_workspace(), 1);
    const auto into = l.locked_camera(3);
    EXPECT_EQ(into.at, c->origin);
    EXPECT_DOUBLE_EQ(into.zoom, 1);
}

XTEST(XdeskLayout, TheWheelScrollsAColumnAtATimeAndAFreeScrollLandsOnOne, .area = "luban", .covers = {"XDESK-LAYOUT"}) {
    Layout l({.inner_gap = 0, .outer_gap = 0, .min_column_width = 500});
    for (layout::Id id = 1; id <= 4; ++id) l.add(id, {600, 400});
    l.focus(1);
    auto ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect.x, 0);
    l.scroll_columns(1);
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 2).rect.x, 0) << "one step: the next column at the start";
    l.scroll_columns(10);
    ps = l.arrange(kOutput);
    EXPECT_LE(of(ps, 4).rect.x + of(ps, 4).rect.width, kOutput.width + 1e-6) << "no further than the last";
    l.scroll_columns(-10);
    l.pan(-130, 0);                              // a free scroll, between columns
    l.snap_scroll();
    ps = l.arrange(kOutput);
    EXPECT_EQ(of(ps, 1).rect.x, 0) << "back on the nearest column's start";
}
