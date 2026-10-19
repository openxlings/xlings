// Dragging a window, the gaps between columns, the maximize button's layouts
// (layout.cppm; design 2026-10-19 §17.9, §17.10). Where a drop lands is read
// off the last arrangement; its preview is the drop made on a copy, arranged.
module xdesk.layout;

import std;

namespace xdesk::layout {

namespace {

constexpr double kEdge = 24;        // a screen edge's band, in pixels
constexpr double kGroupEdge = 32;   // a window's edge on the canvas, inside its frame

}  // namespace

Drop Layout::drop_at(Id dragged, double sx, double sy, bool shift) const {
    if (shift) return {.kind = Drop::Kind::floating};
    const auto& s = space_();
    const Rect area = area_();
    // The window under the pointer (the dragged one and floating ones aside).
    std::optional<Placement> over;
    for (auto it = last_.rbegin(); it != last_.rend(); ++it)
        if (it->id != dragged && !it->floating && it->visible && it->rect.contains(sx, sy)) { over = *it; break; }
    if (s.mode == Mode::tile) {
        if (sx < area.x + kEdge) return {.kind = Drop::Kind::column, .column = 0};
        if (sx > area.x + area.width - kEdge) return {.kind = Drop::Kind::column, .column = s.columns.size()};
        if (!over) return {.kind = Drop::Kind::column, .column = s.columns.size()};
        const auto at = find_(s.columns, over->id);
        const double fx = (sx - over->rect.x) / std::max(1.0, over->rect.width), fy = (sy - over->rect.y) / std::max(1.0, over->rect.height);
        if (fx > 1.0 / 3 && fx < 2.0 / 3 && fy > 1.0 / 3 && fy < 2.0 / 3) return {.kind = Drop::Kind::swap, .target = over->id};
        if (fy <= 1.0 / 3) return {.kind = Drop::Kind::above, .target = over->id};
        if (fy >= 2.0 / 3) return {.kind = Drop::Kind::below, .target = over->id};
        const std::size_t c = at ? at->column : s.columns.size();
        return {.kind = Drop::Kind::column, .column = fx < 0.5 ? c : c + 1};
    }
    if (s.mode == Mode::stack) {
        const bool left = sx < area.x + kEdge, right = sx > area.x + area.width - kEdge;
        const bool top = sy < area.y + kEdge, bottom = sy > area.y + area.height - kEdge;
        if (left || right) return {.kind = Drop::Kind::snap, .snap_x = left ? -1 : 1, .snap_y = top ? -1 : bottom ? 1 : 0};
        if (top) return {.kind = Drop::Kind::snap};
        return {.kind = Drop::Kind::free};
    }
    // The canvas: another window's edge makes or joins its group.
    if (over) {
        const auto& r = over->rect;
        if (sx < r.x + kGroupEdge) return {.kind = Drop::Kind::left, .target = over->id};
        if (sx > r.x + r.width - kGroupEdge) return {.kind = Drop::Kind::right, .target = over->id};
        if (sy < r.y + kGroupEdge) return {.kind = Drop::Kind::above, .target = over->id};
        if (sy > r.y + r.height - kGroupEdge) return {.kind = Drop::Kind::below, .target = over->id};
    }
    return {.kind = Drop::Kind::free};
}

void Layout::drop(Id dragged, const Drop& d, double width) {
    auto* s = space_of_(dragged);
    auto* w = window_(dragged);
    if (!s || !w || d.kind == Drop::Kind::none) return;
    if (d.kind == Drop::Kind::floating) {
        if (!w->floating) toggle_floating(dragged);
        return;
    }
    // Back into the arrangement first, when it floated.
    if (w->floating && d.kind != Drop::Kind::free) toggle_floating(dragged);
    if (s->mode == Mode::tile) {
        // Where it goes is said by the window that is there now (the drag's
        // own column may close up when it leaves).
        std::optional<Id> before;
        if (d.kind == Drop::Kind::column && d.column < s->columns.size()) {
            for (std::size_t c = d.column; c < s->columns.size() && !before; ++c)
                for (const auto& cell : s->columns[c].cells)
                    if (cell.id != dragged) { before = cell.id; break; }
        }
        if (d.kind == Drop::Kind::swap) {
            const auto a = find_(s->columns, dragged), b = find_(s->columns, d.target);
            if (a && b) std::swap(s->columns[a->column].cells[a->cell].id, s->columns[b->column].cells[b->cell].id);
            focus(dragged);
            return;
        }
        erase_(s->columns, dragged);
        if (d.kind == Drop::Kind::column) {
            std::size_t at = s->columns.size();
            if (before)
                if (const auto b = find_(s->columns, *before)) at = b->column;
            s->columns.insert(s->columns.begin() + static_cast<std::ptrdiff_t>(at), Column{.cells = {{.id = dragged}}, .width = width});
        } else if (d.kind == Drop::Kind::above || d.kind == Drop::Kind::below) {
            if (const auto t = find_(s->columns, d.target)) {
                auto& col = s->columns[t->column];
                col.cells.insert(col.cells.begin() + static_cast<std::ptrdiff_t>(t->cell + (d.kind == Drop::Kind::below ? 1 : 0)), Cell{.id = dragged});
                for (auto& c : col.cells) c.share = 0;
            } else {
                insert_column_(s->columns, dragged, std::nullopt);
            }
        } else {
            insert_column_(s->columns, dragged, std::nullopt);
        }
        focus(dragged);
        return;
    }
    if (s->mode == Mode::stack) {
        if (d.kind == Drop::Kind::snap) {
            w->snapped = true;
            w->snap_x = d.snap_x;
            w->snap_y = d.snap_y;
        }
        focus(dragged);
        return;
    }
    // The canvas.
    switch (d.kind) {
        case Drop::Kind::left: group_with(dragged, d.target, Direction::left); break;
        case Drop::Kind::right: group_with(dragged, d.target, Direction::right); break;
        case Drop::Kind::above: group_with(dragged, d.target, Direction::up); break;
        case Drop::Kind::below: group_with(dragged, d.target, Direction::down); break;
        case Drop::Kind::free:
            // Out of its group where it was let go (the frame it has now).
            if (auto* g = group_(*s, dragged); g && !(g->columns.size() == 1 && g->columns.front().cells.size() == 1)) {
                const auto r = world_rect_(*s, dragged);
                erase_(g->columns, dragged);
                refit_group_(*g);
                if (r) s->groups.push_back({.world = *r, .placed = true, .columns = {{.cells = {{.id = dragged}}, .width = r->width}}});
            }
            break;
        default: break;
    }
    focus(dragged);
}

std::optional<Rect> Layout::preview(Id dragged, const Drop& d, double width) const {
    if (d.kind == Drop::Kind::none || d.kind == Drop::Kind::free || d.kind == Drop::Kind::floating) return std::nullopt;
    // The drop, made on a copy: where it would be is where it goes.
    auto copy = *this;
    copy.drop(dragged, d, width);
    for (const auto& p : copy.arrange(output_, reserved_))
        if (p.id == dragged) return p.rect;
    return std::nullopt;
}

std::optional<std::size_t> Layout::gap_at(double sx, double sy) const {
    const auto& s = space_();
    if (s.mode != Mode::tile || s.columns.size() < 2) return std::nullopt;
    const Rect area = area_();
    if (sy < area.y || sy > area.y + area.height) return std::nullopt;
    auto edge = [&](const Column& c) -> std::optional<Rect> {
        for (const auto& p : last_)
            if (p.id == c.cells.front().id) return p.rect;
        return std::nullopt;
    };
    for (std::size_t i = 0; i + 1 < s.columns.size(); ++i) {
        const auto a = edge(s.columns[i]), b = edge(s.columns[i + 1]);
        if (!a || !b) continue;
        const double left = a->x + a->width, right = b->x;
        if (sx >= left - 3 && sx <= right + 3) return i;
    }
    return std::nullopt;
}

void Layout::drag_gap(std::size_t column, double dx) {
    auto& s = space_();
    if (s.mode != Mode::tile || column + 1 >= s.columns.size()) return;
    const double total = std::max(1.0, tile_length_(s) - 2 * o_.outer_gap + o_.inner_gap);
    const double fa = tile_fraction_(s, column), fb = tile_fraction_(s, column + 1), df = dx / total;
    // Every column keeps the width it has now; these two trade.
    std::vector<double> now;
    for (std::size_t i = 0; i < s.columns.size(); ++i) now.push_back(tile_fraction_(s, i));
    for (std::size_t i = 0; i < s.columns.size(); ++i) s.columns[i].width = now[i];
    auto& a = s.columns[column];
    auto& b = s.columns[column + 1];
    const double na = std::clamp(fa + df, 0.1, fa + fb - 0.1);
    a.width = na;
    b.width = fa + fb - na;
}

void Layout::even_widths() {
    for (auto& c : space_().columns) c.width = 0;
}

void Layout::arrange_as(Pattern p, const std::vector<Id>& windows) {
    auto& s = space_();
    std::vector<Id> ws;
    for (const auto id : windows)
        if (const auto* w = window_(id); w && !w->floating && space_of_(id) == &s) ws.push_back(id);
    const std::size_t need = p == Pattern::halves || p == Pattern::two_thirds ? 2 : 3;
    if (ws.empty()) return;
    if (ws.size() > need) ws.resize(need);
    const Rect area = area_();
    if (s.mode == Mode::tile) {
        // Where the first was, the columns of the layout in its place.
        std::size_t at = s.columns.size();
        if (const auto f = find_(s.columns, ws.front())) at = f->column;
        for (const auto id : ws) erase_(s.columns, id);
        at = std::min(at, s.columns.size());
        std::vector<Column> cols;
        switch (p) {
            case Pattern::halves: for (const auto id : ws) cols.push_back({.cells = {{.id = id}}, .width = 0.5}); break;
            case Pattern::two_thirds:
                cols.push_back({.cells = {{.id = ws[0]}}, .width = 2.0 / 3});
                if (ws.size() > 1) cols.push_back({.cells = {{.id = ws[1]}}, .width = 1.0 / 3});
                break;
            case Pattern::one_and_two: {
                cols.push_back({.cells = {{.id = ws[0]}}, .width = 0.5});
                Column right{.width = 0.5};
                for (std::size_t k = 1; k < ws.size(); ++k) right.cells.push_back({.id = ws[k]});
                if (!right.cells.empty()) cols.push_back(std::move(right));
                break;
            }
            case Pattern::thirds: for (const auto id : ws) cols.push_back({.cells = {{.id = id}}, .width = 1.0 / 3}); break;
        }
        s.columns.insert(s.columns.begin() + static_cast<std::ptrdiff_t>(at), cols.begin(), cols.end());
        focus(ws.front());
        return;
    }
    if (s.mode == Mode::stack) {
        // Halves and quarters by snapping; the rest by their frames.
        const double g = o_.inner_gap;
        auto frame = [&](double x0, double x1, double y0, double y1) {
            return Rect{area.x + g + x0 * (area.width - g), area.y + g + y0 * (area.height - g), (x1 - x0) * (area.width - g) - g,
                        (y1 - y0) * (area.height - g) - g};
        };
        std::vector<Rect> rects;
        switch (p) {
            case Pattern::halves: rects = {frame(0, 0.5, 0, 1), frame(0.5, 1, 0, 1)}; break;
            case Pattern::two_thirds: rects = {frame(0, 2.0 / 3, 0, 1), frame(2.0 / 3, 1, 0, 1)}; break;
            case Pattern::one_and_two: rects = {frame(0, 0.5, 0, 1), frame(0.5, 1, 0, 0.5), frame(0.5, 1, 0.5, 1)}; break;
            case Pattern::thirds: rects = {frame(0, 1.0 / 3, 0, 1), frame(1.0 / 3, 2.0 / 3, 0, 1), frame(2.0 / 3, 1, 0, 1)}; break;
        }
        for (std::size_t k = 0; k < ws.size() && k < rects.size(); ++k) {
            auto* w = window_(ws[k]);
            w->snapped = false;
            w->stacked = rects[k];
            w->stack_placed = true;
        }
        focus(ws.front());
        return;
    }
    // The canvas: a tiled group of them, the first where it is.
    if (ws.size() > 1) group_with(ws[1], ws[0], Direction::right);
    if (ws.size() > 2) group_with(ws[2], ws[1], p == Pattern::one_and_two ? Direction::down : Direction::right);
    if (auto* g = group_(s, ws.front())) {
        const double full = area.width;
        if (p == Pattern::two_thirds && g->columns.size() >= 2) {
            g->columns[0].width = full * 2 / 3;
            g->columns[1].width = full / 3;
        } else if (p == Pattern::thirds) {
            for (auto& c : g->columns) c.width = full / 3;
        } else {
            for (auto& c : g->columns) c.width = full / 2;
        }
        refit_group_(*g);
    }
    focus(ws.front());
}

}  // namespace xdesk::layout
