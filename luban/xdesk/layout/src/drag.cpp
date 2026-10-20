// Dragging a window, the gaps between columns, the maximize button's layouts
// (layout.cppm; design 2026-10-19 §17.9, §17.10; 2026-10-20 WuXian §3.7,
// §3.8). A drag freezes the arrangement: where a drop lands is read off it
// as it was when the window was picked up, and nothing else moves until it
// is let go -- then once, to where the drop leaves everything.
module xdesk.layout;

import std;
import openwx;

namespace xdesk::layout {

namespace {

constexpr double kEdge = 24;        // an edge's band, in screen pixels
constexpr double kLine = 4;         // a new column's line, in screen pixels

std::optional<Rect> rect_in_(const std::vector<Placement>& ps, Id id) {
    for (const auto& p : ps)
        if (p.id == id) return p.rect;
    return std::nullopt;
}

const Placement* placement_in_(const std::vector<Placement>& ps, Id id) {
    for (const auto& p : ps)
        if (p.id == id) return &p;
    return nullptr;
}

}  // namespace

DragSession Layout::begin_drag(Id id, double sx, double sy) const {
    DragSession d{.id = id, .view = view_, .snapshot = last_, .frames = frames_};
    if (const auto* s = space_of_(id)) d.from = s->meta.id;
    else if (sticky(id)) d.from = current_;
    if (const auto r = rect_in_(last_, id)) d.grab = {sx - r->x, sy - r->y};
    return d;
}

Drop Layout::drop_at(const DragSession& d, double sx, double sy, bool shift) const {
    const Rect area = area_();
    if (floating(d.id) && d.view == View::workspace) return {};
    // Where the pointer is: the workspace view's workspace, else the
    // top-most workspace under it in the WuXian view, else the plane.
    const Space* s = nullptr;
    Rect box = area;
    double zoom = 1;
    if (d.view == View::workspace) {
        s = &cur_();
        if (shift) return {.kind = Drop::Kind::floating, .workspace = s->meta.id};
    } else {
        for (auto it = d.frames.rbegin(); it != d.frames.rend() && !s; ++it)
            if (it->screen.contains(sx, sy)) {
                s = space_(it->id);
                box = it->screen;
                zoom = it->zoom;
            }
        if (!s) {
            const auto at = openwx::to_world(camera_, {sx - d.grab.x, sy - d.grab.y}, {area.x, area.y});
            return {.kind = Drop::Kind::plane, .at = at};
        }
    }
    const int ws = s->meta.id;
    const bool same = d.from == ws && !is_free(d.id);
    if (s->meta.rule == Rule::stack) {
        // Stacked: halves and quarters at the edges (the workspace view),
        // else where it is let go, in the workspace view's pixels.
        if (d.view == View::workspace && same) {
            const bool left = sx < area.x + kEdge, right = sx > area.x + area.width - kEdge;
            const bool top = sy < area.y + kEdge, bottom = sy > area.y + area.height - kEdge;
            if (left || right) return {.kind = Drop::Kind::snap, .workspace = ws, .snap_x = left ? -1 : 1, .snap_y = top ? -1 : bottom ? 1 : 0};
            if (top) return {.kind = Drop::Kind::snap, .workspace = ws};
        }
        return {.kind = Drop::Kind::free, .workspace = ws,
                .at = {area.x + (sx - d.grab.x - box.x) / zoom, area.y + (sy - d.grab.y - box.y) / zoom}};
    }
    // Tiled: what is under the pointer in the frozen arrangement.
    const bool vertical = s->meta.flow == Flow::vertical;
    const double a = vertical ? sy - box.y : sx - box.x, length = vertical ? box.height : box.width;
    const Placement* over = nullptr;
    for (auto it = d.snapshot.rbegin(); it != d.snapshot.rend(); ++it)
        if (it->workspace == ws && !it->floating && it->visible && it->rect.contains(sx, sy)) { over = &*it; break; }
    // The edges: a column first or last (on its own place too: the screen's edge says it).
    if (a < kEdge) return {.kind = Drop::Kind::column, .workspace = ws, .column = 0};
    if (a > length - kEdge) return {.kind = Drop::Kind::column, .workspace = ws, .column = s->columns.size()};
    if (over && over->id == d.id) return {};       // its own place: back where it was
    if (over) {
        const auto at = find_(s->columns, over->id);
        const double fx = (sx - over->rect.x) / std::max(1.0, over->rect.width), fy = (sy - over->rect.y) / std::max(1.0, over->rect.height);
        const double f_along = vertical ? fy : fx, f_across = vertical ? fx : fy;
        if (f_across <= 0.25) return {.kind = Drop::Kind::above, .workspace = ws, .target = over->id};
        if (f_across >= 0.75) return {.kind = Drop::Kind::below, .workspace = ws, .target = over->id};
        const std::size_t c = at ? at->column : s->columns.size();
        // From another workspace or the plane there is no place to swap into:
        // a column beside it.
        if (f_along > 1.0 / 3 && f_along < 2.0 / 3)
            return same ? Drop{.kind = Drop::Kind::swap, .workspace = ws, .target = over->id}
                        : Drop{.kind = Drop::Kind::column, .workspace = ws, .column = c + 1};
        return {.kind = Drop::Kind::column, .workspace = ws, .column = f_along < 0.5 ? c : c + 1};
    }
    // A gap between columns, or past them: a column there.
    std::size_t c = 0;
    bool past = false;
    for (std::size_t k = 0; k < s->columns.size(); ++k) {
        const auto r = s->columns[k].cells.empty() ? std::nullopt : rect_in_(d.snapshot, s->columns[k].cells.front().id);
        if (!r) continue;
        const double start = vertical ? r->y : r->x, end = vertical ? r->y + r->height : r->x + r->width;
        const double here = vertical ? sy : sx;
        if (here >= end) { c = k + 1; past = true; }
        else if (here < start) break;
    }
    if (s->columns.empty() || !same || past) return {.kind = Drop::Kind::column, .workspace = ws, .column = c};
    return {};
}

DropPreview Layout::preview(const DragSession& d, const Drop& drop, double width) const {
    DropPreview out;
    const Rect area = area_();
    const auto* own = placement_in_(d.snapshot, d.id);
    if (own && d.from && !own->floating) out.placeholder = own->rect;
    // Its own size (1:1): what it has on the plane, or in a stacked spot.
    const Size one{own ? own->rect.width / std::max(0.01, own->zoom) : 600, own ? own->rect.height / std::max(0.01, own->zoom) : 400};
    std::optional<Frame> frame;
    for (const auto& f : d.frames)
        if (f.id == drop.workspace) frame = f;
    auto to_screen = [&](Rect r) {
        if (d.view == View::workspace || !frame) return r;
        return Rect{frame->screen.x + (r.x - area.x) * frame->zoom, frame->screen.y + (r.y - area.y) * frame->zoom, r.width * frame->zoom,
                    r.height * frame->zoom};
    };
    using K = Drop::Kind;
    switch (drop.kind) {
        case K::none: case K::floating: return out;
        case K::plane: {
            const auto at = openwx::to_screen(camera_, drop.at, {area.x, area.y});
            out.box = Rect{at.x, at.y, one.width * camera_.zoom, one.height * camera_.zoom};
            return out;
        }
        case K::free:
            out.box = to_screen({drop.at.x, drop.at.y, one.width, one.height});
            return out;
        case K::snap: {
            Window w{.snapped = true, .snap_x = drop.snap_x, .snap_y = drop.snap_y};
            out.box = to_screen(stack_rect_(w, area));
            return out;
        }
        case K::swap: case K::above: case K::below: {
            out.target = rect_in_(d.snapshot, drop.target);
            if (out.target && drop.kind != K::swap) {
                const auto* s = space_(drop.workspace);
                const bool vertical = s && s->meta.flow == Flow::vertical;
                auto half = *out.target;
                if (vertical) {
                    half.width /= 2;
                    if (drop.kind == K::below) half.x += half.width;
                } else {
                    half.height /= 2;
                    if (drop.kind == K::below) half.y += half.height;
                }
                out.half = half;
            }
            break;
        }
        case K::column: {
            const auto* s = space_(drop.workspace);
            if (!s) return out;
            const bool vertical = s->meta.flow == Flow::vertical;
            const Rect box = d.view == View::workspace || !frame ? area : frame->screen;
            const double gap = o_.inner_gap * (frame && d.view == View::wuxian ? frame->zoom : 1);
            std::vector<Rect> cols;
            for (const auto& c : s->columns) {
                std::optional<Rect> r;
                for (const auto& cell : c.cells)
                    if (const auto cr = rect_in_(d.snapshot, cell.id)) r = r ? detail::bounds(*r, *cr) : *cr;
                if (r) cols.push_back(*r);
            }
            double at = vertical ? box.y + box.height / 2 : box.x + box.width / 2;
            if (!cols.empty()) {
                if (drop.column < cols.size()) at = (vertical ? cols[drop.column].y : cols[drop.column].x) - gap / 2;
                else at = (vertical ? cols.back().y + cols.back().height : cols.back().x + cols.back().width) + gap / 2;
                if (drop.column == 0) at = std::max(at, vertical ? box.y + kLine / 2 : box.x + kLine / 2);
            }
            out.line = vertical ? Rect{box.x, at - kLine / 2, box.width, kLine} : Rect{at - kLine / 2, box.y, kLine, box.height};
            break;
        }
    }
    // The frame it would get: the drop made on a copy, arranged.
    auto copy = *this;
    copy.drop(d, drop, width);
    for (const auto& p : copy.arrange(output_, reserved_))
        if (p.id == d.id) out.box = p.rect;
    return out;
}

void Layout::drop(const DragSession& d, const Drop& drop, double width) {
    // Let go where it is seen: the camera stays (focusing it would pan).
    const auto camera = camera_;
    drop_(d, drop, width);
    camera_ = camera;
}

void Layout::drop_(const DragSession& d, const Drop& drop, double width) {
    using K = Drop::Kind;
    auto* w = window_(d.id);
    if (!w || drop.kind == K::none) return;
    if (drop.kind == K::floating) {
        if (!w->floating && space_of_(d.id)) toggle_floating(d.id);
        return;
    }
    const auto* own = placement_in_(d.snapshot, d.id);
    const Size one{own ? own->rect.width / std::max(0.01, own->zoom) : w->preferred.width,
                   own ? own->rect.height / std::max(0.01, own->zoom) : w->preferred.height};
    if (drop.kind == K::plane) {
        to_wuxian(d.id, Rect{drop.at.x, drop.at.y, one.width, one.height});
        focus(d.id);
        return;
    }
    auto* s = space_(drop.workspace);
    if (!s) return;
    if (space_of_(d.id) != s) {
        // Into this workspace from another, the plane or every workspace: in
        // each of its arrangements, then where the drop says.
        auto copy = *w;
        if (auto* from = space_of_(d.id)) {
            leave_(*from, d.id);
            std::erase_if(from->windows, [&](const Window& x) { return x.id == d.id; });
            if (from->focus == d.id) from->focus = from->windows.empty() ? std::nullopt : std::optional(from->windows.back().id);
        }
        std::erase_if(free_, [&](const Free& f) { return f.w.id == d.id; });
        std::erase_if(sticky_, [&](const Window& x) { return x.id == d.id; });
        fullscreen_.erase(d.id);
        copy.floating = copy.pinned = false;
        into_(*s, copy, std::nullopt);
        w = window_(d.id);
    } else if (w->floating && drop.kind != K::free) {
        toggle_floating(d.id);
        w = window_(d.id);
    }
    if (s->meta.rule == Rule::stack || drop.kind == K::free || drop.kind == K::snap) {
        if (drop.kind == K::snap) {
            w->snapped = true;
            w->snap_x = drop.snap_x;
            w->snap_y = drop.snap_y;
        } else if (drop.kind == K::free) {
            w->snapped = false;
            w->stacked = {drop.at.x, drop.at.y, one.width, one.height};
            w->stack_placed = true;
        }
        focus(d.id);
        return;
    }
    // Tiled. Where it goes is said by the window that is there now (its own
    // column may close up when it leaves).
    std::optional<Id> before;
    if (drop.kind == K::column && drop.column < s->columns.size())
        for (std::size_t c = drop.column; c < s->columns.size() && !before; ++c)
            for (const auto& cell : s->columns[c].cells)
                if (cell.id != d.id) { before = cell.id; break; }
    if (drop.kind == K::swap) {
        const auto a = find_(s->columns, d.id), b = find_(s->columns, drop.target);
        if (a && b) std::swap(s->columns[a->column].cells[a->cell].id, s->columns[b->column].cells[b->cell].id);
        focus(d.id);
        return;
    }
    erase_(s->columns, d.id);
    if (drop.kind == K::column) {
        std::size_t at = s->columns.size();
        if (before)
            if (const auto b = find_(s->columns, *before)) at = b->column;
        s->columns.insert(s->columns.begin() + static_cast<std::ptrdiff_t>(at), Column{.cells = {{.id = d.id}}, .width = width});
    } else if (drop.kind == K::above || drop.kind == K::below) {
        if (const auto t = find_(s->columns, drop.target)) {
            auto& col = s->columns[t->column];
            col.cells.insert(col.cells.begin() + static_cast<std::ptrdiff_t>(t->cell + (drop.kind == K::below ? 1 : 0)), Cell{.id = d.id});
            for (auto& c : col.cells) c.share = 0;
        } else {
            insert_column_(s->columns, d.id, std::nullopt);
        }
    } else {
        insert_column_(s->columns, d.id, std::nullopt);
    }
    focus(d.id);
}

// ── the gaps (the workspace view, a tiled workspace) ─────────────────

std::optional<std::size_t> Layout::gap_at(double sx, double sy) const {
    const auto& s = cur_();
    if (view_ != View::workspace || s.meta.rule != Rule::tile || s.columns.size() < 2) return std::nullopt;
    const Rect area = area_();
    if (sy < area.y || sy > area.y + area.height) return std::nullopt;
    for (std::size_t i = 0; i + 1 < s.columns.size(); ++i) {
        const auto a = rect_in_(last_, s.columns[i].cells.front().id), b = rect_in_(last_, s.columns[i + 1].cells.front().id);
        if (!a || !b) continue;
        const bool vertical = s.meta.flow == Flow::vertical;
        const double left = vertical ? a->y + a->height : a->x + a->width, right = vertical ? b->y : b->x;
        const double here = vertical ? sy : sx;
        if (here >= left - 3 && here <= right + 3) return i;
    }
    return std::nullopt;
}

void Layout::drag_gap(std::size_t column, double dx) {
    auto& s = cur_();
    if (s.meta.rule != Rule::tile || column + 1 >= s.columns.size()) return;
    const double total = std::max(1.0, tile_length_(s) - 2 * o_.outer_gap + o_.inner_gap);
    const double fa = tile_fraction_(s, column), fb = tile_fraction_(s, column + 1), df = dx / total;
    // Every column keeps the width it has now; these two trade.
    std::vector<double> now;
    for (std::size_t i = 0; i < s.columns.size(); ++i) now.push_back(tile_fraction_(s, i));
    for (std::size_t i = 0; i < s.columns.size(); ++i) s.columns[i].width = now[i];
    const double na = std::clamp(fa + df, 0.1, fa + fb - 0.1);
    s.columns[column].width = na;
    s.columns[column + 1].width = fa + fb - na;
}

std::optional<std::pair<std::size_t, std::size_t>> Layout::row_gap_at(double sx, double sy) const {
    const auto& s = cur_();
    if (view_ != View::workspace || s.meta.rule != Rule::tile) return std::nullopt;
    const bool vertical = s.meta.flow == Flow::vertical;
    for (std::size_t c = 0; c < s.columns.size(); ++c)
        for (std::size_t k = 0; k + 1 < s.columns[c].cells.size(); ++k) {
            const auto a = rect_in_(last_, s.columns[c].cells[k].id), b = rect_in_(last_, s.columns[c].cells[k + 1].id);
            if (!a || !b) continue;
            if (!vertical && sx >= a->x && sx < a->x + a->width && sy >= a->y + a->height - 3 && sy <= b->y + 3) return std::pair{c, k};
            if (vertical && sy >= a->y && sy < a->y + a->height && sx >= a->x + a->width - 3 && sx <= b->x + 3) return std::pair{c, k};
        }
    return std::nullopt;
}

void Layout::drag_row_gap(std::size_t column, std::size_t cell, double dy) {
    auto& s = cur_();
    if (s.meta.rule != Rule::tile || column >= s.columns.size()) return;
    auto& col = s.columns[column];
    if (cell + 1 >= col.cells.size()) return;
    const Rect area = area_();
    const double across = (s.meta.flow == Flow::vertical ? area.width : area.height) - 2 * o_.outer_gap;
    const auto hs = heights_(col, across, o_.inner_gap);
    double total = 0;
    for (const double h : hs) total += h;
    // Every cell keeps the share it has; these two trade.
    for (std::size_t k = 0; k < col.cells.size(); ++k) col.cells[k].share = hs[k] / std::max(1.0, total);
    const double pair = col.cells[cell].share + col.cells[cell + 1].share, d = dy / std::max(1.0, total);
    const double above = std::clamp(col.cells[cell].share + d, 0.08, pair - 0.08);
    col.cells[cell].share = above;
    col.cells[cell + 1].share = pair - above;
}

void Layout::even_widths() {
    for (auto& c : cur_().columns) c.width = 0;
}

void Layout::arrange_as(Pattern p, const std::vector<Id>& windows) {
    if (windows.empty()) return;
    auto* s = space_of_(windows.front());
    if (!s) return;
    std::vector<Id> ws;
    for (const auto id : windows)
        if (const auto* w = window_(id); w && !w->floating && space_of_(id) == s) ws.push_back(id);
    const std::size_t need = p == Pattern::halves || p == Pattern::two_thirds ? 2 : 3;
    if (ws.empty()) return;
    if (ws.size() > need) ws.resize(need);
    const Rect area = area_();
    if (s->meta.rule == Rule::tile) {
        // Where the first was, the columns of the layout in its place.
        std::size_t at = s->columns.size();
        if (const auto f = find_(s->columns, ws.front())) at = f->column;
        for (const auto id : ws) erase_(s->columns, id);
        at = std::min(at, s->columns.size());
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
        s->columns.insert(s->columns.begin() + static_cast<std::ptrdiff_t>(at), cols.begin(), cols.end());
        focus(ws.front());
        return;
    }
    // Stacked: halves and quarters by their frames.
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
}

}  // namespace xdesk::layout
