// The two switchers (server.cppm; design 2026-10-20 §10): Super+Tab by
// application, Super+` among an application's windows, Alt+Tab the windows
// where they are -- and the overview's picture of windows they share
// (layout::spread): the windows themselves, live, their frames moved and
// their surfaces scaled, the chosen one larger, the rest dimmed.
module xdesk.compositor;

import std;
import opensal;
import xdesk.os;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import xdesk.apps;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;
import :keys;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace key = xdesk::native::key;
namespace draw = xdesk::draw;
namespace shell = xdesk::shell;
namespace apps = xdesk::apps;

namespace {

constexpr int kQuickMs = 150;        // Alt+Tab let go sooner: the previous window, nothing shown
constexpr double kHoverScale = 1.04; // the window under the pointer, lifted (design 2026-10-20 WuXian §4.2)
constexpr int kFadeMs = 500;         // the chosen one's outline fading where it is, after it was chosen
constexpr float kHoverOutline = 0.55f;   // the one under the pointer: its outline, faint
constexpr double kMargin = 40;

// The nearest of `rs` from `from` that way (its middle that way, the
// distance across it counted twice): what an arrow goes to.
std::optional<std::size_t> nearest_(const std::vector<std::pair<layout::Id, layout::Rect>>& rs, std::size_t from, layout::Direction d) {
    if (from >= rs.size()) return std::nullopt;
    auto mid = [](const layout::Rect& r) { return std::pair{r.x + r.width / 2, r.y + r.height / 2}; };
    const auto [fx, fy] = mid(rs[from].second);
    std::optional<std::size_t> best;
    double best_score = 0;
    for (std::size_t k = 0; k < rs.size(); ++k) {
        if (k == from) continue;
        const auto [x, y] = mid(rs[k].second);
        const double dx = x - fx, dy = y - fy;
        double along = 0, across = 0;
        switch (d) {
            case layout::Direction::left: along = -dx; across = std::abs(dy); break;
            case layout::Direction::right: along = dx; across = std::abs(dy); break;
            case layout::Direction::up: along = -dy; across = std::abs(dx); break;
            case layout::Direction::down: along = dy; across = std::abs(dx); break;
        }
        if (along <= 0) continue;
        const double score = along + 2 * across;
        if (!best || score < best_score) { best = k; best_score = score; }
    }
    return best;
}

std::optional<layout::Direction> arrow_(xkb_keysym_t s) {
    if (s == key::Left) return layout::Direction::left;
    if (s == key::Right) return layout::Direction::right;
    if (s == key::Up) return layout::Direction::up;
    if (s == key::Down) return layout::Direction::down;
    return std::nullopt;
}

}  // namespace

// An application is its app id; a window that gives none is one of its own, by title.
std::string Server::app_of(View& v) const {
    if (v.toplevel->app_id && *v.toplevel->app_id) return v.toplevel->app_id;
    return std::format("window:{}", v.id);
}

// The windows shown, the most recently focused first (the views' order): the
// current workspace's in the workspace view, every one in the WuXian view
// (design 2026-10-20 WuXian §4.2).
std::vector<layout::Id> Server::recent_windows() {
    std::vector<layout::Id> out;
    for (const auto& v : views)
        if (v.mapped && (wuxian() || lay.workspace_of(v.id) == lay.current())) out.push_back(v.id);
    return out;
}

// ── the overview's picture ───────────────────────────────────────────

// `ids` where the overview puts them in `area` (design 2026-10-20 §10.3):
// where they are now to one another, the whole fitted; `chosen` ringed in
// the accent and above the rest, which are dimmed; the one under the pointer
// lifted a little (design 2026-10-20 WuXian §4.2: choosing and hovering are
// two things); any other window is not shown. The windows glide there (the
// springs) and back.
void Server::show_spread(const std::vector<layout::Id>& ids, layout::Rect area, layout::Id chosen) {
    std::vector<layout::Placement> ps;
    std::vector<layout::Rect> real;
    for (const auto id : ids)
        if (const auto p = std::ranges::find(placements, id, &layout::Placement::id); p != placements.end()) {
            ps.push_back(*p);
            real.push_back(p->rect);
        }
    const auto rects = layout::spread(real, area);
    spread_rects.clear();
    spread_zoom.clear();
    for (std::size_t k = 0; k < ps.size(); ++k) {
        auto r = rects[k];
        if (ps[k].id == spread_hover) {
            const double w = r.width * kHoverScale, h = r.height * kHoverScale;
            r = {r.x - (w - r.width) / 2, r.y - (h - r.height) / 2, w, h};
        }
        spread_rects.push_back({ps[k].id, r});
        spread_zoom[ps[k].id] = r.width / std::max(1.0, real[k].width);
        ps[k].rect = r;
        ps[k].focused = ps[k].id == chosen;
        ps[k].visible = true;
    }
    std::ranges::stable_partition(ps, [&](const layout::Placement& p) { return p.id != chosen; });
    // Gathered on the switchers' own layer, over a dimmed output: above the
    // workspaces' plates and labels (WuXian), every window shown whole.
    const float dim[4] = {0, 0, 0, static_cast<float>(std::clamp(1.0 - dim_level, 0.2, 0.85))};
    wlr_scene_rect_set_color(spread_dim, dim);
    wlr_scene_rect_set_size(spread_dim, logical_w(), logical_h());
    wlr_scene_node_set_enabled(&spread_dim->node, true);
    for (auto& v : views)
        if (v.mapped && spread_zoom.contains(v.id)) wlr_scene_node_reparent(&v.frame->node, spread_layer);
    apply(ps, false);
    for (const auto& p : ps)
        if (View* v = view_of(p.id)) wlr_scene_node_raise_to_top(&v->frame->node);
    // The window's own outline: the chosen one's in the accent, the one under
    // the pointer's faint; nothing drawn around them (design 2026-10-21 §8.2).
    for (auto& v : views) {
        if (!v.mapped) continue;
        if (v.id == chosen) set_outline(v, theme.accent, 1.0f);
        else if (v.id == spread_hover) set_outline(v, theme.text, kHoverOutline);
        else set_outline(v, theme.accent, 0);
    }
}

// The windows as they were: their own scale, not dimmed (the caller arranges them).
void Server::end_spread() {
    spread_hover = 0;
    wlr_scene_node_set_enabled(&spread_dim->node, false);
    if (spread_zoom.empty() && spread_rects.empty()) return;
    spread_zoom.clear();
    spread_rects.clear();
    for (auto& v : views) {
        if (v.frame->node.parent == spread_layer) wlr_scene_node_reparent(&v.frame->node, window_layer);
        if (v.id != lit_id || lit == Lit::none) set_outline(v, theme.accent, 0);
    }
}

// ── Super+Tab, Super+` ───────────────────────────────────────────────

void Server::open_switcher(bool back) {
    if (overlay == Overlay::switcher) {
        // Again: the next application (Shift: the one before), its windows closed.
        if (!switcher_apps.empty())
            overlay_at = (overlay_at + (back ? switcher_apps.size() - 1 : 1)) % switcher_apps.size();
        switcher_window = 0;
        if (switcher_open) {
            switcher_open = false;
            end_spread();
            arrange();
        }
        wl_event_source_timer_update(switch_timer, hold_ms);
        draw_overlay();
        return;
    }
    close_overlay(false);
    switcher_apps.clear();
    for (const auto id : recent_windows()) {
        View* v = view_of(id);
        if (!v) continue;
        const auto k = app_of(*v);
        auto it = std::ranges::find(switcher_apps, k, &AppGroup::key);
        if (it == switcher_apps.end()) switcher_apps.push_back({k, {id}});
        else it->windows.push_back(id);
    }
    // The application used before this one: what a press and a release goes to.
    const auto n = switcher_apps.size();
    overlay_at = n > 1 ? (back ? n - 1 : 1) : 0;
    switcher_open = false;
    switcher_window = 0;
    overlay = Overlay::switcher;
    wl_event_source_timer_update(switch_timer, hold_ms);
    draw_overlay();
}

// Super+`: the focused application's window used longest ago -- pressed
// again, the next, round all of them; Shift: the one used just before.
void Server::other_app_window(bool back) {
    View* f = focused();
    if (!f) return;
    const auto app = app_of(*f);
    std::vector<View*> same;
    for (auto& v : views)
        if (&v != f && v.mapped && (wuxian() || lay.workspace_of(v.id) == lay.current()) && app_of(v) == app) same.push_back(&v);
    if (same.empty()) return;
    View* to = back ? same.front() : same.back();
    lay.focus(to->id);
    arrange();
    focus(to);
}

void Server::draw_switcher() {
    const int w = logical_w(), h = logical_h();
    std::vector<shell::AppTile> tiles;
    for (const auto& g : switcher_apps) {
        View* v = view_of(g.windows.front());
        if (!v) continue;
        // The application's name (its id without a desktop entry), not its window's title.
        const char* id = v->toplevel->app_id && *v->toplevel->app_id ? v->toplevel->app_id : nullptr;
        std::string name = name_of(*v);
        if (const auto cut = name.find("  —  "); cut != std::string::npos) name.resize(cut);
        if (id && !app_names.contains(id)) name = id;
        tiles.push_back({.name = name, .key = g.key, .icon = id ? icon_of(id) : nullptr, .windows = static_cast<int>(g.windows.size())});
    }
    overlay_picture = shell::app_switcher(ui(), theme, tiles, overlay_at, switcher_open, w, h, zh, scale);
    const double bar_bottom = overlay_picture.y + overlay_picture.image.height() / scale;
    if (switcher_open && overlay_at < switcher_apps.size()) {
        const auto& ws = switcher_apps[overlay_at].windows;
        switcher_window = std::min(switcher_window, ws.size() - 1);
        show_spread(ws, {kMargin, bar_bottom + 24, w - 2 * kMargin, h - bar_bottom - 24 - kMargin}, ws[switcher_window]);
    }
    wlr_scene_node_set_position(&overlay_node->node, static_cast<int>(overlay_picture.x), static_cast<int>(overlay_picture.y));
    show(overlay_node, std::move(overlay_picture.image), scale);
    overlay_picture.image = {};
    wlr_scene_node_set_enabled(&overlay_node->node, true);
}

bool Server::switcher_key(xkb_keysym_t s) {
    const bool back = s == key::ISO_Left_Tab || (!switcher_open && s == key::Left);
    if (s == key::Tab || s == key::ISO_Left_Tab || (!switcher_open && (s == key::Left || s == key::Right))) {
        open_switcher(back);
    } else if (s == key::Down && !switcher_open && overlay_at < switcher_apps.size()) {
        switcher_open = true;
        draw_overlay();
    } else if (s == key::Up && switcher_open) {
        switcher_open = false;
        end_spread();
        arrange();
        draw_overlay();
    } else if (switcher_open && (s == key::Left || s == key::Right)) {
        if (const auto d = arrow_(s); d && overlay_at < switcher_apps.size()) {
            const auto& ws = switcher_apps[overlay_at].windows;
            const auto at = std::ranges::find(spread_rects, ws[switcher_window], &std::pair<layout::Id, layout::Rect>::first);
            if (const auto k = nearest_(spread_rects, static_cast<std::size_t>(at - spread_rects.begin()), *d)) {
                switcher_window = static_cast<std::size_t>(std::ranges::find(ws, spread_rects[*k].first) - ws.begin());
                draw_overlay();
            }
        }
    } else if (s == key::Return || s == key::KP_Enter) {
        close_overlay(true);
    }
    return true;
}

// ── Alt+Tab ──────────────────────────────────────────────────────────

void Server::open_spread(bool back, bool stays) {
    if (overlay == Overlay::spread) {
        if (!spread_ids.empty()) overlay_at = (overlay_at + (back ? spread_ids.size() - 1 : 1)) % spread_ids.size();
        spread_shown = true;
        draw_overlay();
        return;
    }
    close_overlay(false);
    spread_ids = recent_windows();
    if (spread_ids.empty()) return;
    const auto n = spread_ids.size();
    overlay = Overlay::spread;
    spread_stays = stays;
    if (stays) {
        // Super+O: every window at once, the focused one chosen, until one is.
        overlay_at = 0;
        spread_shown = true;
        draw_overlay();
        return;
    }
    overlay_at = n > 1 ? (back ? n - 1 : 1) : 0;
    // Shown once Alt is held: a quick press is the previous window, at once.
    spread_shown = false;
    wl_event_source_timer_update(switch_timer, kQuickMs);
}

void Server::switch_timer_fired() {
    if (overlay == Overlay::spread && !spread_shown) {
        spread_shown = true;
        draw_overlay();
    } else if (overlay == Overlay::switcher && !switcher_open && overlay_at < switcher_apps.size()) {
        switcher_open = true;
        draw_overlay();
    }
}

void Server::draw_spread() {
    if (!spread_shown || overlay_at >= spread_ids.size()) return;
    const int w = logical_w(), h = logical_h();
    const double top = reserved().top + kMargin;
    show_spread(spread_ids, {kMargin, top, w - 2 * kMargin, h - top - kMargin - 36}, spread_ids[overlay_at]);
    const auto at = std::ranges::find(spread_rects, spread_ids[overlay_at], &std::pair<layout::Id, layout::Rect>::first);
    View* v = view_of(spread_ids[overlay_at]);
    if (at == spread_rects.end() || !v) return;
    const auto& r = at->second;
    overlay_picture = shell::spread_label(ui(), theme, {r.x, r.y, r.width, r.height}, name_of(*v), w, h, scale);
    wlr_scene_node_set_position(&overlay_node->node, static_cast<int>(overlay_picture.x), static_cast<int>(overlay_picture.y));
    show(overlay_node, std::move(overlay_picture.image), scale);
    overlay_picture.image = {};
    wlr_scene_node_set_enabled(&overlay_node->node, true);
}

bool Server::spread_key(xkb_keysym_t s) {
    if (s == key::Tab || s == key::ISO_Left_Tab) {
        open_spread(s == key::ISO_Left_Tab);
    } else if (const auto d = arrow_(s)) {
        if (!spread_shown) { spread_shown = true; draw_overlay(); }
        const auto at = std::ranges::find(spread_rects, spread_ids[overlay_at], &std::pair<layout::Id, layout::Rect>::first);
        if (const auto k = nearest_(spread_rects, static_cast<std::size_t>(at - spread_rects.begin()), *d)) {
            overlay_at = static_cast<std::size_t>(std::ranges::find(spread_ids, spread_rects[*k].first) - spread_ids.begin());
            draw_overlay();
        }
    } else if (s == key::Return || s == key::KP_Enter) {
        close_overlay(true);
    }
    return true;
}

// ── both ─────────────────────────────────────────────────────────────

// The one chosen, its outline lit where it is (design 2026-10-21 §8.2, J15):
// held while the windows go where they go, then for the time chosen, then
// faded in kFadeMs -- the others are never dimmed and lit again. With less
// motion asked for, it is not lit at all.
void Server::start_highlight(layout::Id id) {
    View* v = view_of(id);
    if (highlight_ms <= 0 || !v) return;
    for (auto& o : views)
        if (o.id != id) set_outline(o, theme.accent, 0);
    if (reduce_motion) {
        set_outline(*v, theme.accent, 0);
        lit = Lit::none;
        return;
    }
    lit = Lit::settling;
    lit_id = id;
    lit_at = std::chrono::steady_clock::now();
    set_outline(*v, theme.accent, 1.0f);
    if (lit_timer) wl_event_source_timer_update(lit_timer, 16);
}

void Server::highlight_step() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    View* v = view_of(lit_id);
    if (!v) { lit = Lit::none; return; }
    switch (lit) {
        case Lit::none: return;
        case Lit::settling:
            // Until every window is where it goes (a second at most).
            if (std::ranges::any_of(views, [](const View& o) { return o.moving; }) && now - lit_at < seconds(1)) break;
            lit = Lit::holding;
            lit_at = now;
            break;
        case Lit::holding:
            if (now - lit_at < milliseconds(highlight_ms)) break;
            lit = Lit::fading;
            lit_at = now;
            break;
        case Lit::fading: {
            const double t = duration<double, std::milli>(now - lit_at).count() / kFadeMs;
            if (t >= 1) { set_outline(*v, theme.accent, 0); lit = Lit::none; return; }
            set_outline(*v, theme.accent, static_cast<float>(1 - t));
            break;
        }
    }
    if (lit_timer) wl_event_source_timer_update(lit_timer, 16);
}

// What the switcher holds, focused (close_overlay, before the windows go back).
void Server::choose_switched(Overlay was) {
    layout::Id id = 0;
    if (was == Overlay::switcher && overlay_at < switcher_apps.size()) {
        const auto& ws = switcher_apps[overlay_at].windows;
        id = ws[std::min(switcher_window, ws.size() - 1)];
    } else if (was == Overlay::spread && overlay_at < spread_ids.size()) {
        id = spread_ids[overlay_at];
    }
    if (View* v = id ? view_of(id) : nullptr) {
        lay.focus(v->id);
        focus(v);
        lit_id = v->id;
    }
}

bool Server::switched_click(double x, double y) {
    // A window shown: it. An application's tile: its most recent window.
    for (const auto& [id, r] : spread_rects | std::views::reverse)
        if (r.contains(x, y)) {
            if (overlay == Overlay::spread) overlay_at = static_cast<std::size_t>(std::ranges::find(spread_ids, id) - spread_ids.begin());
            else if (overlay_at < switcher_apps.size()) {
                const auto& ws = switcher_apps[overlay_at].windows;
                switcher_window = static_cast<std::size_t>(std::ranges::find(ws, id) - ws.begin());
            }
            close_overlay(true);
            return true;
        }
    if (overlay == Overlay::switcher)
        for (const auto& a : overlay_picture.areas)
            if (x >= a.rect.x && y >= a.rect.y && x < a.rect.x + a.rect.width && y < a.rect.y + a.rect.height) {
                overlay_at = static_cast<std::size_t>(a.index);
                switcher_window = 0;
                close_overlay(true);
                return true;
            }
    close_overlay(false);
    return true;
}

// The pointer over a window shown lifts it and chooses it (an application's
// tile, that application); leaving them all, it lifts none and the one
// chosen stays chosen (H6).
void Server::switched_hover(double x, double y) {
    layout::Id over = 0;
    for (const auto& [id, r] : spread_rects | std::views::reverse)
        if (r.contains(x, y)) { over = id; break; }
    if (over != spread_hover) {
        spread_hover = over;
        draw_overlay();
    }
    for (const auto& [id, r] : spread_rects | std::views::reverse) {
        if (!r.contains(x, y)) continue;
        if (overlay == Overlay::spread) {
            const auto k = static_cast<std::size_t>(std::ranges::find(spread_ids, id) - spread_ids.begin());
            if (k != overlay_at && k < spread_ids.size()) { overlay_at = k; draw_overlay(); }
        } else if (overlay_at < switcher_apps.size()) {
            const auto& ws = switcher_apps[overlay_at].windows;
            const auto k = static_cast<std::size_t>(std::ranges::find(ws, id) - ws.begin());
            if (k != switcher_window && k < ws.size()) { switcher_window = k; draw_overlay(); }
        }
        return;
    }
    if (overlay == Overlay::switcher && !switcher_open)
        for (const auto& a : overlay_picture.areas)
            if (x >= a.rect.x && y >= a.rect.y && x < a.rect.x + a.rect.width && y < a.rect.y + a.rect.height
                && static_cast<std::size_t>(a.index) != overlay_at) {
                overlay_at = static_cast<std::size_t>(a.index);
                wl_event_source_timer_update(switch_timer, hold_ms);
                draw_overlay();
                return;
            }
}

// An application's icon (design 2026-10-20 §10.1, 2026-10-24 §9.5): its
// desktop entry's Icon=, a PNG in the icon themes, a scalable SVG drawn at the
// size it is shown, the pixmaps; none is an initial. At `logical` pixels (the
// switcher's by default), at this density.
const draw::Image* Server::icon_of(const std::string& app, int logical) {
    return icon_named(app, app_icon(app), logical);
}

const draw::Image* Server::icon_named(const std::string& app, const std::string& name, int logical) {
    const int px = static_cast<int>(std::lround((logical > 0 ? logical : shell::kAppIcon) * scale));
    const auto key = std::pair{app, px};
    if (const auto it = icons.find(key); it != icons.end()) return it->second.empty() ? nullptr : &it->second;
    if (name.empty()) return nullptr;   // its entry not read yet: asked again later, not remembered as none
    auto& img = icons[key];
    const auto env = os::environment();
    if (const auto file = apps::icon_file(env, name, px)) {
        auto read = file->extension() == ".svg" ? draw::read_svg(*file, px) : draw::read_png(*file);
        if (read) img = read->width() == px && read->height() == px ? std::move(*read) : draw::scaled(*read, px, px);
        else log("icon " + file->string() + ": " + read.error());
    }
    return img.empty() ? nullptr : &img;
}

}  // namespace xdesk::compositor
