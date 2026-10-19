// The canvas (layout.cppm): an unbounded plane seen through a camera that
// pans and zooms; on it free windows and tiled groups -- two or more windows
// in columns, moved and zoomed as one. A free window is a group of one.
module xdesk.layout;

import std;

namespace xdesk::layout {

using detail::kMinZoom;
using detail::overlaps;

Layout::Group* Layout::group_(Space& s, Id id) {
    for (auto& g : s.groups)
        if (find_(g.columns, id)) return &g;
    return nullptr;
}

// The group's width from its columns' (a free window's is its own).
void Layout::refit_group_(Group& g) {
    double width = 0;
    for (auto& c : g.columns) {
        if (c.width <= 0) c.width = g.columns.size() == 1 && g.world.width > 0 ? g.world.width : 400;
        width += c.width;
    }
    width += o_.gap * static_cast<double>(g.columns.empty() ? 0 : g.columns.size() - 1);
    g.world.width = width;
}

std::vector<std::pair<Id, Rect>> Layout::group_rects_(const Group& g) const {
    std::vector<std::pair<Id, Rect>> out;
    double x = g.world.x;
    for (const auto& c : g.columns) {
        const double width = g.columns.size() == 1 ? g.world.width : c.width;
        const auto hs = heights_(c, g.world.height, o_.gap);
        double y = g.world.y;
        for (std::size_t k = 0; k < c.cells.size(); ++k) {
            out.push_back({c.cells[k].id, Rect{x, y, width, hs[k]}});
            y += hs[k] + o_.gap;
        }
        x += width + o_.gap;
    }
    return out;
}

std::optional<Rect> Layout::world_rect_(Space& s, Id id) {
    const auto* g = group_(s, id);
    if (!g || !g->placed) return std::nullopt;
    for (const auto& [wid, r] : group_rects_(*g))
        if (wid == id) return r;
    return std::nullopt;
}

// A new window on the canvas: right of the window focused when it came,
// else the middle of the view; then further right until it covers nothing.
void Layout::place_group_(Space& s, Group& g) {
    const Rect area = area_();
    const auto* w = window_(g.columns.front().cells.front().id);
    if (!w) return;
    const double width = std::min(w->preferred.width, area.width / s.zoom), height = std::min(w->preferred.height, area.height / s.zoom);
    Rect r { s.cx + (area.width / s.zoom - width) / 2, s.cy + (area.height / s.zoom - height) / 2, width, height };
    if (w->beside)
        if (const auto* anchor = window_(*w->beside); anchor && !anchor->floating)
            if (const auto a = world_rect_(s, anchor->id)) r = {a->x + a->width + 3 * o_.gap, a->y, width, height};
    for (int tries = 0; tries < 64; ++tries) {
        const auto hit = std::ranges::find_if(s.groups, [&](const Group& o) { return &o != &g && o.placed && overlaps(o.world, r); });
        if (hit == s.groups.end()) break;
        r.x = hit->world.x + hit->world.width + 3 * o_.gap;
    }
    g.world = r;
    g.columns.front().width = r.width;
    g.placed = true;
}

void Layout::group_with(Id id, Id target, Direction side) {
    auto* s = space_of_(id);
    if (!s || id == target || s != space_of_(target)) return;
    const auto* w = window_(id);
    const auto* t = window_(target);
    if (!w || !t || w->floating || t->floating) return;
    const auto mine = world_rect_(*s, id);
    const double width = std::max(120.0, mine ? mine->width : w->preferred.width);
    // Out of its own group (which closes up), into the target's.
    for (auto it = s->groups.begin(); it != s->groups.end(); ++it) {
        if (!find_(it->columns, id)) continue;
        erase_(it->columns, id);
        if (it->columns.empty()) s->groups.erase(it);
        else refit_group_(*it);
        break;
    }
    auto* g = group_(*s, target);
    if (!g) return;
    const auto at = *find_(g->columns, target);
    if (side == Direction::left || side == Direction::right) {
        const auto where = at.column + (side == Direction::right ? 1 : 0);
        g->columns.insert(g->columns.begin() + static_cast<std::ptrdiff_t>(where), Column{.cells = {{.id = id}}, .width = width});
        // The target stays where it is: a column on its left grows the group leftwards.
        if (side == Direction::left) g->world.x -= width + o_.gap;
    } else {
        auto& col = g->columns[at.column];
        const auto where = at.cell + (side == Direction::down ? 1 : 0);
        col.cells.insert(col.cells.begin() + static_cast<std::ptrdiff_t>(where), Cell{.id = id});
        for (auto& c : col.cells) c.share = 0;
    }
    refit_group_(*g);
    s->focus = id;
    focus(id);
}

void Layout::canvas_join_(Space& s) {
    if (!s.focus) return;
    const Id id = *s.focus;
    const auto* mine = group_(s, id);
    if (!mine || !mine->placed) return;
    // The nearest other group to the left (else the right), by its frame.
    std::vector<std::pair<Id, Rect>> others;
    for (const auto& g : s.groups)
        if (&g != mine && g.placed) others.push_back({g.columns.front().cells.front().id, g.world});
    auto pick = detail::nearest(mine->world, others, Direction::left);
    Direction side = Direction::right;
    if (!pick) {
        pick = detail::nearest(mine->world, others, Direction::right);
        side = Direction::left;
    }
    if (!pick) return;
    const auto* g = group_(s, *pick);
    const auto& col = side == Direction::right ? g->columns.back() : g->columns.front();
    group_with(id, col.cells[std::min(col.active, col.cells.size() - 1)].id, side);
}

void Layout::canvas_split_(Space& s) {
    if (!s.focus) return;
    const Id id = *s.focus;
    auto* g = group_(s, id);
    if (!g || (g->columns.size() == 1 && g->columns.front().cells.size() == 1)) return;
    const auto r = world_rect_(s, id);
    if (!r) return;
    erase_(g->columns, id);
    refit_group_(*g);
    // A free window beside the group it left.
    Rect at { g->world.x + g->world.width + 3 * o_.gap, r->y, r->width, r->height };
    s.groups.push_back({.world = at, .placed = true, .columns = {{.cells = {{.id = id}}, .width = r->width}}});
    auto& mine = s.groups.back();
    for (int tries = 0; tries < 64; ++tries) {
        const auto hit = std::ranges::find_if(s.groups, [&](const Group& o) { return &o != &mine && o.placed && overlaps(o.world, mine.world); });
        if (hit == s.groups.end()) break;
        mine.world.x = hit->world.x + hit->world.width + 3 * o_.gap;
    }
    reveal_(s, id);
}

// ── the camera ───────────────────────────────────────────────────────

void Layout::pan(double dx, double dy) {
    auto& s = space_();
    if (s.mode == Mode::tile) { s.scroll -= dx; return; }
    if (s.mode != Mode::canvas) return;
    s.cx -= dx / s.zoom;
    s.cy -= dy / s.zoom;
}

void Layout::zoom_at(double factor, double sx, double sy) {
    auto& s = space_();
    if (s.mode != Mode::canvas || factor <= 0) return;
    const Rect area = area_();
    const double wx = s.cx + (sx - area.x) / s.zoom, wy = s.cy + (sy - area.y) / s.zoom;
    s.zoom = std::clamp(s.zoom * factor, kMinZoom, 1.0);
    s.cx = wx - (sx - area.x) / s.zoom;
    s.cy = wy - (sy - area.y) / s.zoom;
}

double Layout::zoom() const { return mode() == Mode::canvas ? space_().zoom : 1.0; }
bool Layout::in_overview() const { return mode() == Mode::canvas && space_().overview; }
std::pair<double, double> Layout::camera() const {
    if (mode() != Mode::canvas) return {0, 0};
    return {space_().cx, space_().cy};
}

void Layout::overview(bool on) {
    auto& s = space_();
    if (s.mode != Mode::canvas || on == s.overview) return;
    if (on) {
        // Fitted by each arrangement, as windows come and go.
        s.overview = true;
        s.saved_zoom = s.zoom;
        s.saved_cx = s.cx;
        s.saved_cy = s.cy;
    } else {
        // Back to where it was, exactly: it is still there.
        s.overview = false;
        s.zoom = s.saved_zoom;
        s.cx = s.saved_cx;
        s.cy = s.saved_cy;
    }
}

void Layout::center(Id id) {
    auto* s = space_of_(id);
    const auto* w = window_(id);
    if (!s || !w || s->mode != Mode::canvas) return;
    s->overview = false;
    s->zoom = 1;
    s->focus = id;
    if (w->floating) return;
    if (const auto r = world_rect_(*s, id)) {
        const Rect area = area_();
        s->cx = r->x + r->width / 2 - area.width / 2;
        s->cy = r->y + r->height / 2 - area.height / 2;
    }
}

// The overview: everything on the canvas in view, as big as fits.
void Layout::fit_(Space& s, Rect area) {
    std::optional<Rect> box;
    for (const auto& g : s.groups) {
        if (!g.placed) continue;
        if (!box) { box = g.world; continue; }
        const double x0 = std::min(box->x, g.world.x), y0 = std::min(box->y, g.world.y);
        const double x1 = std::max(box->x + box->width, g.world.x + g.world.width);
        const double y1 = std::max(box->y + box->height, g.world.y + g.world.height);
        box = Rect{x0, y0, x1 - x0, y1 - y0};
    }
    if (!box) return;
    const double margin = 4 * o_.gap;
    s.zoom = std::clamp(std::min(area.width / (box->width + 2 * margin), area.height / (box->height + 2 * margin)), kMinZoom, 1.0);
    s.cx = box->x + box->width / 2 - area.width / 2 / s.zoom;
    s.cy = box->y + box->height / 2 - area.height / 2 / s.zoom;
}

// Keeps a window in view: the least pan that shows it (canvas; tile scrolls
// in its arrangement).
void Layout::reveal_(Space& s, Id id) {
    if (s.mode != Mode::canvas || s.overview) return;
    const auto* w = window_(id);
    if (!w || w->floating) return;
    const auto r = world_rect_(s, id);
    if (!r) return;
    const Rect area = area_();
    const double vw = area.width / s.zoom, vh = area.height / s.zoom, m = 2 * o_.gap;
    if (r->x - m < s.cx || r->width + 2 * m > vw) s.cx = r->x - m;
    else if (r->x + r->width + m > s.cx + vw) s.cx = r->x + r->width + m - vw;
    if (r->y - m < s.cy || r->height + 2 * m > vh) s.cy = r->y - m;
    else if (r->y + r->height + m > s.cy + vh) s.cy = r->y + r->height + m - vh;
}

std::vector<Placement> Layout::canvas_(Space& s, Rect area) {
    std::vector<Placement> out;
    // New windows first, in the order they came (each beside its anchor);
    // the focused one, if new, brought into view.
    bool focus_new = false;
    for (auto& g : s.groups) {
        if (g.placed) continue;
        place_group_(s, g);
        if (s.focus && find_(g.columns, *s.focus)) focus_new = true;
    }
    if (s.overview) fit_(s, area);
    else if (focus_new) reveal_(s, *s.focus);
    for (const auto& g : s.groups)
        for (const auto& [id, w] : group_rects_(g)) {
            const Rect r { area.x + (w.x - s.cx) * s.zoom, area.y + (w.y - s.cy) * s.zoom, w.width * s.zoom, w.height * s.zoom };
            out.push_back({.id = id, .rect = r, .visible = overlaps(r, area), .focused = s.focus == id});
        }
    return out;
}

}  // namespace xdesk::layout
