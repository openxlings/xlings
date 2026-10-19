// Stacking (layout.cppm): windows free on the screen, overlapping, kept on
// the output; maximized, or snapped to a half or a quarter of it.
module xdesk.layout;

import std;

namespace xdesk::layout {

using detail::overlaps;

Rect Layout::stack_rect_(const Window& w, Rect area) const {
    const double g = o_.gap;
    if (w.snapped) {
        // -1 the left (top) half, 0 all of it, 1 the right (bottom) half.
        auto span = [&](int part, double from, double size) -> std::pair<double, double> {
            const double whole = size - 2 * g, half = (size - 3 * g) / 2;
            if (part < 0) return {from + g, half};
            if (part > 0) return {from + 2 * g + half, half};
            return {from + g, whole};
        };
        const auto [x, width] = span(w.snap_x, area.x, area.width);
        const auto [y, height] = span(w.snap_y, area.y, area.height);
        return {x, y, std::max(1.0, width), std::max(1.0, height)};
    }
    Rect r = w.stacked;
    r.width = std::min(r.width, area.width);
    r.height = std::min(r.height, area.height);
    r.x = std::clamp(r.x, area.x, area.x + area.width - r.width);
    r.y = std::clamp(r.y, area.y, area.y + area.height - r.height);
    return r;
}

// In the middle, as big as it asked (or fits); down and right of one that
// is exactly there already, so each shows.
void Layout::stack_place_new_(Space& s, Window& w, Rect area) {
    const double width = std::min(w.preferred.width, area.width - 4 * o_.gap);
    const double height = std::min(w.preferred.height, area.height - 4 * o_.gap);
    Rect r { area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height };
    for (int tries = 0; tries < 16; ++tries) {
        const bool taken = std::ranges::any_of(s.windows, [&](const Window& o) {
            return o.id != w.id && !o.floating && o.stack_placed && !o.snapped && std::abs(o.stacked.x - r.x) < 2
                   && std::abs(o.stacked.y - r.y) < 2;
        });
        if (!taken) break;
        r.x += 32;
        r.y += 32;
        if (r.x + r.width > area.x + area.width || r.y + r.height > area.y + area.height) {
            r.x = area.x + 2 * o_.gap;
            r.y = area.y + 2 * o_.gap;
        }
    }
    w.stacked = r;
    w.stack_placed = true;
}

// Super+Shift+arrows: from free, left and right are the halves and up the
// whole output; then each way steps through halves and quarters; down from
// the whole output or the bottom is free again, where it was.
void Layout::stack_move_(Window& w, Direction d) {
    if (!w.snapped) {
        if (d == Direction::down) return;
        w.snapped = true;
        w.snap_x = d == Direction::left ? -1 : d == Direction::right ? 1 : 0;
        w.snap_y = 0;
        return;
    }
    switch (d) {
        case Direction::left: w.snap_x = std::max(-1, w.snap_x - 1); break;
        case Direction::right: w.snap_x = std::min(1, w.snap_x + 1); break;
        case Direction::up:
            if (w.snap_x != 0 || w.snap_y != 0) w.snap_y = std::max(-1, w.snap_y - 1);
            break;
        case Direction::down:
            if ((w.snap_x == 0 && w.snap_y == 0) || w.snap_y == 1) w.snapped = false;
            else w.snap_y += 1;
            break;
    }
}

std::vector<Placement> Layout::stack_(Space& s, Rect area) {
    std::vector<Placement> out;
    for (const Id id : s.order) {
        auto* w = window_(id);
        if (!w || w->floating) continue;
        if (!w->stack_placed) stack_place_new_(s, *w, area);
        const Rect r = stack_rect_(*w, area);
        out.push_back({.id = id, .rect = r, .visible = overlaps(r, area), .focused = s.focus == id,
                       .maximized = w->snapped && w->snap_x == 0 && w->snap_y == 0});
    }
    return out;
}

}  // namespace xdesk::layout
