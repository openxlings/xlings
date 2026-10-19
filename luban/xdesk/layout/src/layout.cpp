// What every preset shares: the windows of a workspace and their place in
// each arrangement, switching presets, focus, floating, workspaces, and the
// arrangement handed to the compositor (layout.cppm). The presets' own rules
// are tile.cpp, stack.cpp and canvas.cpp.
module xdesk.layout;

import std;

namespace xdesk::layout {

using detail::inset;
using detail::overlaps;

namespace {

std::size_t index_(Mode m) { return static_cast<std::size_t>(m); }

}  // namespace

Mode next(Mode m) {
    switch (m) {
        case Mode::tile: return Mode::stack;
        case Mode::stack: return Mode::canvas;
        case Mode::canvas: return Mode::tile;
    }
    return Mode::tile;
}

std::string_view name(Mode m) {
    switch (m) {
        case Mode::tile: return "tile";
        case Mode::stack: return "stack";
        case Mode::canvas: return "canvas";
    }
    return "tile";
}

std::optional<Mode> mode_named(std::string_view n) {
    for (const auto m : {Mode::tile, Mode::stack, Mode::canvas})
        if (name(m) == n) return m;
    return std::nullopt;
}

std::string_view name(Flow f) { return f == Flow::vertical ? "vertical" : "horizontal"; }

std::optional<Flow> flow_named(std::string_view n) {
    for (const auto f : {Flow::horizontal, Flow::vertical})
        if (name(f) == n) return f;
    return std::nullopt;
}

Layout::Layout(Options o) : o_(o), spaces_(static_cast<std::size_t>(std::max(1, o.workspaces))) {
    for (auto& s : spaces_) s.mode = o.mode, s.flow = o.flow;
}

Flow Layout::flow() const { return space_().flow; }
void Layout::set_flow(Flow f) { space_().flow = f; }

void Layout::set_spacing(double inner_gap, double outer_gap, double min_column_width) {
    o_.inner_gap = std::clamp(inner_gap, 0.0, 64.0);
    o_.outer_gap = std::clamp(outer_gap, 0.0, 64.0);
    o_.min_column_width = std::clamp(min_column_width, 120.0, 4096.0);
}

Layout::Window* Layout::window_(Id id) {
    for (auto& s : spaces_)
        for (auto& w : s.windows)
            if (w.id == id) return &w;
    for (auto& w : sticky_)
        if (w.id == id) return &w;
    return nullptr;
}

const Layout::Window* Layout::window_(Id id) const { return const_cast<Layout*>(this)->window_(id); }

Layout::Space* Layout::space_of_(Id id) {
    for (auto& s : spaces_)
        for (const auto& w : s.windows)
            if (w.id == id) return &s;
    return nullptr;
}

Rect Layout::area_() const { return inset(output_, reserved_); }

bool Layout::contains(Id id) const { return window_(id) != nullptr; }

void Layout::toggle_pinned(Id id) {
    auto* w = window_(id);
    if (!w) return;
    if (w->pinned) { w->pinned = false; return; }
    if (!w->floating) toggle_floating(id);
    if (auto* now = window_(id)) now->pinned = true;
}

bool Layout::pinned(Id id) const {
    const auto* w = window_(id);
    return w && w->pinned;
}

void Layout::toggle_sticky(Id id) {
    if (auto it = std::ranges::find(sticky_, id, &Window::id); it != sticky_.end()) {
        // Back to the workspace shown, floating where it is.
        auto w = *it;
        sticky_.erase(it);
        space_().windows.push_back(w);
        space_().focus = id;
        return;
    }
    auto* s = space_of_(id);
    if (!s) return;
    if (!floating(id)) toggle_floating(id);
    auto* w = window_(id);
    if (!w) return;
    auto copy = *w;
    std::erase_if(s->windows, [&](const Window& x) { return x.id == id; });
    if (s->focus == id) s->focus = s->windows.empty() ? std::nullopt : std::optional(s->windows.back().id);
    sticky_.push_back(copy);
    space_().focus = id;
}

bool Layout::sticky(Id id) const { return std::ranges::contains(sticky_, id, &Window::id); }
bool Layout::floating(Id id) const {
    const auto* w = window_(id);
    return w && w->floating;
}

std::optional<int> Layout::workspace_of(Id id) const {
    for (std::size_t i = 0; i < spaces_.size(); ++i)
        for (const auto& w : spaces_[i].windows)
            if (w.id == id) return static_cast<int>(i);
    // On every workspace: on the one shown.
    if (sticky(id)) return shown_;
    return std::nullopt;
}

// ── presets ──────────────────────────────────────────────────────────

Mode Layout::mode() const { return space_().mode; }

Mode Layout::mode_of(int workspace) const {
    if (workspace < 0 || workspace >= workspaces()) return o_.mode;
    return spaces_[static_cast<std::size_t>(workspace)].mode;
}

void Layout::preset(int workspace, Mode m) {
    if (workspace < 0 || workspace >= workspaces()) return;
    auto& s = spaces_[static_cast<std::size_t>(workspace)];
    if (!s.windows.empty()) return;
    s.mode = m;
}

void Layout::set_mode(Mode m) {
    auto& s = space_();
    if (s.mode == m) return;
    // What is on the screen now is where a preset shown for the first time starts.
    if (last_space_ != shown_) arrange(output_, reserved_);
    s.shown[index_(s.mode)] = true;
    if (!s.shown[index_(m)]) convert_(s, m);
    s.mode = m;
    s.shown[index_(m)] = true;
}

void Layout::convert_(Space& s, Mode to) {
    const Rect area = area_();
    auto on_screen = [&](Id id) -> std::optional<Rect> {
        for (const auto& p : last_)
            if (p.id == id && !p.floating) return p.rect;
        return std::nullopt;
    };
    if (to == Mode::tile) {
        std::vector<Column> cols;
        if (s.mode == Mode::canvas) {
            // Left to right; a group's columns stay columns.
            std::vector<const Group*> gs;
            for (const auto& g : s.groups) gs.push_back(&g);
            std::ranges::stable_sort(gs, {}, [](const Group* g) { return g->placed ? g->world.x : std::numeric_limits<double>::max(); });
            for (const auto* g : gs)
                for (auto c : g->columns) {
                    c.width = 0;
                    cols.push_back(std::move(c));
                }
        } else {
            std::vector<std::pair<double, Id>> xs;
            for (const auto& w : s.windows)
                if (!w.floating) xs.push_back({on_screen(w.id).value_or(Rect{std::numeric_limits<double>::max()}).x, w.id});
            std::ranges::stable_sort(xs, {}, &std::pair<double, Id>::first);
            for (const auto& [x, id] : xs) cols.push_back({.cells = {{.id = id}}});
        }
        s.columns = std::move(cols);
        s.scroll = 0;
        return;
    }
    if (to == Mode::canvas) {
        // The view where the screen was: what is placed stays where it is seen.
        s.cx = 0;
        s.cy = 0;
        s.zoom = 1;
        s.overview = false;
        std::vector<Group> groups;
        if (s.mode == Mode::tile && s.columns.size() > 1) {
            Group g;
            std::optional<Rect> box;
            for (const auto& c : s.columns) {
                auto col = c;
                const auto r = c.cells.empty() ? std::nullopt : on_screen(c.cells.front().id);
                col.width = r ? r->width : 400;
                for (const auto& cell : c.cells)
                    if (const auto cr = on_screen(cell.id)) {
                        const Rect wr { cr->x - area.x, cr->y - area.y, cr->width, cr->height };
                        if (!box) box = wr;
                        else {
                            const double x0 = std::min(box->x, wr.x), y0 = std::min(box->y, wr.y);
                            box = Rect{x0, y0, std::max(box->x + box->width, wr.x + wr.width) - x0,
                                       std::max(box->y + box->height, wr.y + wr.height) - y0};
                        }
                    }
                g.columns.push_back(std::move(col));
            }
            if (box) {
                g.world = *box;
                g.placed = true;
            }
            refit_group_(g);
            groups.push_back(std::move(g));
        } else {
            for (const auto& w : s.windows) {
                if (w.floating) continue;
                Group g { .columns = {{.cells = {{.id = w.id}}}} };
                if (const auto r = on_screen(w.id)) {
                    g.world = {r->x - area.x, r->y - area.y, r->width, r->height};
                    g.placed = true;
                    g.columns.front().width = r->width;
                }
                groups.push_back(std::move(g));
            }
        }
        s.groups = std::move(groups);
        return;
    }
    // To stack: each window where it is seen, kept on the output.
    for (auto& w : s.windows) {
        if (w.floating) continue;
        w.snapped = false;
        if (const auto r = on_screen(w.id)) {
            w.stacked = *r;
            w.stack_placed = true;
        } else {
            w.stack_placed = false;
        }
    }
    if (s.focus) {
        std::erase(s.order, *s.focus);
        if (const auto* w = window_(*s.focus); w && !w->floating) s.order.push_back(*s.focus);
    }
}

// ── membership ───────────────────────────────────────────────────────

// A window that is not floating has a place in every arrangement of its
// workspace: a column (tile), a group (canvas), a place in the order (stack).
void Layout::enter_(Space& s, Window& w, std::optional<Id> after) {
    if (w.floating) return;
    insert_column_(s.columns, w.id, after);
    s.groups.push_back({.columns = {{.cells = {{.id = w.id}}}}});
    s.order.push_back(w.id);
}

void Layout::leave_(Space& s, Id id) {
    erase_(s.columns, id);
    for (auto it = s.groups.begin(); it != s.groups.end(); ++it) {
        if (!find_(it->columns, id)) continue;
        erase_(it->columns, id);
        if (it->columns.empty()) s.groups.erase(it);
        else refit_group_(*it);
        break;
    }
    std::erase(s.order, id);
}

void Layout::add(Id id, Size preferred, bool floating) {
    if (contains(id)) return;
    auto& s = space_();
    // Placed by the next arrangement, which knows the output.
    s.windows.push_back({.id = id, .preferred = preferred, .floating = floating, .beside = s.focus});
    enter_(s, s.windows.back(), s.focus);
    s.focus = id;
    reveal_(s, id);
}

void Layout::remove(Id id) {
    if (std::erase_if(sticky_, [&](const Window& w) { return w.id == id; }) > 0) {
        if (space_().focus == id) space_().focus.reset();
        return;
    }
    auto* s = space_of_(id);
    if (!s) return;
    // The focus goes to a neighbour: in the column, else the next column (tile).
    std::optional<Id> next;
    if (s->mode == Mode::tile)
        if (const auto at = find_(s->columns, id)) {
            const auto& col = s->columns[at->column];
            if (col.cells.size() > 1) next = col.cells[at->cell + 1 < col.cells.size() ? at->cell + 1 : at->cell - 1].id;
            else if (s->columns.size() > 1) {
                const auto j = at->column + 1 < s->columns.size() ? at->column + 1 : at->column - 1;
                const auto& n = s->columns[j];
                next = n.cells[std::min(n.active, n.cells.size() - 1)].id;
            }
        }
    leave_(*s, id);
    std::erase_if(s->windows, [&](const Window& w) { return w.id == id; });
    if (s->focus == id) {
        if (!next && s->mode == Mode::stack && !s->order.empty()) next = s->order.back();
        if (!next && !s->windows.empty()) next = s->windows.back().id;
        s->focus = next;
        if (next) focus(*next);
    }
}

std::vector<Id> Layout::group_of(Id id) const {
    auto* self = const_cast<Layout*>(this);
    auto* s = self->space_of_(id);
    if (!s) return {};
    const std::vector<Column>* cols = nullptr;
    if (s->mode == Mode::canvas) {
        if (const auto* g = self->group_(*s, id)) cols = &g->columns;
    } else if (s->mode == Mode::tile) {
        if (const auto at = find_(s->columns, id)) {
            std::vector<Id> out;
            for (const auto& c : s->columns[at->column].cells) out.push_back(c.id);
            return out;
        }
    }
    if (!cols) return {id};
    std::vector<Id> out;
    for (const auto& c : *cols)
        for (const auto& cell : c.cells) out.push_back(cell.id);
    return out;
}

// ── focus and moving ─────────────────────────────────────────────────

void Layout::focus(Id id) {
    if (sticky(id)) { space_().focus = id; return; }
    auto* s = space_of_(id);
    if (!s) return;
    s->focus = id;
    if (const auto at = find_(s->columns, id)) s->columns[at->column].active = at->cell;
    s->scrolled = false;   // the focus moved: it is shown again
    if (auto* g = group_(*s, id))
        if (const auto at = find_(g->columns, id)) g->columns[at->column].active = at->cell;
    if (const auto* w = window_(id); w && !w->floating) {
        std::erase(s->order, id);
        s->order.push_back(id);
    }
    reveal_(*s, id);
}

std::optional<Id> Layout::focused() const { return space_().focus; }

void Layout::move_focus(Direction d) {
    auto& s = space_();
    if (s.windows.empty()) return;
    if (!s.focus) { focus(s.windows.back().id); return; }
    const auto* current = window_(*s.focus);
    if (!current) return;
    if (s.mode == Mode::tile) {
        d = turned_(s, d);
        const auto at = find_(s.columns, *s.focus);
        if (!at) {
            // A floating window: to the columns' first or last.
            if (s.columns.empty() || (d != Direction::left && d != Direction::right)) return;
            const auto& c = d == Direction::right ? s.columns.front() : s.columns.back();
            focus(c.cells[std::min(c.active, c.cells.size() - 1)].id);
            return;
        }
        if (d == Direction::up || d == Direction::down) {
            const auto& col = s.columns[at->column];
            const auto j = static_cast<std::ptrdiff_t>(at->cell) + (d == Direction::down ? 1 : -1);
            if (j >= 0 && j < static_cast<std::ptrdiff_t>(col.cells.size())) focus(col.cells[static_cast<std::size_t>(j)].id);
            return;
        }
        const auto j = static_cast<std::ptrdiff_t>(at->column) + (d == Direction::right ? 1 : -1);
        if (j < 0 || j >= static_cast<std::ptrdiff_t>(s.columns.size())) return;
        const auto& c = s.columns[static_cast<std::size_t>(j)];
        focus(c.cells[std::min(c.active, c.cells.size() - 1)].id);
        return;
    }
    // Stack and canvas: the nearest that way, by where each window is.
    std::vector<std::pair<Id, Rect>> rs;
    std::optional<Rect> from;
    const Rect area = area_();
    for (const auto& w : s.windows) {
        std::optional<Rect> r;
        if (w.floating) r = w.placed ? std::optional(w.free) : std::nullopt;
        else if (s.mode == Mode::stack) r = stack_rect_(w, area);
        else r = world_rect_(s, w.id);
        if (!r) continue;
        if (w.id == current->id) from = r;
        else if (!w.floating) rs.push_back({w.id, *r});
    }
    if (!from) return;
    if (const auto best = detail::nearest(*from, rs, d)) focus(*best);
}

void Layout::move(Direction d) {
    auto& s = space_();
    if (!s.focus) return;
    auto* w = window_(*s.focus);
    if (!w) return;
    const Rect area = area_();
    const double sx = d == Direction::left ? -1 : d == Direction::right ? 1 : 0;
    const double sy = d == Direction::up ? -1 : d == Direction::down ? 1 : 0;
    if (w->floating) {
        w->free.x += sx * area.width / 4;
        w->free.y += sy * area.height / 4;
        return;
    }
    switch (s.mode) {
        case Mode::tile: tile_move_(s, turned_(s, d)); return;
        case Mode::stack: stack_move_(*w, d); return;
        case Mode::canvas:
            if (auto* g = group_(s, w->id)) {
                g->world.x += sx * area.width / 4 / s.zoom;
                g->world.y += sy * area.height / 4 / s.zoom;
                reveal_(s, w->id);
            }
            return;
    }
}

void Layout::join() {
    auto& s = space_();
    if (s.mode == Mode::tile) tile_join_(s);
    else if (s.mode == Mode::canvas) canvas_join_(s);
}

void Layout::split() {
    auto& s = space_();
    if (s.mode == Mode::tile) tile_split_(s);
    else if (s.mode == Mode::canvas) canvas_split_(s);
}

void Layout::toggle_floating(Id id) {
    auto* s = space_of_(id);
    auto* w = window_(id);
    if (!s || !w) return;
    const Rect area = area_();
    if (!w->floating) {
        // Where it is on screen is where it floats; a tiled one is centred.
        std::optional<Rect> seen;
        if (s->mode != Mode::tile && last_space_ == shown_ && s == &space_())
            for (const auto& p : last_)
                if (p.id == id) seen = p.rect;
        leave_(*s, id);
        w->floating = true;
        if (seen) {
            w->free = *seen;
            w->placed = true;
        } else {
            place_new_float_(*w);
        }
    } else {
        w->floating = false;
        const auto after = s->focus && *s->focus != id ? s->focus : std::nullopt;
        enter_(*s, *w, after);
        // Floating it was on the screen: there it is in every arrangement.
        if (w->placed) {
            w->stacked = w->free;
            w->stack_placed = true;
            w->snapped = false;
            auto& g = s->groups.back();
            g.world = {s->cx + (w->free.x - area.x) / s->zoom, s->cy + (w->free.y - area.y) / s->zoom, w->free.width / s->zoom,
                       w->free.height / s->zoom};
            g.columns.front().width = g.world.width;
            g.placed = true;
        }
    }
    s->focus = id;
    focus(id);
}

void Layout::toggle_maximized(Id id) {
    auto* w = window_(id);
    if (!w || w->floating) return;
    if (auto* s = space_of_(id); s && s->mode == Mode::tile) {
        if (const auto at = find_(s->columns, id)) {
            auto& col = s->columns[at->column];
            if (col.width >= 1.0) col.width = col.restore;
            else { col.restore = col.width; col.width = 1.0; }
            reveal_(*s, id);
        }
        return;
    }
    if (w->snapped && w->snap_x == 0 && w->snap_y == 0) {
        w->snapped = false;
    } else {
        w->snapped = true;
        w->snap_x = w->snap_y = 0;
    }
}

bool Layout::maximized(Id id) const {
    const auto* w = window_(id);
    if (!w || w->floating) return false;
    auto* self = const_cast<Layout*>(this);
    if (auto* s = self->space_of_(id); s && s->mode == Mode::tile) {
        const auto at = find_(s->columns, id);
        return at && s->columns[at->column].width >= 1.0;
    }
    return w->snapped && w->snap_x == 0 && w->snap_y == 0;
}

void Layout::place(Id id, Rect screen) {
    if (auto it = std::ranges::find(sticky_, id, &Window::id); it != sticky_.end()) {
        it->free = screen;
        it->placed = true;
        return;
    }
    auto* s = space_of_(id);
    auto* w = window_(id);
    if (!s || !w) return;
    const Rect area = area_();
    if (w->floating) {
        w->free = screen;
        w->placed = true;
        return;
    }
    if (s->mode == Mode::stack) {
        w->stacked = screen;
        w->stack_placed = true;
        w->snapped = false;
        return;
    }
    if (s->mode != Mode::canvas) return;
    auto* g = group_(*s, id);
    if (!g) return;
    const Rect world { s->cx + (screen.x - area.x) / s->zoom, s->cy + (screen.y - area.y) / s->zoom, screen.width / s->zoom,
                       screen.height / s->zoom };
    if (g->columns.size() == 1 && g->columns.front().cells.size() == 1) {
        g->world = world;
        g->columns.front().width = world.width;
    } else if (const auto now = world_rect_(*s, id)) {
        // In a group: the group moves with it.
        g->world.x += world.x - now->x;
        g->world.y += world.y - now->y;
    }
    g->placed = true;
}

void Layout::cycle_width() {
    auto& s = space_();
    if (!s.focus) return;
    static constexpr std::array<double, 4> kSteps { 1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0 };
    auto next_of = [](double now) {
        for (double f : kSteps)
            if (f > now + 1e-6) return f;
        return kSteps.front();
    };
    if (s.mode == Mode::tile) {
        const auto at = find_(s.columns, *s.focus);
        if (!at) return;
        auto& col = s.columns[at->column];
        col.width = next_of(tile_fraction_(s, at->column));
        reveal_(s, *s.focus);
    } else if (s.mode == Mode::canvas) {
        auto* g = group_(s, *s.focus);
        if (!g || g->columns.size() < 2) return;
        const auto at = find_(g->columns, *s.focus);
        const double full = area_().width;
        auto& col = g->columns[at->column];
        col.width = next_of(col.width / std::max(1.0, full)) * full;
        refit_group_(*g);
    }
}

void Layout::cycle_height() {
    auto& s = space_();
    if (!s.focus) return;
    std::vector<Column>* cols = s.mode == Mode::tile ? &s.columns : nullptr;
    if (s.mode == Mode::canvas)
        if (auto* g = group_(s, *s.focus)) cols = &g->columns;
    if (!cols) return;
    const auto at = find_(*cols, *s.focus);
    if (!at) return;
    auto& col = (*cols)[at->column];
    if (col.cells.size() < 2) return;
    // An equal part, 2/3, 1/3.
    auto& mine = col.cells[at->cell].share;
    const double next = mine == 0 ? 2.0 / 3 : mine > 0.5 ? 1.0 / 3 : 0;
    for (auto& c : col.cells) c.share = 0;
    mine = next;
}

void Layout::resize(double step) {
    auto& s = space_();
    if (!s.focus) return;
    const Rect area = area_();
    if (s.mode == Mode::tile) {
        const auto at = find_(s.columns, *s.focus);
        if (!at) return;
        auto& col = s.columns[at->column];
        col.width = std::clamp(tile_fraction_(s, at->column) + step, 0.1, 1.0);
        reveal_(s, *s.focus);
        return;
    }
    if (s.mode == Mode::canvas) {
        auto* g = group_(s, *s.focus);
        if (!g) return;
        const auto at = find_(g->columns, *s.focus);
        auto& col = g->columns[at->column];
        col.width = std::max(120.0, col.width + step * area.width);
        if (g->columns.size() == 1 && col.cells.size() == 1) g->world.height = std::max(80.0, g->world.height + step * area.height);
        refit_group_(*g);
        return;
    }
    auto* w = window_(*s.focus);
    if (!w || w->floating) return;
    Rect r = stack_rect_(*w, area);
    const double dw = step * area.width, dh = step * area.height;
    r = {r.x - dw / 2, r.y - dh / 2, std::max(120.0, r.width + dw), std::max(80.0, r.height + dh)};
    w->stacked = r;
    w->stack_placed = true;
    w->snapped = false;
}

// ── workspaces ───────────────────────────────────────────────────────

void Layout::show(int n) {
    if (n >= 0 && n < workspaces()) shown_ = n;
}

void Layout::send_to(Id id, int n) {
    if (n < 0 || n >= workspaces()) return;
    auto* from = space_of_(id);
    auto& to = spaces_[static_cast<std::size_t>(n)];
    if (!from || from == &to) return;
    auto w = *window_(id);
    remove(id);
    w.beside = to.focus;
    w.stack_placed = false;
    w.snapped = false;
    to.windows.push_back(w);
    enter_(to, to.windows.back(), std::nullopt);
    if (!to.focus) to.focus = id;
}

// ── arranging ────────────────────────────────────────────────────────

void Layout::place_new_float_(Window& w) {
    // Floating: in the middle of the output, as big as it asked (or fits).
    const Rect area = area_();
    const double width = std::min(w.preferred.width, area.width - 2 * o_.inner_gap);
    const double height = std::min(w.preferred.height, area.height - 2 * o_.inner_gap);
    w.free = {area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height};
    w.placed = true;
}

std::vector<Placement> Layout::arrange(Size output, Insets reserved) {
    output_ = output;
    reserved_ = reserved;
    auto& s = space_();
    s.shown[index_(s.mode)] = true;
    const Rect area = area_();
    std::vector<Placement> out;
    switch (s.mode) {
        case Mode::tile: out = tile_(s, area); break;
        case Mode::stack: out = stack_(s, area); break;
        case Mode::canvas: out = canvas_(s, area); break;
    }
    // Floating windows above, then those on every workspace, then those
    // always on top (design 2026-10-19 §18.2).
    for (const bool top : {false, true}) {
        for (auto* list : {&s.windows, &sticky_})
            for (auto& w : *list) {
                if (!w.floating || w.pinned != top) continue;
                if (!w.placed) place_new_float_(w);
                out.push_back({.id = w.id, .rect = w.free, .visible = overlaps(w.free, area), .focused = s.focus == w.id,
                               .floating = true, .pinned = w.pinned});
            }
    }
    last_ = out;
    last_space_ = shown_;
    return out;
}

std::optional<Id> Layout::at(double sx, double sy) const {
    for (auto it = last_.rbegin(); it != last_.rend(); ++it)
        if (it->visible && it->rect.contains(sx, sy)) return it->id;
    return std::nullopt;
}

// ── Spring ───────────────────────────────────────────────────────────

void Spring::step(double seconds, double stiffness) {
    if (seconds <= 0) return;
    // x(t) = (x0 + (v0 + w x0) t) e^(-w t), its derivative for the speed.
    const double w = std::sqrt(stiffness), x0 = value - target, v0 = velocity;
    const double e = std::exp(-w * seconds);
    value = target + (x0 + (v0 + w * x0) * seconds) * e;
    velocity = (v0 - w * (v0 + w * x0) * seconds) * e;
    if (settled()) snap();
}

bool Spring::settled() const { return std::abs(value - target) < 0.5 && std::abs(velocity) < 5; }

}  // namespace xdesk::layout
