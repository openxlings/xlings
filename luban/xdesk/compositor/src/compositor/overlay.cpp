// The person's desktop (server.cppm; design 2026-10-19 §16-§18): their keys
// and state, the applications' rules, what each action does, and the
// overlays -- the arrangements (Super+m), the keys (Super+/), a window's
// menu; the switchers are switchers.cpp's.
module xdesk.compositor;

import std;
import opensal;
import xdesk.os;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import xdesk.apps;
import xdesk.config;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;
import :keys;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace key = xdesk::native::key;
namespace shell = xdesk::shell;
namespace apps = xdesk::apps;
namespace config = xdesk::config;

namespace {

std::string_view tr_(bool zh, std::string_view en, std::string_view cn) { return zh ? cn : en; }

std::string_view mode_label_(layout::Mode m, bool zh) {
    switch (m) {
        case layout::Mode::tile: return tr_(zh, "Tile", "平铺");
        case layout::Mode::stack: return tr_(zh, "Stack", "层叠");
        case layout::Mode::canvas: return tr_(zh, "Canvas", "画布");
    }
    return "";
}

}  // namespace

// ── the person's ─────────────────────────────────────────────────────

void Server::load_person() {
    const auto env = os::environment();
    if (auto k = config::read_keys(env)) keys.load(*k);
    else {
        log("keys: " + k.error() + " (the desktop's own keys are used)");
        keys.load({});
    }
    for (const auto& p : keys.problems) log(p);
    if (auto s = config::read_state(env)) state = std::move(*s);
    else {
        // Not read is not nothing: said, the defaults used, the file left as it is.
        state_broken = true;
        log("state: " + s.error() + " (started as if new; the file is kept as it is)");
    }
    // Each workspace's arrangement as it was; the others the session's: the
    // xdesk session's is the one last chosen (tile the first time).
    std::optional<layout::Mode> session;
    if (o.profile == "xdesk") session = layout::mode_named(state.last).value_or(layout::Mode::tile);
    for (int w = 0; w < lay.workspaces(); ++w) {
        const auto saved = w < static_cast<int>(state.arrangements.size()) ? layout::mode_named(state.arrangements[static_cast<std::size_t>(w)])
                                                                           : std::nullopt;
        if (saved) lay.preset(w, *saved);
        else if (session) lay.preset(w, *session);
    }
    if (!state.hinted) {
        show_hint(tr_(zh, "Super+m: tile, stack, canvas  ·  Super+/: the keys", "Super+m 切换排布  ·  Super+/ 查看快捷键"), 8000);
        state.hinted = true;
        save_state();
    }
}

void Server::save_state() {
    if (state_broken) return;
    if (auto r = config::write_state(os::environment(), state); !r) log("state: " + r.error());
}

void Server::remember_arrangement() {
    const auto w = static_cast<std::size_t>(lay.workspace());
    if (state.arrangements.size() <= w) state.arrangements.resize(w + 1);
    state.arrangements[w] = std::string(layout::name(lay.mode()));
    state.last = state.arrangements[w];
    save_state();
}

// An application's rule, as its window maps (design 2026-10-19 §18.1).
void Server::apply_rules(View& v) {
    const char* id = v.toplevel->app_id;
    if (!id) return;
    const auto it = state.rules.find(id);
    if (it == state.rules.end()) return;
    const auto& r = it->second;
    if ((r.floating || r.rect) && !lay.floating(v.id)) lay.toggle_floating(v.id);
    if (r.rect) lay.place(v.id, {double((*r.rect)[0]), double((*r.rect)[1]), double((*r.rect)[2]), double((*r.rect)[3])});
    if (r.workspace && *r.workspace >= 1 && *r.workspace <= lay.workspaces() && lay.workspace_of(v.id) != *r.workspace - 1)
        lay.send_to(v.id, *r.workspace - 1);
}

// A window by its application's name (design 2026-10-19 §18.3), its own
// title beside it -- whatever a terminal or the program writes there.
std::string Server::name_of(View& v) {
    const std::string title = v.toplevel->title ? v.toplevel->title : "";
    const std::string id = v.toplevel->app_id ? v.toplevel->app_id : "";
    if (id.empty()) return title;
    auto it = app_names.find(id);
    if (it == app_names.end() && std::chrono::steady_clock::now() - app_names_read > std::chrono::seconds(10)) {
        app_names_read = std::chrono::steady_clock::now();
        auto env = os::environment();
        std::string lang;
        for (const char* k : {"LC_ALL", "LC_MESSAGES", "LANG"})
            if (env.contains(k) && !env.at(k).empty()) { lang = env.at(k); break; }
        app_names.clear();
        app_icons.clear();
        for (const auto& a : apps::scan(apps::dirs(env), lang)) {
            app_names[a.id] = a.name;
            app_icons[a.id] = a.icon;
        }
        it = app_names.find(id);
    }
    const std::string name = it != app_names.end() ? it->second : std::string{};
    if (name.empty()) return title.empty() ? id : title;
    if (title.empty() || title == name) return name;
    return name + "  —  " + title;
}

// Its desktop entry's Icon= (read with the names, name_of).
std::string Server::app_icon(const std::string& id) {
    const auto it = app_icons.find(id);
    return it != app_icons.end() ? it->second : std::string{};
}

// ── what each action does ────────────────────────────────────────────

void Server::act(Action a) {
    auto way = [&](layout::Direction d, bool move) {
        if (move) lay.move(d); else lay.move_focus(d);
        arrange();
        follow_focus();
    };
    switch (a) {
        case Action::apps:
            if (launcher.is_open()) { launcher.close(); draw_launcher(); }
            else open_launcher();
            return;
        case Action::terminal: launch(terminal_argv({})); return;
        case Action::close: if (focused()) wlr_xdg_toplevel_send_close(focused()->toplevel); return;
        case Action::keys: open_sheet(); return;
        case Action::focus_left: return way(layout::Direction::left, false);
        case Action::focus_right: return way(layout::Direction::right, false);
        case Action::focus_up: return way(layout::Direction::up, false);
        case Action::focus_down: return way(layout::Direction::down, false);
        case Action::move_left: return way(layout::Direction::left, true);
        case Action::move_right: return way(layout::Direction::right, true);
        case Action::move_up: return way(layout::Direction::up, true);
        case Action::move_down: return way(layout::Direction::down, true);
        case Action::join: lay.join(); arrange(); follow_focus(); return;
        case Action::split: lay.split(); arrange(); follow_focus(); return;
        case Action::width: lay.cycle_width(); arrange(); return;
        case Action::even:
            // The canvas: in to the focused window (design 2026-10-20 §9.1).
            if (canvas()) { if (const auto f = lay.focused()) lay.center(*f); }
            else lay.even_widths();
            arrange();
            return;
        case Action::height: lay.cycle_height(); arrange(); return;
        case Action::smaller: case Action::larger: {
            const bool in = a == Action::larger;
            // The canvas: the view; else the focused column or window.
            if (canvas()) lay.zoom_at(in ? 1.1 : 1 / 1.1, logical_w() / 2.0, logical_h() / 2.0);
            else lay.resize(in ? 0.05 : -0.05);
            arrange();
            return;
        }
        case Action::toggle_float:
            // Stacked: the whole output and back; else floating and back.
            if (const auto f = lay.focused()) {
                if (lay.mode() == layout::Mode::stack && !lay.floating(*f)) lay.toggle_maximized(*f);
                else lay.toggle_floating(*f);
                arrange();
            }
            return;
        case Action::pin: if (const auto f = lay.focused()) { lay.toggle_pinned(*f); arrange(); } return;
        case Action::arrangements: open_chooser(); return;
        case Action::switch_apps: open_switcher(held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)); return;
        case Action::app_windows: other_app_window(held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)); return;
        case Action::windows: open_spread(held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)); return;
        case Action::overview: lay.overview(!lay.in_overview()); arrange(); return;
        case Action::show1: case Action::show2: case Action::show3: case Action::show4:
            lay.show(static_cast<int>(a) - static_cast<int>(Action::show1));
            arrange();
            follow_focus();
            return;
        case Action::send1: case Action::send2: case Action::send3: case Action::send4:
            if (const auto f = lay.focused()) lay.send_to(*f, static_cast<int>(a) - static_cast<int>(Action::send1));
            arrange();
            follow_focus();
            return;
    }
}

// ── the overlays ─────────────────────────────────────────────────────

void Server::open_chooser() {
    if (overlay == Overlay::chooser) {
        overlay_at = (overlay_at + 1) % chooser_modes.size();
        draw_overlay();
        return;
    }
    close_overlay(false);
    chooser_modes = {layout::Mode::tile, layout::Mode::stack, layout::Mode::canvas};
    const auto next = layout::next(lay.mode());
    overlay_at = static_cast<std::size_t>(std::ranges::find(chooser_modes, next) - chooser_modes.begin());
    overlay = Overlay::chooser;
    draw_overlay();
}

void Server::open_sheet() {
    close_overlay(false);
    overlay = Overlay::sheet;
    draw_overlay();
}

void Server::open_menu(View* v, double x, double y) {
    if (!v) return;
    close_overlay(false);
    menu_for = v->id;
    overlay = Overlay::menu;
    overlay_at = 0;
    overlay_picture.x = x;
    overlay_picture.y = y;
    draw_overlay();
}

void Server::draw_overlay() {
    if (overlay == Overlay::none) {
        wlr_scene_node_set_enabled(&overlay_node->node, false);
        return;
    }
    const int w = logical_w(), h = logical_h();
    switch (overlay) {
        case Overlay::chooser: {
            std::vector<shell::ChooserCard> cards;
            for (const auto m : chooser_modes) {
                // The workspace laid out this way: the same function, a copy of it.
                auto copy = lay;
                copy.set_mode(m);
                // Every window of it, the ones off the output too: the overview's picture.
                shell::ChooserCard card{.label = std::string(mode_label_(m, zh))};
                const auto ps = copy.arrange(output_size(), reserved());
                std::vector<layout::Rect> rects;
                for (const auto& p : ps) rects.push_back(p.rect);
                const auto fitted = layout::spread(rects, {0, 0, double(w), double(h)});
                for (std::size_t k = 0; k < ps.size(); ++k) {
                    View* v = view_of(ps[k].id);
                    const auto& r = fitted[k];
                    card.windows.push_back({.rect = {r.x, r.y, r.width, r.height}, .title = v ? name_of(*v) : "", .focused = ps[k].focused});
                }
                cards.push_back(std::move(card));
            }
            overlay_picture = shell::chooser(ui(), theme, cards, overlay_at, w, h, zh, scale);
            break;
        }
        case Overlay::switcher: draw_switcher(); return;
        case Overlay::spread: draw_spread(); return;
        case Overlay::sheet: {
            // Only what means something in this arrangement (design 2026-10-19 §16.3).
            std::vector<shell::SheetRow> rows;
            const auto m = lay.mode();
            for (const auto& a : actions()) {
                if ((m == layout::Mode::tile && !a.tile) || (m == layout::Mode::stack && !a.stack) || (m == layout::Mode::canvas && !a.canvas))
                    continue;
                std::string chords;
                for (const auto& c : keys.chords(a.action)) chords += (chords.empty() ? "" : " / ") + chord_text(c);
                if (!chords.empty()) rows.push_back({chords, std::string(zh ? a.zh : a.en)});
            }
            const auto title = std::format("{} · {}", tr_(zh, "Keys", "快捷键"), mode_label_(m, zh));
            overlay_picture = shell::sheet(ui(), big_font ? &*big_font : nullptr, theme, title, rows, w, h, zh, scale);
            break;
        }
        case Overlay::menu: {
            View* v = view_of(menu_for);
            if (!v) { close_overlay(false); return; }
            const char* app = v->toplevel->app_id;
            const auto rule = app && state.rules.contains(app) ? state.rules.at(app) : config::Rule{};
            std::vector<shell::MenuEntry> items;
            menu_actions.clear();
            auto add = [&](std::string_view en, std::string_view cn, std::string act, bool checked = false) {
                items.push_back({.label = std::string(tr_(zh, en, cn)), .checked = checked});
                menu_actions.push_back(std::move(act));
            };
            auto sep = [&] { items.push_back({.separator = true}); menu_actions.push_back(""); };
            add("Maximize / restore", "最大化 / 还原", "maximize", lay.maximized(v->id));
            add("Float / put back", "浮动 / 放回布局", "float", lay.floating(v->id));
            add("Always on top", "置顶", "pin", lay.pinned(v->id));
            add("On every workspace", "在所有工作区显示", "sticky", lay.sticky(v->id));
            sep();
            for (int k = 1; k <= std::min(4, lay.workspaces()); ++k)
                add(std::format("To workspace {}", k), std::format("移到工作区 {}", k), std::format("ws:{}", k), lay.workspace_of(v->id) == k - 1);
            if (app) {
                sep();
                add("Always floating (this app)", "总是浮动（此应用）", "rule:float", rule.floating);
                add("Always on this workspace (this app)", "总在此工作区打开（此应用）", "rule:workspace", rule.workspace.has_value());
                add("Remember size and place (this app)", "记住大小和位置（此应用）", "rule:rect", rule.rect.has_value());
            }
            sep();
            add("Close", "关闭", "close");
            overlay_picture = shell::menu(ui(), theme, items, overlay_picture.x, overlay_picture.y, w, h, scale);
            break;
        }
        case Overlay::layouts:
            overlay_picture = shell::layouts(theme, overlay_picture.x, overlay_picture.y, w, h, scale);
            break;
        case Overlay::none: break;
    }
    wlr_scene_node_set_position(&overlay_node->node, static_cast<int>(overlay_picture.x), static_cast<int>(overlay_picture.y));
    show(overlay_node, std::move(overlay_picture.image), scale);
    overlay_picture.image = {};
    wlr_scene_node_set_enabled(&overlay_node->node, true);
}

void Server::close_overlay(bool apply) {
    const auto was = overlay;
    overlay = Overlay::none;
    wlr_scene_node_set_enabled(&overlay_node->node, false);
    if (was == Overlay::switcher || was == Overlay::spread) {
        // The windows back where they are, or the one chosen first.
        if (apply) choose_switched(was);
        end_spread();
        arrange();
        follow_focus();
        return;
    }
    if (!apply) return;
    if (was == Overlay::chooser && overlay_at < chooser_modes.size()) {
        lay.set_mode(chooser_modes[overlay_at]);
        remember_arrangement();
        arrange();
        draw_shell();
        follow_focus();
    }
}

void Server::menu_choose(std::size_t item) {
    if (item >= menu_actions.size()) return;
    const auto what = menu_actions[item];
    View* v = view_of(menu_for);
    close_overlay(false);
    if (!v || what.empty()) return;
    const auto id = v->id;
    const char* app = v->toplevel->app_id;
    if (what == "maximize") lay.toggle_maximized(id);
    else if (what == "float") lay.toggle_floating(id);
    else if (what == "pin") lay.toggle_pinned(id);
    else if (what == "sticky") lay.toggle_sticky(id);
    else if (what.starts_with("ws:")) lay.send_to(id, std::atoi(what.c_str() + 3) - 1);
    else if (what == "close") { wlr_xdg_toplevel_send_close(v->toplevel); return; }
    else if (app && what.starts_with("rule:")) {
        auto& r = state.rules[app];
        if (what == "rule:float") r.floating = !r.floating;
        else if (what == "rule:workspace") {
            if (r.workspace) r.workspace.reset();
            else if (const auto ws = lay.workspace_of(id)) r.workspace = *ws + 1;
        } else if (what == "rule:rect") {
            if (r.rect) r.rect.reset();
            else if (const auto p = placement_of(id))
                r.rect = std::array{int(std::lround(p->x)), int(std::lround(p->y)), int(std::lround(p->width)), int(std::lround(p->height))};
        }
        if (r == config::Rule{}) state.rules.erase(app);
        save_state();
    }
    arrange();
    follow_focus();
}

// A key while an overlay is up: it is the overlay's.
bool Server::overlay_key(xkb_keysym_t sym, bool down) {
    if (overlay == Overlay::none) return false;
    if (!down) return true;
    const auto s = xkb_keysym_to_lower(sym);
    if (s == key::Escape) { close_overlay(false); return true; }
    switch (overlay) {
        case Overlay::chooser:
            if (s == key::m || s == key::Right || s == key::Tab) open_chooser();
            else if (s == key::Left) { overlay_at = (overlay_at + chooser_modes.size() - 1) % chooser_modes.size(); draw_overlay(); }
            else if (s == key::Return || s == key::KP_Enter) close_overlay(true);
            return true;
        case Overlay::switcher: return switcher_key(s);
        case Overlay::spread: return spread_key(s);
        case Overlay::sheet:
            // Any key closes it (a modifier alone does not).
            if (s != key::Super_L && s != key::Super_R && s != key::Alt_L && s != key::Alt_R) close_overlay(false);
            return true;
        case Overlay::menu: case Overlay::layouts:
            return true;
        case Overlay::none: return false;
    }
    return false;
}

bool Server::overlay_click(double x, double y) {
    if (overlay == Overlay::none) return false;
    if (overlay == Overlay::switcher || overlay == Overlay::spread) return switched_click(x, y);
    // Hit areas are in output pixels.
    std::optional<shell::Area> hit;
    for (const auto& a : overlay_picture.areas)
        if (x >= a.rect.x && y >= a.rect.y && x < a.rect.x + a.rect.width && y < a.rect.y + a.rect.height) hit = a;
    if (!hit) { close_overlay(false); return true; }
    const auto k = static_cast<std::size_t>(hit->index);
    switch (overlay) {
        case Overlay::chooser: overlay_at = k; close_overlay(true); break;
        case Overlay::menu: menu_choose(k); break;
        case Overlay::layouts: {
            // The window and the ones used most recently on its workspace (design 2026-10-19 §17.10).
            close_overlay(false);
            std::vector<layout::Id> ids{layouts_for};
            for (const auto& v : views)
                if (v.mapped && v.id != layouts_for && lay.workspace_of(v.id) == lay.workspace() && !lay.floating(v.id)) ids.push_back(v.id);
            lay.arrange_as(static_cast<layout::Pattern>(std::min<std::size_t>(k, 3)), ids);
            arrange();
            follow_focus();
            break;
        }
        default: close_overlay(false);
    }
    return true;
}

void Server::show_hint(std::string_view text, int ms) {
    auto pic = shell::hint(ui(), theme, text, logical_w(), logical_h(), scale);
    wlr_scene_node_set_position(&hint_node->node, static_cast<int>(pic.x), static_cast<int>(pic.y));
    show(hint_node, std::move(pic.image), scale);
    wlr_scene_node_set_enabled(&hint_node->node, true);
    if (hint_timer) wl_event_source_timer_update(hint_timer, ms);
}

}  // namespace xdesk::compositor
