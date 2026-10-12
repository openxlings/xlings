module xdesk.layout;

import std;

namespace xdesk::layout {

namespace {

constexpr double kMinZoom = 0.1;

bool overlaps_(const Rect& a, const Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

Rect inset_(Size output, Insets r) {
    return {r.left, r.top, std::max(0.0, output.width - r.left - r.right), std::max(0.0, output.height - r.top - r.bottom)};
}

}  // namespace

Layout::Layout(Options o) : o_(o), spaces_(static_cast<std::size_t>(std::max(1, o.workspaces))) {}

Layout::Window* Layout::window_(Id id) {
    for (auto& s : spaces_)
        for (auto& w : s.windows)
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

bool Layout::contains(Id id) const { return window_(id) != nullptr; }
bool Layout::floating(Id id) const {
    const auto* w = window_(id);
    return w && w->floating;
}

std::optional<int> Layout::workspace_of(Id id) const {
    for (std::size_t i = 0; i < spaces_.size(); ++i)
        for (const auto& w : spaces_[i].windows)
            if (w.id == id) return static_cast<int>(i);
    return std::nullopt;
}

// ── adding and removing ──────────────────────────────────────────────

void Layout::place_new_(Space& s, Window& w) {
    const Rect area = inset_(output_, reserved_);
    if (w.floating) {
        // Floating: in the middle of the output, as big as it asked (or fits).
        const double width = std::min(w.preferred.width, area.width - 2 * o_.gap);
        const double height = std::min(w.preferred.height, area.height - 2 * o_.gap);
        w.free = {area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height};
        w.placed = true;
        return;
    }
    if (o_.mode != Mode::canvas) return;
    // On the canvas: right of the focused window, else the middle of the view;
    // then further right until it covers nothing.
    const double width = std::min(w.preferred.width, area.width / s.zoom), height = std::min(w.preferred.height, area.height / s.zoom);
    Rect r { s.cx + (area.width / s.zoom - width) / 2, s.cy + (area.height / s.zoom - height) / 2, width, height };
    if (w.beside)
        for (const auto& f : s.windows)
            if (f.id == *w.beside && f.placed && !f.floating) r = {f.free.x + f.free.width + 3 * o_.gap, f.free.y, width, height};
    for (int tries = 0; tries < 64; ++tries) {
        const auto hit = std::ranges::find_if(s.windows, [&](const Window& o) {
            return o.id != w.id && o.placed && !o.floating && overlaps_(o.free, r);
        });
        if (hit == s.windows.end()) break;
        r.x = hit->free.x + hit->free.width + 3 * o_.gap;
    }
    w.free = r;
    w.placed = true;
}

void Layout::add(Id id, Size preferred, bool floating) {
    if (contains(id)) return;
    auto& s = space_();
    // Placed by the next arrangement, which knows the output.
    s.windows.push_back({.id = id, .preferred = preferred, .floating = floating, .beside = s.focus});
    if (!floating) {
        auto at = s.columns.end();
        if (s.focus) {
            const auto f = std::ranges::find(s.columns, *s.focus, &Column::id);
            if (f != s.columns.end()) at = f + 1;
        }
        s.columns.insert(at, {.id = id});
    }
    s.focus = id;
    reveal_(s, id);
}

void Layout::remove(Id id) {
    auto* s = space_of_(id);
    if (!s) return;
    const auto col = std::ranges::find(s->columns, id, &Column::id);
    std::optional<Id> next;
    if (col != s->columns.end()) {
        const auto i = static_cast<std::size_t>(col - s->columns.begin());
        s->columns.erase(col);
        if (!s->columns.empty()) next = s->columns[std::min(i, s->columns.size() - 1)].id;
    }
    std::erase_if(s->windows, [&](const Window& w) { return w.id == id; });
    if (s->focus == id) {
        if (!next && !s->windows.empty()) next = s->windows.back().id;
        s->focus = next;
        if (next) reveal_(*s, *next);
    }
}

// ── focus and moving ─────────────────────────────────────────────────

void Layout::focus(Id id) {
    auto* s = space_of_(id);
    if (!s) return;
    s->focus = id;
    reveal_(*s, id);
}

std::optional<Id> Layout::focused() const { return space_().focus; }

void Layout::move_focus(Direction d) {
    auto& s = space_();
    if (s.windows.empty()) return;
    if (!s.focus) { focus(s.windows.back().id); return; }
    const auto* current = window_(*s.focus);
    if (o_.mode == Mode::tile || (current && current->floating)) {
        if (d != Direction::left && d != Direction::right) return;
        if (s.columns.empty()) return;
        const auto col = std::ranges::find(s.columns, *s.focus, &Column::id);
        if (col == s.columns.end()) {
            focus(d == Direction::right ? s.columns.front().id : s.columns.back().id);
            return;
        }
        const auto i = col - s.columns.begin();
        const auto j = d == Direction::right ? i + 1 : i - 1;
        if (j >= 0 && j < static_cast<std::ptrdiff_t>(s.columns.size())) focus(s.columns[static_cast<std::size_t>(j)].id);
        return;
    }
    // The canvas: the nearest window whose centre is that way, straight ahead
    // counting more than off to the side.
    const double fx = current->free.x + current->free.width / 2, fy = current->free.y + current->free.height / 2;
    std::optional<Id> best;
    double best_score = std::numeric_limits<double>::infinity();
    for (const auto& w : s.windows) {
        if (w.id == current->id || w.floating || !w.placed) continue;
        const double dx = w.free.x + w.free.width / 2 - fx, dy = w.free.y + w.free.height / 2 - fy;
        double ahead = 0, side = 0;
        switch (d) {
            case Direction::right: ahead = dx; side = dy; break;
            case Direction::left: ahead = -dx; side = dy; break;
            case Direction::down: ahead = dy; side = dx; break;
            case Direction::up: ahead = -dy; side = dx; break;
        }
        if (ahead <= 0) continue;
        const double score = ahead + 2 * std::abs(side);
        if (score < best_score) { best_score = score; best = w.id; }
    }
    if (best) focus(*best);
}

void Layout::move(Direction d) {
    auto& s = space_();
    if (!s.focus) return;
    auto* w = window_(*s.focus);
    if (!w) return;
    const Rect area = inset_(output_, reserved_);
    const double sx = d == Direction::left ? -1 : d == Direction::right ? 1 : 0;
    const double sy = d == Direction::up ? -1 : d == Direction::down ? 1 : 0;
    if (w->floating) {
        w->free.x += sx * area.width / 4;
        w->free.y += sy * area.height / 4;
        return;
    }
    if (o_.mode == Mode::canvas) {
        w->free.x += sx * area.width / 4 / s.zoom;
        w->free.y += sy * area.height / 4 / s.zoom;
        reveal_(s, w->id);
        return;
    }
    if (sx == 0) return;
    const auto col = std::ranges::find(s.columns, w->id, &Column::id);
    if (col == s.columns.end()) return;
    const auto i = col - s.columns.begin();
    const auto j = i + static_cast<std::ptrdiff_t>(sx);
    if (j < 0 || j >= static_cast<std::ptrdiff_t>(s.columns.size())) return;
    std::swap(s.columns[static_cast<std::size_t>(i)], s.columns[static_cast<std::size_t>(j)]);
    reveal_(s, w->id);
}

void Layout::toggle_floating(Id id) {
    auto* s = space_of_(id);
    auto* w = window_(id);
    if (!s || !w) return;
    const Rect area = inset_(output_, reserved_);
    if (!w->floating) {
        std::erase_if(s->columns, [&](const Column& c) { return c.id == id; });
        // Where it is on screen is where it floats; a tiled one is centred.
        if (o_.mode == Mode::canvas && w->placed) {
            w->free = {area.x + (w->free.x - s->cx) * s->zoom, area.y + (w->free.y - s->cy) * s->zoom,
                       w->free.width * s->zoom, w->free.height * s->zoom};
            w->floating = true;
        } else {
            w->floating = true;
            place_new_(*s, *w);
        }
    } else {
        w->floating = false;
        if (o_.mode == Mode::canvas) {
            w->free = {s->cx + (w->free.x - area.x) / s->zoom, s->cy + (w->free.y - area.y) / s->zoom,
                       w->free.width / s->zoom, w->free.height / s->zoom};
        }
        auto at = s->columns.end();
        if (s->focus && *s->focus != id) {
            const auto f = std::ranges::find(s->columns, *s->focus, &Column::id);
            if (f != s->columns.end()) at = f + 1;
        }
        s->columns.insert(at, {.id = id});
    }
    s->focus = id;
    reveal_(*s, id);
}

void Layout::place(Id id, Rect screen) {
    auto* s = space_of_(id);
    auto* w = window_(id);
    if (!s || !w) return;
    const Rect area = inset_(output_, reserved_);
    if (w->floating) {
        w->free = screen;
    } else if (o_.mode == Mode::canvas) {
        w->free = {s->cx + (screen.x - area.x) / s->zoom, s->cy + (screen.y - area.y) / s->zoom,
                   screen.width / s->zoom, screen.height / s->zoom};
    } else {
        return;
    }
    w->placed = true;
}

void Layout::cycle_width() {
    auto& s = space_();
    if (o_.mode != Mode::tile || !s.focus) return;
    const auto col = std::ranges::find(s.columns, *s.focus, &Column::id);
    if (col == s.columns.end()) return;
    static constexpr std::array<double, 4> kSteps { 1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0 };
    const double now = col->fraction > 0 ? col->fraction : (s.columns.size() <= 2 ? 1.0 / static_cast<double>(s.columns.size()) : 0.5);
    double next = kSteps.front();
    for (double f : kSteps)
        if (f > now + 1e-6) { next = f; break; }
    col->fraction = next;
    reveal_(s, col->id);
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
    to.windows.push_back(w);
    if (!w.floating) to.columns.push_back({.id = id});
    if (!to.focus) to.focus = id;
}

// ── the canvas's camera ──────────────────────────────────────────────

void Layout::pan(double dx, double dy) {
    auto& s = space_();
    if (o_.mode == Mode::tile) { s.scroll -= dx; return; }
    s.cx -= dx / s.zoom;
    s.cy -= dy / s.zoom;
}

void Layout::zoom_at(double factor, double sx, double sy) {
    auto& s = space_();
    if (o_.mode != Mode::canvas || factor <= 0) return;
    const Rect area = inset_(output_, reserved_);
    const double wx = s.cx + (sx - area.x) / s.zoom, wy = s.cy + (sy - area.y) / s.zoom;
    s.zoom = std::clamp(s.zoom * factor, kMinZoom, 1.0);
    s.cx = wx - (sx - area.x) / s.zoom;
    s.cy = wy - (sy - area.y) / s.zoom;
}

double Layout::zoom() const { return o_.mode == Mode::canvas ? space_().zoom : 1.0; }
bool Layout::in_overview() const { return space_().overview; }
std::pair<double, double> Layout::camera() const {
    if (o_.mode != Mode::canvas) return {0, 0};
    return {space_().cx, space_().cy};
}

void Layout::overview(bool on) {
    auto& s = space_();
    if (o_.mode != Mode::canvas || on == s.overview) return;
    const Rect area = inset_(output_, reserved_);
    if (on) {
        s.overview = true;
        s.saved_zoom = s.zoom;
        (void)area;   // fitted by each arrangement, as windows come and go
    } else {
        s.overview = false;
        s.zoom = 1;
        // Back in, around the focused window.
        if (s.focus)
            if (const auto* w = window_(*s.focus); w && w->placed && !w->floating) {
                s.cx = w->free.x + w->free.width / 2 - area.width / 2;
                s.cy = w->free.y + w->free.height / 2 - area.height / 2;
            }
    }
}

// The overview: every window of the canvas in view, as big as fits.
void Layout::fit_(Space& s, Rect area) {
    std::optional<Rect> box;
    for (const auto& w : s.windows) {
        if (w.floating || !w.placed) continue;
        if (!box) { box = w.free; continue; }
        const double x0 = std::min(box->x, w.free.x), y0 = std::min(box->y, w.free.y);
        const double x1 = std::max(box->x + box->width, w.free.x + w.free.width);
        const double y1 = std::max(box->y + box->height, w.free.y + w.free.height);
        box = Rect{x0, y0, x1 - x0, y1 - y0};
    }
    if (!box) return;
    const double margin = 4 * o_.gap;
    s.zoom = std::clamp(std::min(area.width / (box->width + 2 * margin), area.height / (box->height + 2 * margin)), kMinZoom, 1.0);
    s.cx = box->x + box->width / 2 - area.width / 2 / s.zoom;
    s.cy = box->y + box->height / 2 - area.height / 2 / s.zoom;
}

// Keeps a window in view: the least scroll (tile) or pan (canvas) that shows it.
void Layout::reveal_(Space& s, Id id) {
    const auto* w = window_(id);
    if (!w || w->floating) return;
    const Rect area = inset_(output_, reserved_);
    if (o_.mode == Mode::canvas) {
        if (s.overview || !w->placed) return;
        const double vw = area.width / s.zoom, vh = area.height / s.zoom, m = 2 * o_.gap;
        if (w->free.x - m < s.cx || w->free.width + 2 * m > vw) s.cx = w->free.x - m;
        else if (w->free.x + w->free.width + m > s.cx + vw) s.cx = w->free.x + w->free.width + m - vw;
        if (w->free.y - m < s.cy || w->free.height + 2 * m > vh) s.cy = w->free.y - m;
        else if (w->free.y + w->free.height + m > s.cy + vh) s.cy = w->free.y + w->free.height + m - vh;
    }
    // Tile: arrange() scrolls the focused column into view.
}

// ── arranging ────────────────────────────────────────────────────────

std::vector<Placement> Layout::tile_(Space& s, Rect area) {
    std::vector<Placement> out;
    const double g = o_.gap;
    const std::size_t n = s.columns.size();
    // Strip positions: one or two columns share the output, more are halves.
    std::vector<double> xs, ws;
    double x = g;
    for (const auto& c : s.columns) {
        const double f = c.fraction > 0 ? c.fraction : (n <= 2 ? 1.0 / static_cast<double>(n) : 0.5);
        const double w = std::max(1.0, f * (area.width - g) - g);
        xs.push_back(x);
        ws.push_back(w);
        x += w + g;
    }
    const double strip = x;
    // The focused column in view; a strip narrower than the output centred.
    double scroll = s.scroll;
    if (strip <= area.width) {
        scroll = -(area.width - strip) / 2;
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            if (s.focus != s.columns[i].id) continue;
            if (xs[i] - g < scroll) scroll = xs[i] - g;
            else if (xs[i] + ws[i] + g > scroll + area.width) scroll = xs[i] + ws[i] + g - area.width;
        }
        scroll = std::clamp(scroll, 0.0, strip - area.width);
    }
    s.scroll = scroll;
    for (std::size_t i = 0; i < n; ++i) {
        const Rect r { area.x + xs[i] - scroll, area.y + g, ws[i], std::max(1.0, area.height - 2 * g) };
        out.push_back({.id = s.columns[i].id, .rect = r, .visible = overlaps_(r, area), .focused = s.focus == s.columns[i].id});
    }
    return out;
}

std::vector<Placement> Layout::canvas_(Space& s, Rect area) {
    std::vector<Placement> out;
    // New windows first, in the order they came (each beside its anchor);
    // the focused one, if new, brought into view.
    bool focus_new = false;
    for (auto& w : s.windows) {
        if (w.floating || w.placed) continue;
        place_new_(s, w);
        if (s.focus == w.id) focus_new = true;
    }
    if (s.overview) fit_(s, area);
    else if (focus_new) reveal_(s, *s.focus);
    for (auto& w : s.windows) {
        if (w.floating) continue;
        const Rect r { area.x + (w.free.x - s.cx) * s.zoom, area.y + (w.free.y - s.cy) * s.zoom, w.free.width * s.zoom,
                       w.free.height * s.zoom };
        out.push_back({.id = w.id, .rect = r, .visible = overlaps_(r, area), .focused = s.focus == w.id});
    }
    return out;
}

std::vector<Placement> Layout::arrange(Size output, Insets reserved) {
    output_ = output;
    reserved_ = reserved;
    auto& s = space_();
    const Rect area = inset_(output, reserved);
    auto out = o_.mode == Mode::canvas ? canvas_(s, area) : tile_(s, area);
    // Floating windows above, kept on the output.
    for (auto& w : s.windows) {
        if (!w.floating) continue;
        if (!w.placed) place_new_(s, w);
        out.push_back({.id = w.id, .rect = w.free, .visible = overlaps_(w.free, area), .focused = s.focus == w.id, .floating = true});
    }
    last_ = out;
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
