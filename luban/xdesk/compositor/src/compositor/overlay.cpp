// The person's desktop (server.cppm; design 2026-10-19 §16-§18): their keys
// and state, the applications' rules, what each action does, and the
// overlays -- the arrangements (Super+m), the keys (Super+/), a window's
// menu; the switchers are switchers.cpp's.
module xdesk.compositor;

import std;
import xdesk.words;
import opensal;
import xdesk.os;
import xdesk.layout;
import openwx;
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

std::string_view rule_label_(layout::Rule r, bool zh) {
    return r == layout::Rule::stack ? i18n::tr("Stack") : i18n::tr("Tile");
}

}  // namespace

// ── the person's ─────────────────────────────────────────────────────

void Server::load_person() {
    const auto env = os::environment();
    if (auto k = config::read_keys(env)) {
        key_overrides = *k;
        key_overrides.erase("set");
        keys.load(*k);
    } else {
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
    // The workspaces' arrangements of before are WuXian's now (not carried
    // over): the file of before kept aside once, and said.
    if (state.old_arrangements) {
        if (auto kept = config::keep_aside(config::state_file(env))) log("state: the arrangements of before are not read (WuXian keeps its own: wuxian.json); the file is kept as " + kept->string());
        else log("state: " + kept.error());
        state.old_arrangements = false;
        save_state();
    }
    // WuXian as it was left: its workspaces at once, each window where it
    // was as it comes. One that cannot be read is kept aside, said, and the
    // desktop starts as new.
    // The canvas session of before is an alias (design 2026-10-20 WuXian
    // §12.2): it starts in the WuXian view, until WuXian was kept.
    if (o.profile == "canvas") lay.set_view(layout::View::wuxian);
    if (auto text = config::read_wuxian(env); !text) {
        wuxian_broken = true;
        log("wuxian: " + text.error() + " (started as new; the file is left as it is)");
    } else if (*text) {
        if (auto st = openwx::parse(**text); st && openwx::validate(*st).empty()) {
            lay.restore(*st);
            wuxian_written = **text;
        } else {
            const auto why = st ? openwx::validate(*st).front() : st.error();
            const auto kept = config::keep_aside(config::wuxian_file(env));
            log(std::format("wuxian: {} (started as new; {})", why, kept ? "the file is kept as " + kept->string() : kept.error()));
            show_hint(i18n::tr("The workspaces of before could not be read: started as new (the file is kept beside it)"), 8000);
        }
    }
    if (state.hinted && state.keys_seen < 2) {
        // Someone who knew the keys before (design 2026-10-20 §12.2): what changed, once.
        show_hint(i18n::tr("New keys: Super+F maximizes (float: Super+Shift+Space)  ·  Super+P on top  ·  Alt+Tab the windows  ·  Super+/ all of them"), 12000);
        state.keys_seen = 2;
        save_state();
    }
    // The first session: the tour (design 2026-10-21 §8.1); ended early, the keys' line instead.
    if (!state.toured) {
        state.hinted = true;
        state.keys_seen = 2;
        save_state();
        start_tour();
    }
}

void Server::save_state() {
    if (state_broken) return;
    if (auto r = config::write_state(os::environment(), state); !r) log("state: " + r.error());
}

void Server::remember_arrangement() { save_wuxian(); }

// WuXian, kept: each window by its application and title.
void Server::save_wuxian() {
    if (wuxian_broken) return;
    const auto text = openwx::format(lay.state([this](layout::Id id) -> layout::Match {
        const View* v = view_of(id);
        if (!v || !v->toplevel) return {};
        return {v->toplevel->app_id ? v->toplevel->app_id : "", v->toplevel->title ? v->toplevel->title : ""};
    }));
    if (text == wuxian_written) return;
    if (auto r = config::write_wuxian(os::environment(), text); !r) log("wuxian: " + r.error());
    else wuxian_written = text;
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
    if (r.workspace && *r.workspace >= 1 && *r.workspace <= lay.options().max_workspaces && lay.workspace_of(v.id) != *r.workspace)
        lay.send_to(v.id, *r.workspace);
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
    if (const auto info = std::ranges::find(actions(), a, &ActionInfo::action); info != actions().end())
        publish("action", {{"name", std::string(info->name)}});
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
        case Action::keys: open_settings(SettingsPage::keys); return;
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
            // The WuXian view: in to the focused window (design 2026-10-20 §9.1).
            if (wuxian()) { if (const auto f = lay.focused()) lay.center(*f); }
            else lay.even_widths();
            arrange();
            return;
        case Action::height: lay.cycle_height(); arrange(); return;
        case Action::smaller: case Action::larger: {
            const bool in = a == Action::larger;
            // The WuXian view: the view; else the focused column or window.
            if (wuxian()) lay.zoom_at(in ? 1.1 : 1 / 1.1, logical_w() / 2.0, logical_h() / 2.0);
            else lay.resize(in ? 0.05 : -0.05);
            arrange();
            return;
        }
        case Action::maximize:
            // Maximized and back, in every arrangement (design 2026-10-20 §12.2).
            if (const auto f = lay.focused()) { lay.toggle_maximized(*f); arrange(); }
            return;
        case Action::fullscreen:
            if (const auto f = lay.focused()) { lay.toggle_fullscreen(*f); arrange(); }
            return;
        case Action::toggle_float:
            if (const auto f = lay.focused()) { lay.toggle_floating(*f); arrange(); }
            return;
        case Action::pin: if (const auto f = lay.focused()) { lay.toggle_pinned(*f); arrange(); } return;
        case Action::sticky: if (const auto f = lay.focused()) { lay.toggle_sticky(*f); arrange(); } return;
        case Action::collapse: collapse_focused(); return;
        case Action::settings: open_settings(SettingsPage::displays); return;
        case Action::arrangements: open_chooser(); return;
        case Action::switch_apps: open_switcher(held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)); return;
        case Action::app_windows: other_app_window(held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)); return;
        case Action::windows: open_spread(held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)); return;
        case Action::wuxian:
            // Into the WuXian view, and back to the focused workspace
            // (design 2026-10-20 WuXian §3.2).
            change_view();
            return;
        case Action::to_wuxian:
            if (const auto f = lay.focused()) { lay.to_wuxian(*f); remember_arrangement(); arrange(); follow_focus(); }
            return;
        case Action::new_workspace: {
            const int made = lay.add_workspace(lay.options().rule);
            if (made == 0) { show_hint(i18n::tr("Nine workspaces are as many as there are"), 3000); return; }
            if (wuxian()) { lay.frame_workspace(made); ws_selected = made; arrange(); }
            else switch_workspace(made);
            remember_arrangement();
            return;
        }
        case Action::copy: copy_paste(false); return;
        case Action::paste: copy_paste(true); return;
        case Action::show_all:
            if (wuxian()) { lay.show_all(); arrange(); }
            return;
        case Action::show1: case Action::show2: case Action::show3: case Action::show4: case Action::show5:
        case Action::show6: case Action::show7: case Action::show8: case Action::show9: {
            // Workspace n + 1, made when it is not there (design 2026-10-20 WuXian §3.9).
            const int n = static_cast<int>(a) - static_cast<int>(Action::show1) + 1;
            if (wuxian()) { lay.show(n); arrange(); }
            else switch_workspace(n);
            return;
        }
        case Action::send1: case Action::send2: case Action::send3: case Action::send4: case Action::send5:
        case Action::send6: case Action::send7: case Action::send8: case Action::send9: {
            const int n = static_cast<int>(a) - static_cast<int>(Action::send1) + 1;
            if (const auto f = lay.focused()) lay.send_to(*f, n);
            arrange();
            follow_focus();
            return;
        }
        case Action::workspace_prev: case Action::workspace_next: case Action::carry_prev: case Action::carry_next: {
            // The one before or after by number, among those there are.
            const bool back = a == Action::workspace_prev || a == Action::carry_prev;
            const auto ids = lay.workspaces();
            const auto at = std::ranges::find(ids, lay.current());
            if (at == ids.end() || (back && at == ids.begin()) || (!back && at + 1 == ids.end())) return;
            const int n = back ? *(at - 1) : *(at + 1);
            const auto f = lay.focused();
            if ((a == Action::carry_prev || a == Action::carry_next) && f) lay.send_to(*f, n);
            if (!wuxian() && !((a == Action::carry_prev || a == Action::carry_next) && f)) { switch_workspace(n); return; }
            lay.show(n);
            if ((a == Action::carry_prev || a == Action::carry_next) && f) lay.focus(*f);
            arrange();
            follow_focus();
            return;
        }
    }
}

// ── the overlays ─────────────────────────────────────────────────────

void Server::open_chooser() {
    if (overlay == Overlay::chooser) {
        overlay_at = (overlay_at + 1) % chooser_rules.size();
        draw_overlay();
        return;
    }
    close_overlay(false);
    chooser_rules = {layout::Rule::tile, layout::Rule::stack};
    const auto next = layout::other(lay.rule());
    overlay_at = static_cast<std::size_t>(std::ranges::find(chooser_rules, next) - chooser_rules.begin());
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
    menu_of = MenuOf::window;
    menu_for = v->id;
    overlay = Overlay::menu;
    overlay_at = 0;
    overlay_picture.x = x;
    overlay_picture.y = y;
    draw_overlay();
}

void Server::draw_overlay() {
    if (static Overlay said = Overlay::none; said != overlay) {
        said = overlay;
        publish("overlay", {{"open", std::string(std::array<std::string_view, 10>{"none", "chooser", "switcher", "spread", "sheet", "menu",
                                                                             "layouts", "settings", "dialog", "naming"}[static_cast<std::size_t>(overlay)])}});
    }
    if (overlay == Overlay::none) {
        wlr_scene_node_set_enabled(&overlay_node->node, false);
        return;
    }
    const int w = logical_w(), h = logical_h();
    switch (overlay) {
        case Overlay::chooser: {
            std::vector<shell::ChooserCard> cards;
            for (const auto r : chooser_rules) {
                // The workspace laid out this way: the same function, a copy of it.
                auto copy = lay;
                copy.set_rule(copy.current(), r);
                // Every window of it, the ones off the output too: the overview's picture.
                shell::ChooserCard card{.label = std::string(rule_label_(r, zh))};
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
        case Overlay::settings: draw_settings(); return;
        case Overlay::spread: draw_spread(); return;
        case Overlay::sheet: {
            // Super held a second (design 2026-10-20 §12.4): the keys used
            // most, the first chord of each; the rest are Super+/'s.
            std::vector<shell::SheetRow> rows;
            rows.push_back({"Super", std::string(i18n::tr("Applications"))});
            for (const auto a : {Action::switch_apps, Action::windows, Action::terminal, Action::maximize, Action::close,
                                 Action::arrangements, Action::settings, Action::keys}) {
                const auto chords = keys.chords(a);
                const auto* info = &*std::ranges::find(actions(), a, &ActionInfo::action);
                if (!chords.empty()) rows.push_back({chord_text(chords.front()), std::string(i18n::tr(info->en))});
            }
            overlay_picture = shell::sheet(ui(), big_font ? &*big_font : nullptr, theme, i18n::tr("The keys used most"), rows,
                                           w, h, zh, scale);
            break;
        }
        case Overlay::dialog: draw_dialog(); break;
        case Overlay::naming: draw_naming(); break;
        case Overlay::menu: {
            std::vector<shell::MenuEntry> entries;
            // `label` in the person's language (i18n::tr), or a name as it is.
            auto item = [&](std::string_view label, std::string act, bool checked = false) {
                entries.push_back({.label = std::string(label), .checked = checked});
                menu_actions.push_back(std::move(act));
            };
            auto line = [&] { entries.push_back({.separator = true}); menu_actions.push_back(""); };
            if (menu_of != MenuOf::window) {
                // A workspace's, WuXian's, the workspace view's background (design 2026-10-20 WuXian §3.9, §3.11).
                menu_actions.clear();
                if (menu_of == MenuOf::workspace) {
                    const auto* w = lay.workspace_info(menu_workspace);
                    if (!w) { close_overlay(false); return; }
                    item(i18n::tr("Go in"), "enter");
                    item(i18n::tr("Name…"), "rename");
                    line();
                    item(i18n::tr("Tile"), "rule:tile", w->rule == layout::Rule::tile);
                    item(i18n::tr("Stack"), "rule:stack", w->rule == layout::Rule::stack);
                    item(i18n::tr("Another colour"), "color");
                    item(i18n::tr("Half size on WuXian"), "scale:0.5", std::abs(w->scale - 0.5) < 1e-3);
                    item(i18n::tr("Full size on WuXian"), "scale:1", std::abs(w->scale - 1) < 1e-3);
                    item(i18n::tr("Collapsed (its label only)"), "collapse", w->collapsed);
                    line();
                    item(i18n::tr("Remove the workspace"), "delete");
                } else if (menu_of == MenuOf::bookmarks) {
                    // A bookmark's "+N": the covered items not shown.
                    for (const auto& [window, ws] : menu_more) {
                        if (window) {
                            View* mv = view_of(window);
                            const auto name = mv ? name_of(*mv) : std::string("?");
                            item(name, std::format("bm:w:{}", window));
                        } else if (const auto* w = lay.workspace_info(ws)) {
                            item(w->name.empty() ? i18n::trf("Workspace {}", w->id) : std::format("{} · {}", w->id, w->name), std::format("bm:s:{}", ws));
                        }
                    }
                } else if (menu_of == MenuOf::plane) {
                    item(i18n::tr("A tiled workspace here"), "new:tile");
                    item(i18n::tr("A stacked workspace here"), "new:stack");
                    line();
                    item(i18n::tr("Tidy the workspaces up (one row)"), "tidy");
                    item(i18n::tr("Show everything"), "show-all");
                } else {
                    item(i18n::tr("Terminal"), "terminal");
                    item(i18n::tr("Settings"), "settings");
                    item(i18n::tr("The WuXian view"), "wuxian");
                }
                overlay_picture = shell::menu(ui(), theme, entries, overlay_picture.x, overlay_picture.y, w, h, scale);
                break;
            }
            View* v = view_of(menu_for);
            if (!v) { close_overlay(false); return; }
            const char* app = v->toplevel->app_id;
            const auto rule = app && state.rules.contains(app) ? state.rules.at(app) : config::Rule{};
            std::vector<shell::MenuEntry> items;
            menu_actions.clear();
            auto add = [&](std::string_view label, std::string act, bool checked = false) {
                items.push_back({.label = std::string(label), .checked = checked});
                menu_actions.push_back(std::move(act));
            };
            auto sep = [&] { items.push_back({.separator = true}); menu_actions.push_back(""); };
            add(i18n::tr("Maximize / restore"), "maximize", lay.maximized(v->id));
            add(i18n::tr("Float / put back"), "float", lay.floating(v->id));
            add(i18n::tr("Always on top"), "pin", lay.pinned(v->id));
            add(i18n::tr("On every workspace"), "sticky", lay.sticky(v->id));
            sep();
            for (const int k : lay.workspaces())
                add(i18n::trf("To workspace {}", k), std::format("ws:{}", k), lay.workspace_of(v->id) == k);
            add(i18n::tr("Onto WuXian (a free window)"), "wuxian", lay.is_free(v->id));
            if (lay.is_free(v->id) && wuxian()) add(i18n::tr("Collapsed (its title bar only)"), "collapse", lay.collapsed(v->id));
            if (app) {
                sep();
                add(i18n::tr("Always floating (this app)"), "rule:float", rule.floating);
                add(i18n::tr("Always on this workspace (this app)"), "rule:workspace", rule.workspace.has_value());
                add(i18n::tr("Remember size and place (this app)"), "rule:rect", rule.rect.has_value());
            }
            sep();
            add(i18n::tr("Close"), "close");
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
    if (was == Overlay::dialog) { removal.reset(); asking_reset = false; }
    if (was == Overlay::naming) naming = 0;
    wlr_scene_node_set_enabled(&overlay_node->node, false);
    if (was == Overlay::switcher || was == Overlay::spread) {
        // The windows back where they are, or the one chosen first -- and it
        // kept lit a moment while the others are dimmed (design 2026-10-20 WuXian §4.3).
        lit_id = 0;
        if (apply) choose_switched(was);
        end_spread();
        arrange();
        follow_focus();
        if (apply && lit_id) start_highlight(lit_id);
        return;
    }
    if (!apply) return;
    if (was == Overlay::chooser && overlay_at < chooser_rules.size()) {
        lay.set_rule(lay.current(), chooser_rules[overlay_at]);
        remember_arrangement();
        arrange();
        draw_shell();
        follow_focus();
    }
}

void Server::menu_choose(std::size_t item) {
    if (item >= menu_actions.size()) return;
    const auto what = menu_actions[item];
    if (menu_of != MenuOf::window) {
        close_overlay(false);
        if (!what.empty()) workspace_action(what);
        return;
    }
    View* v = view_of(menu_for);
    close_overlay(false);
    if (!v || what.empty()) return;
    const auto id = v->id;
    const char* app = v->toplevel->app_id;
    if (what == "maximize") lay.toggle_maximized(id);
    else if (what == "float") lay.toggle_floating(id);
    else if (what == "pin") lay.toggle_pinned(id);
    else if (what == "sticky") lay.toggle_sticky(id);
    else if (what.starts_with("ws:")) lay.send_to(id, std::atoi(what.c_str() + 3));
    else if (what == "wuxian") lay.to_wuxian(id);
    else if (what == "collapse") { collapse_window(id); return; }
    else if (what == "close") { wlr_xdg_toplevel_send_close(v->toplevel); return; }
    else if (app && what.starts_with("rule:")) {
        auto& r = state.rules[app];
        if (what == "rule:float") r.floating = !r.floating;
        else if (what == "rule:workspace") {
            if (r.workspace) r.workspace.reset();
            else if (const auto ws = lay.workspace_of(id)) r.workspace = *ws;
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
    if (overlay == Overlay::dialog) return dialog_key(sym);
    if (overlay == Overlay::naming) return naming_key(sym, pressed_code);
    // The settings take every key (a key being given, text being typed).
    if (overlay == Overlay::settings) {
        if (sym == key::Escape && !settings_capture) {
            if (!settings_search.empty()) { settings_search.clear(); draw_overlay(); }
            else close_overlay(false);
            return true;
        }
        return settings_key(sym);
    }
    const auto s = xkb_keysym_to_lower(sym);
    if (s == key::Escape) { close_overlay(false); return true; }
    switch (overlay) {
        case Overlay::chooser:
            if (s == key::m || s == key::Right || s == key::Tab) open_chooser();
            else if (s == key::Left) { overlay_at = (overlay_at + chooser_rules.size() - 1) % chooser_rules.size(); draw_overlay(); }
            else if (s == key::Return || s == key::KP_Enter) close_overlay(true);
            return true;
        case Overlay::switcher: return switcher_key(s);
        case Overlay::settings: return true;
        case Overlay::spread: return spread_key(s);
        case Overlay::sheet:
            // A key: it goes (and the key does what it does: input.cpp).
            close_overlay(false);
            return false;
        case Overlay::menu: case Overlay::layouts: case Overlay::dialog: case Overlay::naming:
            return true;
        case Overlay::none: return false;
    }
    return false;
}

bool Server::overlay_click(double x, double y) {
    if (overlay == Overlay::none) return false;
    if (overlay == Overlay::switcher || overlay == Overlay::spread) return switched_click(x, y);
    if (overlay == Overlay::settings) return settings_click(x, y);
    if (overlay == Overlay::dialog) return dialog_click(x, y);
    if (overlay == Overlay::naming) {
        // Elsewhere than the field: what is typed is the name.
        if (!shell::hit(overlay_picture, x, y)) {
            lay.rename(naming, name_text);
            close_overlay(false);
            remember_arrangement();
            arrange();
        }
        return true;
    }
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
                if (v.mapped && v.id != layouts_for && lay.workspace_of(v.id) == lay.workspace_of(layouts_for) && !lay.floating(v.id)) ids.push_back(v.id);
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
    hint_text = std::string(text);
    redraw_hint();
    wlr_scene_node_set_enabled(&hint_node->node, true);
    if (hint_timer) wl_event_source_timer_update(hint_timer, ms);
}

// Placed by the output's size: a window that is resized right after the
// desktop starts (the first entry) had it half under the bottom edge (design
// 2026-10-21 §6.2, J10).
void Server::redraw_hint() {
    if (hint_text.empty()) return;
    auto pic = shell::hint(ui(), theme, hint_text, logical_w(), logical_h(), scale);
    wlr_scene_node_set_position(&hint_node->node, static_cast<int>(pic.x), static_cast<int>(pic.y));
    show(hint_node, std::move(pic.image), scale);
}

}  // namespace xdesk::compositor
