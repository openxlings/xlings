// What every workspace shares: its windows and their place in each rule's
// arrangement, switching rules, focus, floating, the workspaces themselves
// and windows going between them and the plane (layout.cppm). The rules'
// own arrangements are tile.cpp and stack.cpp; the plane, the camera and
// the two views wuxian.cpp; dragging drag.cpp; the state state.cpp.
module xdesk.layout;

import std;
import openwx;

namespace xdesk::layout {

using detail::inset;
using detail::overlaps;

namespace {

std::size_t index_(Rule r) { return r == Rule::stack ? 1 : 0; }

}  // namespace

Rule other(Rule r) { return r == Rule::tile ? Rule::stack : Rule::tile; }

Layout::Layout(Options o) : o_(o) {
    o_.max_workspaces = std::clamp(o_.max_workspaces, 1, 99);
    Space s;
    s.meta = {.id = 1, .color = openwx::color_for(1), .rule = o_.rule, .flow = o_.flow};
    spaces_.push_back(std::move(s));
}

void Layout::set_spacing(double inner_gap, double outer_gap, double min_column_width) {
    o_.inner_gap = std::clamp(inner_gap, 0.0, 64.0);
    o_.outer_gap = std::clamp(outer_gap, 0.0, 64.0);
    o_.min_column_width = std::clamp(min_column_width, 120.0, 4096.0);
}

// ── finding things ───────────────────────────────────────────────────

Layout::Space* Layout::space_(int id) {
    for (auto& s : spaces_)
        if (s.meta.id == id) return &s;
    return nullptr;
}
const Layout::Space* Layout::space_(int id) const { return const_cast<Layout*>(this)->space_(id); }

Layout::Window* Layout::window_(Id id) {
    for (auto& s : spaces_)
        for (auto& w : s.windows)
            if (w.id == id) return &w;
    for (auto& f : free_)
        if (f.w.id == id) return &f.w;
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
const Layout::Space* Layout::space_of_(Id id) const { return const_cast<Layout*>(this)->space_of_(id); }

Layout::Free* Layout::free_of_(Id id) {
    for (auto& f : free_)
        if (f.w.id == id) return &f;
    return nullptr;
}
const Layout::Free* Layout::free_of_(Id id) const { return const_cast<Layout*>(this)->free_of_(id); }

Rect Layout::area_() const { return inset(output_, reserved_); }

bool Layout::contains(Id id) const { return window_(id) != nullptr; }

std::optional<int> Layout::workspace_of(Id id) const {
    if (const auto* s = space_of_(id)) return s->meta.id;
    // On every workspace: on the one shown.
    if (sticky(id)) return current_;
    return std::nullopt;
}

bool Layout::is_free(Id id) const { return free_of_(id) != nullptr; }

std::vector<openwx::Layer> Layout::layers_() const {
    std::vector<openwx::Layer> out;
    for (const auto& s : spaces_)
        out.push_back({.kind = openwx::Layer::Kind::workspace, .workspace = s.meta.id, .world = layer_world_(s), .z = s.meta.z});
    for (const auto& f : free_) out.push_back({.kind = openwx::Layer::Kind::window, .window = f.w.id, .world = layer_world_(f), .z = f.z});
    return out;
}

Rect Layout::layer_world_(const Space& s) const {
    const Rect r = world_of_(s);
    if (!s.meta.collapsed) return r;
    const double z = camera_.zoom > 0 ? camera_.zoom : 1;
    return {r.x, r.y, std::min(r.width, detail::kLabelStrip / z), o_.title / z};
}

Rect Layout::layer_world_(const Free& f) const {
    if (!f.collapsed) return f.world;
    const double z = camera_.zoom > 0 ? camera_.zoom : 1;
    return {f.world.x, f.world.y, f.world.width, std::min(f.world.height, o_.title / z)};
}

void Layout::toggle_collapsed(Id id) {
    if (auto* f = free_of_(id)) f->collapsed = !f->collapsed;
}

void Layout::toggle_workspace_collapsed(int id) {
    if (auto* s = space_(id)) s->meta.collapsed = !s->meta.collapsed;
}

bool Layout::collapsed(Id id) const {
    const auto* f = free_of_(id);
    return f && f->collapsed;
}

bool Layout::workspace_collapsed(int id) const {
    const auto* s = space_(id);
    return s && s->meta.collapsed;
}

int Layout::top_z_() const {
    const auto ls = layers_();
    return openwx::top_z(ls);
}

// Right of everything on the plane, a gap from it, at the top of the first.
Point Layout::right_of_all_() const {
    double x = 0, y = 0;
    bool any = false;
    for (const auto& l : layers_()) {
        if (!any || l.world.x + l.world.width > x) x = l.world.x + l.world.width;
        if (!any) y = l.world.y;
        any = true;
    }
    return {any ? x + 160 : 0, y};
}

// ── workspaces ───────────────────────────────────────────────────────

std::vector<int> Layout::workspaces() const {
    std::vector<int> out;
    for (const auto& s : spaces_) out.push_back(s.meta.id);
    return out;
}

const openwx::Workspace* Layout::workspace_info(int id) const {
    const auto* s = space_(id);
    return s ? &s->meta : nullptr;
}

std::vector<Id> Layout::windows_in(int id) const {
    std::vector<Id> out;
    if (const auto* s = space_(id))
        for (const auto& w : s->windows) out.push_back(w.id);
    return out;
}

int Layout::add_workspace(Rule r, std::optional<int> id, std::optional<Point> world_at) {
    std::vector<openwx::Workspace> metas;
    for (const auto& s : spaces_) metas.push_back(s.meta);
    int n = 0;
    if (id) {
        if (*id < 1 || *id > o_.max_workspaces || space_(*id)) return 0;
        n = *id;
    } else if (const auto free = openwx::next_id(metas, o_.max_workspaces)) {
        n = *free;
    } else {
        return 0;
    }
    Space s;
    s.meta = {.id = n, .color = openwx::color_for(n), .rule = r, .flow = o_.flow, .origin = world_at.value_or(right_of_all_()), .z = top_z_()};
    const auto at = std::ranges::find_if(spaces_, [&](const Space& o) { return o.meta.id > n; });
    spaces_.insert(at, std::move(s));
    return n;
}

bool Layout::remove_workspace(int id) {
    if (spaces_.size() <= 1 || !space_(id)) return false;
    // Its windows free on the plane, where they are on it now.
    for (const auto wid : windows_in(id)) to_wuxian(wid);
    std::erase_if(spaces_, [&](const Space& s) { return s.meta.id == id; });
    if (current_ == id) {
        // The one before it, else the first.
        int to = spaces_.front().meta.id;
        for (const auto& s : spaces_)
            if (s.meta.id < id) to = s.meta.id;
        current_ = to;
    }
    return true;
}

void Layout::rename(int id, std::string name) {
    if (auto* s = space_(id)) {
        s->meta.name = std::move(name);
        s->implicit = false;
    }
}

void Layout::set_rule(int id, Rule r) {
    auto* s = space_(id);
    if (!s || s->meta.rule == r) return;
    // What is on the screen now is where a rule shown for the first time starts.
    if (s->meta.id == current_ && view_ == View::workspace) arrange(output_, reserved_);
    s->shown[index_(s->meta.rule)] = true;
    if (!s->shown[index_(r)]) convert_(*s, r);
    s->meta.rule = r;
    s->shown[index_(r)] = true;
}

void Layout::set_color(int id, std::uint32_t rgb) {
    if (auto* s = space_(id)) s->meta.color = rgb & 0xffffff;
}

void Layout::set_flow(int id, Flow f) {
    if (auto* s = space_(id)) s->meta.flow = f;
}

void Layout::set_scale(int id, double scale) {
    if (auto* s = space_(id)) s->meta.scale = std::clamp(scale, openwx::kMinScale, openwx::kMaxScale);
}

void Layout::move_workspace(int id, Point world) {
    if (auto* s = space_(id)) s->meta.origin = world;
}

void Layout::raise(int id) {
    auto* s = space_(id);
    if (!s) return;
    const int z = top_z_();
    if (s->meta.z < z - 1 || std::ranges::count_if(layers_(), [&](const openwx::Layer& l) { return l.z == s->meta.z; }) > 1) s->meta.z = z;
}

void Layout::line_up() {
    if (spaces_.empty()) return;
    const auto top = spaces_.front().meta.origin;
    double x = top.x;
    for (auto& s : spaces_) {
        s.meta.origin = {x, top.y};
        x += world_of_(s).width + 160;
    }
}

Rule Layout::rule() const { return cur_().meta.rule; }
Flow Layout::flow() const { return cur_().meta.flow; }

// A workspace made by going to its number, left as it was made: gone
// (design 2026-10-20 WuXian §3.9) -- one made on purpose, named or that held
// a window stays.
void Layout::forget_implicit_(int leaving) {
    if (leaving == current_ || spaces_.size() <= 1) return;
    const auto* s = space_(leaving);
    if (s && s->implicit && s->meta.name.empty() && s->windows.empty() && !s->had_windows)
        std::erase_if(spaces_, [&](const Space& o) { return o.meta.id == leaving; });
}

void Layout::show(int id) {
    if (!space_(id)) {
        if (add_workspace(o_.rule, id) == 0) return;
        space_(id)->implicit = true;
    }
    const int leaving = current_;
    current_ = id;
    forget_implicit_(leaving);
    focus_ = cur_().focus;
    if (view_ == View::wuxian) frame_workspace(id);
}

// ── windows in and out of workspaces ────────────────────────────────

// A window that is not floating has a place in each rule's arrangement of
// its workspace: a column (tile), a place in the order (stack).
void Layout::enter_(Space& s, Window& w, std::optional<Id> after) {
    if (w.floating) return;
    insert_column_(s.columns, w.id, after);
    s.order.push_back(w.id);
}

void Layout::leave_(Space& s, Id id) {
    erase_(s.columns, id);
    std::erase(s.order, id);
}

void Layout::into_(Space& s, Window w, std::optional<Id> after) {
    w.stack_placed = false;
    w.snapped = false;
    s.windows.push_back(w);
    enter_(s, s.windows.back(), after);
    s.had_windows = true;
    if (!s.focus) s.focus = w.id;
}

void Layout::send_to(Id id, int n) {
    if (!space_(n) && add_workspace(o_.rule, n) == 0) return;
    auto* to = space_(n);
    auto* from = space_of_(id);
    if (from == to) return;
    auto* w = window_(id);
    if (!w) return;
    auto copy = *w;
    if (sticky(id)) { std::erase_if(sticky_, [&](const Window& x) { return x.id == id; }); }
    else if (from) {
        leave_(*from, id);
        std::erase_if(from->windows, [&](const Window& x) { return x.id == id; });
        if (from->focus == id) from->focus = from->windows.empty() ? std::nullopt : std::optional(from->windows.back().id);
    } else {
        std::erase_if(free_, [&](const Free& f) { return f.w.id == id; });
        copy.floating = false;
    }
    fullscreen_.erase(id);
    into_(*space_(n), copy, std::nullopt);
}

void Layout::to_wuxian(Id id, std::optional<Rect> world) {
    if (is_free(id)) {
        // Collapsed, it moves and keeps the size it unfolds to (openwx 0.2 §5.2, J63).
        if (auto* f = free_of_(id); world) f->world = f->collapsed ? Rect{world->x, world->y, f->world.width, f->world.height} : *world;
        return;
    }
    auto* w = window_(id);
    if (!w) return;
    const auto where = world ? world : world_rect_of_(id);
    auto copy = *w;
    copy.floating = copy.pinned = false;
    if (auto* s = space_of_(id)) {
        leave_(*s, id);
        std::erase_if(s->windows, [&](const Window& x) { return x.id == id; });
        if (s->focus == id) s->focus = s->windows.empty() ? std::nullopt : std::optional(s->windows.back().id);
    } else {
        std::erase_if(sticky_, [&](const Window& x) { return x.id == id; });
    }
    fullscreen_.erase(id);
    const Size size{std::max(120.0, copy.preferred.width), std::max(80.0, copy.preferred.height)};
    free_.push_back({.w = copy, .world = where.value_or(free_place_(size, std::nullopt)), .z = top_z_()});
}

// ── rules ────────────────────────────────────────────────────────────

void Layout::convert_(Space& s, Rule to) {
    // Where each window is seen now, in the workspace view's pixels.
    auto seen = [&](Id id) -> std::optional<Rect> {
        for (const auto& p : last_)
            if (p.id == id && !p.floating && p.workspace == s.meta.id) return view_ == View::workspace ? p.rect : to_space_(s, p.rect);
        return std::nullopt;
    };
    if (to == Rule::tile) {
        std::vector<std::pair<double, Id>> xs;
        for (const auto& w : s.windows)
            if (!w.floating) xs.push_back({seen(w.id).value_or(Rect{std::numeric_limits<double>::max()}).x, w.id});
        std::ranges::stable_sort(xs, {}, &std::pair<double, Id>::first);
        std::vector<Column> cols;
        for (const auto& [x, id] : xs) cols.push_back({.cells = {{.id = id}}});
        s.columns = std::move(cols);
        s.scroll = 0;
        return;
    }
    // To stack: each window where it is seen, kept on the output.
    for (auto& w : s.windows) {
        if (w.floating) continue;
        w.snapped = false;
        if (const auto r = seen(w.id)) {
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

// ── adding and removing ──────────────────────────────────────────────

void Layout::add(Id id, Size preferred, bool floating, const Match& match) {
    if (contains(id)) return;
    bool placed = false;
    take_pending_(id, preferred, floating, match, placed);
    if (placed) return;
    Window w{.id = id, .preferred = preferred, .floating = floating};
    openwx::Destination dest{.workspace = current_};
    if (view_ == View::wuxian) {
        const Rect area = area_();
        const auto centre = openwx::to_world(camera_, {area.x + area.width / 2, area.y + area.height / 2}, {area.x, area.y});
        const auto ls = layers_();
        dest = openwx::new_window(View::wuxian, current_, ls, centre);
    }
    if (dest.plane) {
        w.floating = false;
        free_.push_back({.w = w, .world = free_place_(preferred, std::nullopt), .z = top_z_()});
        focus_ = id;
        reveal_(id);
        return;
    }
    auto* s = space_(dest.workspace);
    if (!s) s = &cur_();
    into_(*s, w, s->focus);
    s->focus = id;
    focus_ = id;
    current_ = s->meta.id;
    reveal_(id);
}

void Layout::remove(Id id) {
    fullscreen_.erase(id);
    if (std::erase_if(sticky_, [&](const Window& w) { return w.id == id; }) > 0) {
        if (cur_().focus == id) cur_().focus.reset();
        if (focus_ == id) focus_ = cur_().focus;
        return;
    }
    if (std::erase_if(free_, [&](const Free& f) { return f.w.id == id; }) > 0) {
        if (focus_ == id) focus_ = cur_().focus;
        return;
    }
    auto* s = space_of_(id);
    if (!s) return;
    // The focus goes to a neighbour: in the column, else the next column (tile).
    std::optional<Id> next;
    if (s->meta.rule == Rule::tile)
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
        if (!next && s->meta.rule == Rule::stack && !s->order.empty()) next = s->order.back();
        if (!next && !s->windows.empty()) next = s->windows.back().id;
        s->focus = next;
    }
    if (focus_ == id) {
        focus_ = s->focus;
        if (focus_) focus(*focus_);
    }
}

std::vector<Id> Layout::group_of(Id id) const {
    const auto* s = space_of_(id);
    if (!s || s->meta.rule != Rule::tile) return {id};
    if (const auto at = find_(s->columns, id)) {
        std::vector<Id> out;
        for (const auto& c : s->columns[at->column].cells) out.push_back(c.id);
        return out;
    }
    return {id};
}

// ── focus and moving ─────────────────────────────────────────────────

void Layout::focus(Id id) {
    if (sticky(id)) {
        cur_().focus = id;
        focus_ = id;
        return;
    }
    if (auto* f = free_of_(id)) {
        focus_ = id;
        const int z = top_z_();
        if (f->z < z - 1 || std::ranges::count_if(free_, [&](const Free& o) { return o.z == f->z; }) > 1) f->z = z;
        reveal_(id);
        return;
    }
    auto* s = space_of_(id);
    if (!s) return;
    s->focus = id;
    if (const auto at = find_(s->columns, id)) s->columns[at->column].active = at->cell;
    s->scrolled = false;   // the focus moved: it is shown again
    if (const auto* w = window_(id); w && !w->floating) {
        std::erase(s->order, id);
        s->order.push_back(id);
    }
    // The workspace view stays on its workspace: another's window is the one
    // it gives the focus to when it is shown. In the WuXian view the
    // workspace of the window focused is the current one.
    if (view_ == View::workspace && current_ != s->meta.id) return;
    focus_ = id;
    if (current_ != s->meta.id) {
        const int leaving = current_;
        current_ = s->meta.id;
        forget_implicit_(leaving);
    }
    reveal_(id);
}

std::optional<Id> Layout::focused() const {
    if (focus_ && contains(*focus_)) {
        if (view_ == View::wuxian) return focus_;
        if (sticky(*focus_) || (space_of_(*focus_) && space_of_(*focus_)->meta.id == current_)) return focus_;
    }
    return cur_().focus;
}

void Layout::move_focus(Direction d) {
    const auto f = focused();
    auto& s = cur_();
    if (!f) {
        if (view_ == View::workspace && !s.windows.empty()) focus(s.windows.back().id);
        return;
    }
    if (view_ == View::workspace && s.meta.rule == Rule::tile && space_of_(*f) == &s) {
        d = turned_(s, d);
        const auto at = find_(s.columns, *f);
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
    // Stacked, and the WuXian view: the nearest that way, by where each
    // window is now -- across workspaces and the plane in the WuXian view.
    std::vector<std::pair<Id, Rect>> rs;
    std::optional<Rect> from;
    for (const auto& p : last_) {
        if (p.id == *f) from = p.rect;
        else if (!p.pinned) rs.push_back({p.id, p.rect});
    }
    if (!from) return;
    if (const auto best = detail::nearest(*from, rs, d)) focus(*best);
}

void Layout::move(Direction d) {
    const auto f = focused();
    if (!f) return;
    auto* w = window_(*f);
    if (!w) return;
    const Rect area = area_();
    const double sx = d == Direction::left ? -1 : d == Direction::right ? 1 : 0;
    const double sy = d == Direction::up ? -1 : d == Direction::down ? 1 : 0;
    if (auto* fr = free_of_(*f)) {
        // An eighth of the view (design 2026-10-20 WuXian §3.12).
        const double z = view_ == View::wuxian ? camera_.zoom : 1;
        fr->world.x += sx * area.width / 8 / z;
        fr->world.y += sy * area.height / 8 / z;
        reveal_(*f);
        return;
    }
    if (w->floating) {
        w->free.x += sx * area.width / 4;
        w->free.y += sy * area.height / 4;
        return;
    }
    auto* s = space_of_(*f);
    if (!s) return;
    if (s->meta.rule == Rule::tile) tile_move_(*s, turned_(*s, d));
    else stack_move_(*w, d);
}

void Layout::join() {
    if (const auto f = focused())
        if (auto* s = space_of_(*f); s && s->meta.rule == Rule::tile) tile_join_(*s);
}

void Layout::split() {
    if (const auto f = focused())
        if (auto* s = space_of_(*f); s && s->meta.rule == Rule::tile) tile_split_(*s);
}

// ── a window's state ─────────────────────────────────────────────────

void Layout::toggle_floating(Id id) {
    auto* s = space_of_(id);
    auto* w = window_(id);
    if (!s || !w) return;
    if (!w->floating) {
        // Where it is on screen is where it floats; a tiled one is centred.
        std::optional<Rect> seen;
        if (s->meta.rule != Rule::tile)
            for (const auto& p : last_)
                if (p.id == id) seen = view_ == View::workspace ? p.rect : to_space_(*s, p.rect);
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
        // Floating it was on the screen: there it is stacked.
        if (w->placed) {
            w->stacked = w->free;
            w->stack_placed = true;
            w->snapped = false;
        }
    }
    s->focus = id;
    focus(id);
}

bool Layout::floating(Id id) const {
    const auto* w = window_(id);
    return w && w->floating;
}

void Layout::toggle_pinned(Id id) {
    auto* w = window_(id);
    if (!w || is_free(id)) return;
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
        cur_().windows.push_back(w);
        cur_().focus = id;
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
    cur_().focus = id;
    focus_ = id;
}

bool Layout::sticky(Id id) const { return std::ranges::contains(sticky_, id, &Window::id); }

void Layout::toggle_maximized(Id id) {
    auto* w = window_(id);
    if (!w || w->floating || is_free(id)) return;
    if (auto* s = space_of_(id); s && s->meta.rule == Rule::tile) {
        if (const auto at = find_(s->columns, id)) {
            auto& col = s->columns[at->column];
            if (col.width >= 1.0) col.width = col.restore;
            else { col.restore = col.width; col.width = 1.0; }
            reveal_(id);
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
    if (!w || w->floating || is_free(id)) return false;
    if (const auto* s = space_of_(id); s && s->meta.rule == Rule::tile) {
        const auto at = find_(s->columns, id);
        return at && s->columns[at->column].width >= 1.0;
    }
    return w->snapped && w->snap_x == 0 && w->snap_y == 0;
}

void Layout::toggle_fullscreen(Id id) { set_fullscreen(id, !fullscreen(id)); }

void Layout::set_fullscreen(Id id, bool on) {
    if (!window_(id)) return;
    if (!on) { fullscreen_.erase(id); return; }
    // One on a workspace: another there leaves it.
    const auto ws = workspace_of(id);
    std::erase_if(fullscreen_, [&](Id other) { return workspace_of(other) == ws; });
    fullscreen_.insert(id);
}

bool Layout::fullscreen(Id id) const { return fullscreen_.contains(id); }

// A screen rect of the WuXian view in the workspace view's pixels of `s`
// (what a floating or stacked window's frame is kept in).
Rect Layout::to_space_(const Space& s, Rect screen) const {
    if (view_ == View::workspace) return screen;
    const auto f = frame_(s.meta.id);
    if (!f || f->zoom <= 0) return screen;
    const Rect area = area_();
    return {area.x + (screen.x - f->screen.x) / f->zoom, area.y + (screen.y - f->screen.y) / f->zoom, screen.width / f->zoom,
            screen.height / f->zoom};
}

std::optional<Frame> Layout::frame_(int id) const {
    for (const auto& f : frames_)
        if (f.id == id) return f;
    return std::nullopt;
}

void Layout::place(Id id, Rect screen) {
    if (auto it = std::ranges::find(sticky_, id, &Window::id); it != sticky_.end()) {
        it->free = screen;
        it->placed = true;
        return;
    }
    if (auto* f = free_of_(id)) {
        if (view_ != View::wuxian) return;
        const Rect area = area_();
        const auto at = openwx::to_world(camera_, {screen.x, screen.y}, {area.x, area.y});
        // Collapsed: only where it is (its title bar is not its size, J63).
        if (f->collapsed) f->world = {at.x, at.y, f->world.width, f->world.height};
        else f->world = {at.x, at.y, screen.width / camera_.zoom, screen.height / camera_.zoom};
        return;
    }
    auto* s = space_of_(id);
    auto* w = window_(id);
    if (!s || !w) return;
    const Rect r = to_space_(*s, screen);
    if (w->floating) {
        w->free = r;
        w->placed = true;
        return;
    }
    if (s->meta.rule == Rule::stack) {
        w->stacked = r;
        w->stack_placed = true;
        w->snapped = false;
    }
}

void Layout::cycle_width() {
    const auto f = focused();
    auto* s = f ? space_of_(*f) : nullptr;
    if (!s || s->meta.rule != Rule::tile) return;
    static constexpr std::array<double, 4> kSteps { 1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0 };
    const auto at = find_(s->columns, *f);
    if (!at) return;
    const double now = tile_fraction_(*s, at->column);
    double next = kSteps.front();
    for (const double step : kSteps)
        if (step > now + 1e-6) { next = step; break; }
    s->columns[at->column].width = next;
    reveal_(*f);
}

void Layout::cycle_height() {
    const auto f = focused();
    auto* s = f ? space_of_(*f) : nullptr;
    if (!s || s->meta.rule != Rule::tile) return;
    const auto at = find_(s->columns, *f);
    if (!at) return;
    auto& col = s->columns[at->column];
    if (col.cells.size() < 2) return;
    // An equal part, 2/3, 1/3.
    auto& mine = col.cells[at->cell].share;
    const double next = mine == 0 ? 2.0 / 3 : mine > 0.5 ? 1.0 / 3 : 0;
    for (auto& c : col.cells) c.share = 0;
    mine = next;
}

void Layout::resize(double step) {
    const auto f = focused();
    if (!f) return;
    const Rect area = area_();
    if (auto* fr = free_of_(*f)) {
        fr->world.width = std::max(120.0, fr->world.width + step * area.width);
        fr->world.height = std::max(80.0, fr->world.height + step * area.height);
        return;
    }
    auto* s = space_of_(*f);
    if (!s) return;
    if (s->meta.rule == Rule::tile) {
        const auto at = find_(s->columns, *f);
        if (!at) return;
        s->columns[at->column].width = std::clamp(tile_fraction_(*s, at->column) + step, 0.1, 1.0);
        reveal_(*f);
        return;
    }
    auto* w = window_(*f);
    if (!w || w->floating) return;
    Rect r = stack_rect_(*w, area);
    const double dw = step * area.width, dh = step * area.height;
    r = {r.x - dw / 2, r.y - dh / 2, std::max(120.0, r.width + dw), std::max(80.0, r.height + dh)};
    w->stacked = r;
    w->stack_placed = true;
    w->snapped = false;
}

std::optional<Id> Layout::at(double sx, double sy) const {
    for (auto it = last_.rbegin(); it != last_.rend(); ++it)
        if (it->visible && it->rect.contains(sx, sy)) return it->id;
    return std::nullopt;
}

// ── Spring, and the wheel ────────────────────────────────────────────

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

double wheel_distance(const WheelSpeed& s, double steps, double ms) {
    const double base = s.base * s.factor;
    if (ms >= s.slow_ms || ms <= 0) return steps * base;
    // Faster than slow: the step grows with the speed, up to six times.
    const double speed = s.slow_ms / std::max(1.0, ms);
    return steps * base * std::min(6.0, 1 + (speed - 1));
}

// ── covered items (design 2026-10-21 §8.4) ───────────────────────────

namespace {

// `r` less `cut`: up to four rectangles.
void subtract_(const Rect& r, const Rect& cut, std::vector<Rect>& out) {
    if (!detail::overlaps(r, cut)) { out.push_back(r); return; }
    const double x0 = std::max(r.x, cut.x), x1 = std::min(r.x + r.width, cut.x + cut.width);
    const double y0 = std::max(r.y, cut.y), y1 = std::min(r.y + r.height, cut.y + cut.height);
    if (y0 > r.y) out.push_back({r.x, r.y, r.width, y0 - r.y});                                  // above
    if (y1 < r.y + r.height) out.push_back({r.x, y1, r.width, r.y + r.height - y1});              // below
    if (x0 > r.x) out.push_back({r.x, y0, x0 - r.x, y1 - y0});                                   // left
    if (x1 < r.x + r.width) out.push_back({x1, y0, r.x + r.width - x1, y1 - y0});                // right
}

constexpr double kSliver = 4;

Rect clip_(const Rect& r, const Rect& to) {
    const double x0 = std::max(r.x, to.x), y0 = std::max(r.y, to.y);
    const double x1 = std::min(r.x + r.width, to.x + to.width), y1 = std::min(r.y + r.height, to.y + to.height);
    return {x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
}

}  // namespace

std::vector<Covered> covered(std::span<const Rect> items, Rect screen) {
    std::vector<Covered> out;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const Rect shown = clip_(items[k], screen);
        if (shown.width < 0.5 || shown.height < 0.5) continue;   // off the screen: elsewhere, not covered
        std::vector<Rect> left{shown};
        for (std::size_t a = k + 1; a < items.size() && !left.empty(); ++a) {
            std::vector<Rect> next;
            for (const auto& r : left) subtract_(r, items[a], next);
            // A sliver narrower than a few pixels shows nothing one could take hold of.
            std::erase_if(next, [](const Rect& r) { return r.width < kSliver || r.height < kSliver; });
            left = std::move(next);
        }
        if (!left.empty()) continue;
        // The top-most over its centre carries its bookmark.
        // (Covered by several, none over its centre: the top-most over it.)
        const auto c = items[k].centre();
        std::optional<std::size_t> host;
        for (std::size_t a = items.size(); a-- > k + 1 && !host;)
            if (items[a].contains(c.x, c.y)) host = a;
        for (std::size_t a = items.size(); a-- > k + 1 && !host;)
            if (detail::overlaps(items[a], items[k])) host = a;
        out.push_back({k, host.value_or(items.size() - 1)});
    }
    return out;
}

std::vector<Rect> merge_damage(std::vector<Rect> rects, std::size_t limit) {
    limit = std::max<std::size_t>(limit, 1);
    while (rects.size() > limit) {
        std::size_t a = 0, b = 1;
        double best = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < rects.size(); ++i)
            for (std::size_t j = i + 1; j < rects.size(); ++j) {
                const auto u = detail::bounds(rects[i], rects[j]);
                const double added = u.width * u.height - rects[i].width * rects[i].height - rects[j].width * rects[j].height;
                if (added < best) { best = added; a = i; b = j; }
            }
        rects[a] = detail::bounds(rects[a], rects[b]);
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(b));
    }
    return rects;
}

}  // namespace xdesk::layout
