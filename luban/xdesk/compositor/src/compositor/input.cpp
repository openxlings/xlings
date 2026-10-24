// The window's input: keys, the pointer, the wheel, and the desktop's own chords (server.cppm).
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import opensal.keys;
import xdesk.os;
import xdesk.layout;
import telemetry;
import xdesk.draw;
import xdesk.shell;
import xdesk.apps;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace key = xdesk::native::key;
namespace draw = xdesk::draw;
namespace shell = xdesk::shell;
namespace apps = xdesk::apps;
namespace fs = std::filesystem;

// ── input ────────────────────────────────────────────────────────
bool Server::held(wlr_keyboard_modifier m) {
    return wlr_keyboard_get_modifiers(&keyboard) & static_cast<std::uint32_t>(m);
}

// A chord the desktop keeps for itself -- what the key table says
// (keys.json over the desktop's own). Super's layer, and the few Alt chords
// it lists (Alt+Tab, Alt+F4, Ctrl+Alt+T): Alt alone is the applications'
// (design 2026-10-20 §6, §12.1).
bool Server::shortcut(std::uint32_t code) {
    const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO), alt = held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
    if (!super && !alt) return false;
    Chord pressed{.super = super, .ctrl = held(wlr_keyboard_modifier::WLR_MODIFIER_CTRL), .shift = held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT),
                  .alt = alt};
    // A digit by its place (Shift changes what it types): evdev 2-10 are 1-9, 11 is 0 (10 here).
    if (code >= 2 && code <= 11) pressed.digit = static_cast<int>(code) - 1;
    const xkb_keysym_t* syms = nullptr;
    const int n = xkb_state_key_get_syms(keyboard.xkb_state, code + 8, &syms);
    // Shift turns Tab into ISO_Left_Tab: the chord is still Shift+Tab.
    if (n > 0) pressed.sym = syms[0] == key::ISO_Left_Tab ? key::Tab : xkb_keysym_to_lower(syms[0]);
    auto found = keys.match(pressed);
    if (!found && pressed.digit) {
        pressed.digit = 0;
        found = keys.match(pressed);
    }
    if (!found) return false;
    // Super+Space is held as a hand (design 2026-10-20 WuXian §3.12): let go
    // without a drag, it is the applications.
    if (*found == Action::apps && pressed.sym == key::space) {
        hand = true;
        hand_used = false;
        pointer_shape("grab");
        return true;
    }
    act(*found);
    return true;
}

void Server::key(const opensal::Key& k) {
    if (k.repeat) return;   // clients repeat a held key themselves
    auto stroke = opensal::keys::evdev_key(k.keysym);
    if (!stroke) return;
    auto press = [this](std::uint32_t code, bool down) {
        wlr_keyboard_key_event ev{.time_msec = now_ms(), .keycode = code, .update_state = true,
                                  .state = down ? wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_PRESSED
                                                : wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_RELEASED};
        wlr_keyboard_notify_key(&keyboard, &ev);
        const xkb_keysym_t* syms = nullptr;
        const xkb_keysym_t sym = xkb_state_key_get_syms(keyboard.xkb_state, code + 8, &syms) > 0 ? syms[0] : 0;
        pressed_code = code;
        const bool super_key = sym == key::Super_L || sym == key::Super_R;
        // Alt with Shift is Meta in the usual keymaps.
        const bool alt_key = sym == key::Alt_L || sym == key::Alt_R || sym == key::Meta_L || sym == key::Meta_R;
        const bool modifier = super_key || alt_key;
        // Super let go: what the chooser or the applications hold is chosen,
        // Alt let go: the window Alt+Tab holds; Super pressed and let go
        // alone opens the apps (design 2026-10-19 §16.3).
        if (!down && modifier) {
            if (super_key && overlay == Overlay::sheet) close_overlay(false);   // the keys held on: gone, nothing opened
            else if (super_key && (overlay == Overlay::chooser || overlay == Overlay::switcher)) close_overlay(true);
            else if (alt_key && overlay == Overlay::spread && !spread_stays) close_overlay(true);
            else if (super_key && super_alone && overlay == Overlay::none) act(Action::apps);
        }
        if (down) {
            super_alone = super_key;
            if (super_key) { super_x = cursor->x; super_y = cursor->y; }
            // Held a second alone: the keys used most (design 2026-10-20 §12.4);
            // any other key meanwhile: not.
            if (super_key && hold_timer) wl_event_source_timer_update(hold_timer, 1000);
            else if (hold_timer) wl_event_source_timer_update(hold_timer, 0);
        }
        // Super+Space held as a hand: let go, the applications unless it dragged.
        if (hand && !down && (sym == key::space || super_key)) {
            hand = false;
            if (!hand_used && overlay == Overlay::none) act(Action::apps);
            pointer_moved();
            if (sym == key::space) return;
        }
        // The WuXian view with no window focused (design 2026-10-20 WuXian
        // §3.12): Esc back to a workspace, Enter into the one chosen, Delete removes it.
        if (down && overlay == Overlay::none && !launcher.is_open() && wuxian() && plane_focus
            && !held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO)) {
            if (sym == key::Escape) { change_view(); return; }
            if (ws_selected && (sym == key::Return || sym == key::KP_Enter)) { change_view(ws_selected); return; }
            if (ws_selected && sym == key::Delete) { ask_remove(ws_selected); return; }
        }
        // Dragged onto a new column: 1-4 is its width (evdev 2-5).
        if (down && code >= 2 && code <= 5 && drag_width(0, static_cast<int>(code) - 1)) return;
        // An overlay takes the keys (a modifier still reaches the windows: they track it).
        if (!modifier && overlay != Overlay::none) {
            if (overlay_key(sym, down)) return;
        }
        if (down && shortcut(code)) return;
        if (launcher.is_open()) {
            if (down) launcher_key(code);
            return;
        }
        // The settings' window focused: its keys are the settings' (Esc closes it).
        if (View* sw = settings_window(); sw && focused() == sw && !modifier) {
            if (down && sym == key::Escape && !settings_capture) wlr_xdg_toplevel_send_close(sw->toplevel);
            else if (down) settings_key(sym);
            return;
        }
        wlr_seat_set_keyboard(seat, &keyboard);
        wlr_seat_keyboard_notify_key(seat, ev.time_msec, code, static_cast<std::uint32_t>(ev.state));
        wlr_seat_keyboard_notify_modifiers(seat, &keyboard.modifiers);
    };
    // A character that needs Shift and was given without it.
    const bool add_shift = stroke->shift && !held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT) && k.down
                           && opensal::keys::modifier_of(k.keysym) == 0;
    if (add_shift) press(42, true);
    press(stroke->code, k.down);
    if (add_shift) { press(stroke->code, false); press(42, false); }
}

void Server::pointer_moved() {
    if (drag == Drag::move) { drag_motion(); return; }
    // Over the settings' window: its rows lit (design 2026-10-24 §9.6).
    if (drag == Drag::none && overlay == Overlay::none && settings_view)
        if (View* v = view_at(cursor->x, cursor->y); v && v->id == settings_view && !on_title(*v, cursor->y))
            (void)settings_window_pointer(cursor->x, cursor->y, std::nullopt);
    if (drag == Drag::workspace || drag == Drag::scale) { workspace_motion(); return; }
    if (drag == Drag::resize) { resize_motion(); return; }
    if (drag == Drag::minimap) { minimap_motion(); return; }
    if (drag != Drag::none) {
        const double dx = cursor->x - drag_x, dy = cursor->y - drag_y;
        drag_x = cursor->x;
        drag_y = cursor->y;
        if (drag == Drag::pan) {
            pointer_shape("grabbing");
            lay.pan(dx, dy);
            view_moved();
        } else if (drag == Drag::row_gap) {
            const bool vertical = lay.flow() == layout::Flow::vertical;
            pointer_shape(vertical ? "ew-resize" : "ns-resize");
            lay.drag_row_gap(row_gap.first, row_gap.second, vertical ? dx : dy);
        } else {
            const bool vertical = lay.flow() == layout::Flow::vertical;
            pointer_shape(vertical ? "ns-resize" : "ew-resize");
            lay.drag_gap(gap_column, vertical ? dy : dx);
        }
        arrange(true);
        return;
    }
    // A bookmark under the pointer: lit, the item it is for outlined.
    if (overlay == Overlay::none && !bookmarks.empty() && bookmark_hover(cursor->x, cursor->y)) {
        pointer_shape("default");
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    // The settings: the row under the pointer has its own look.
    if (overlay == Overlay::settings) {
        settings_motion(cursor->x, cursor->y);
        pointer_shape("default");
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    // The switchers: the pointer chooses among what they show.
    if (overlay == Overlay::spread || overlay == Overlay::switcher) {
        switched_hover(cursor->x, cursor->y);
        pointer_shape("default");
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    status_hover(cursor->x, cursor->y);
    if (hand) {
        pointer_shape("grab");
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    // The WuXian view: a label is moved, a corner scales.
    if (wuxian() && overlay == Overlay::none) {
        for (const auto& n : ws_nodes) {
            const layout::Rect corner{n.frame.x + n.frame.width - 18, n.frame.y + n.frame.height - 18, 27, 27};
            if (n.label_at.contains(cursor->x, cursor->y) || corner.contains(cursor->x, cursor->y)) {
                pointer_shape(n.label_at.contains(cursor->x, cursor->y) ? "grab" : "se-resize");
                wlr_seat_pointer_clear_focus(seat);
                return;
            }
        }
    }
    if (const auto hover = close_under_pointer(); hover != close_hover) {
        close_hover = hover;
        arrange();
    }
    // The maximize button: rested on, its layouts.
    if (const auto m = maximize_under_pointer(); m != max_hover) {
        max_hover = m;
        arrange();
        if (m && max_timer) wl_event_source_timer_update(max_timer, 450);
    }
    // A gap between columns: its pointer; dragged, it resizes what is either side.
    if (live() && overlay == Overlay::none && lay.gap_at(cursor->x, cursor->y)) {
        pointer_shape(lay.flow() == layout::Flow::vertical ? "ns-resize" : "ew-resize");
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    if (live() && overlay == Overlay::none && lay.row_gap_at(cursor->x, cursor->y)) {
        pointer_shape(lay.flow() == layout::Flow::vertical ? "ew-resize" : "ns-resize");
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    // A stacked, floating or canvas window's border: its size (design 2026-10-20 §8).
    if (overlay == Overlay::none)
        if (View* v = view_at(cursor->x, cursor->y))
            if (const auto e = edges_at(*v)) {
                pointer_shape(resize_shape(e));
                wlr_seat_pointer_clear_focus(seat);
                return;
            }
    double sx = 0, sy = 0;
    wlr_surface* surface = nullptr;
    if (live()) {
        wlr_scene_node* node = wlr_scene_node_at(&scene->tree.node, cursor->x, cursor->y, &sx, &sy);
        if (node && node->type == wlr_scene_node_type::WLR_SCENE_NODE_BUFFER)
            if (auto* sb = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node))) surface = sb->surface;
    }
    if (!surface) {
        pointer_shape("default");
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    // Zoomed, the surface is drawn smaller: its own coordinates are larger.
    if (const View* v = view_at(cursor->x, cursor->y); v && v->zoom != 1) sx /= v->zoom, sy /= v->zoom;
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, now_ms(), sx, sy);
}

std::optional<layout::Rect> Server::placement_of(layout::Id id) const {
    for (const auto& p : placements)
        if (p.id == id) return p.rect;
    return std::nullopt;
}

// The window whose close button is under the pointer (at zoom 1), where it is drawn.
layout::Id Server::close_under_pointer() {
    if (!live()) return 0;
    View* v = view_at(cursor->x, cursor->y);
    const auto r = v ? placement_of(v->id) : std::nullopt;
    if (!r) return 0;
    const auto b = shell::close_button(static_cast<int>(r->width));
    const double x = cursor->x - v->frame->node.x, y = cursor->y - v->frame->node.y;
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height ? v->id : 0;
}

void Server::button(const opensal::PointerButton& b) {
    // What each press met, in the flight recorder (xdesk msg telemetry recent).
    if (b.down) {
        View* at = view_at(cursor->x, cursor->y);
        telemetry::log().debug("pointer-button", {{"x", cursor->x}, {"y", cursor->y}, {"button", static_cast<int>(b.button)},
                                                  {"window", at ? at->id : 0}, {"edges", at ? edges_at(*at) : 0u},
                                                  {"overlay", static_cast<int>(overlay)}, {"wuxian", wuxian()}, {"live", live()}});
    }
    // Super with a click (a drag, a pan) is not Super alone: its release opens nothing.
    if (b.down) super_alone = false;
    if (!b.down && drag != Drag::none) {
        // Let go: a window lands where its preview was; the window under the
        // pointer says its pointer again.
        if (drag == Drag::move) drag_end();
        if (drag == Drag::minimap) minimap_release();
        if (drag == Drag::workspace || drag == Drag::scale) {
            ws_dragged = 0;
            remember_arrangement();
        }
        drag = Drag::none;
        wlr_seat_pointer_clear_focus(seat);
        pointer_moved();
        return;
    }
    // The launcher: a row starts it, anywhere else closes it.
    if (launcher.is_open()) {
        if (b.down) {
            if (auto app = launcher.choose_at(cursor->x, cursor->y)) start_app(*app);
            else launcher.close();
            draw_launcher();
        }
        return;
    }
    // The panel's buttons.
    if (const auto a = shell::hit(panel_picture, cursor->x, cursor->y)) {
        if (b.down) {
            using T = shell::Target;
            if (a->target == T::launcher) open_launcher();
            else if (a->target == T::workspace) {
                if (wuxian()) { lay.show(a->index); arrange(); }
                else switch_workspace(a->index);
            } else if (a->target == T::rule) open_chooser();
            else if (a->target == T::wuxian) change_view();
            else if (a->target == T::free) {
                if (!wuxian()) change_view();
            } else if (a->target == T::show_all) { lay.show_all(); arrange(); }
            else if (a->target == T::minimap) {
                minimap_on = !minimap_on;
                setting("wuxian", "minimap", minimap_on ? "on" : "off");
                draw_shell();
            }
        }
        return;
    }
    // The tour's card: its buttons (design 2026-10-21 §8.1).
    if (overlay == Overlay::none && b.button == opensal::Button::Left && tour_at >= 0) {
        if (b.down && tour_click(cursor->x, cursor->y)) return;
    }
    // A bookmark: what it is for (design 2026-10-21 §8.4).
    if (overlay == Overlay::none && b.button == opensal::Button::Left && !bookmarks.empty()) {
        bool on = false;
        for (const auto& bm : bookmarks) on = on || bm.shown().contains(cursor->x, cursor->y);
        if (on) {
            if (b.down) bookmark_press(cursor->x, cursor->y);
            return;
        }
    }
    // A title bar's close button; its maximize button.
    if (const auto id = close_under_pointer()) {
        if (b.down)
            if (View* v = view_of(id)) wlr_xdg_toplevel_send_close(v->toplevel);
        return;
    }
    if (const auto id = maximize_under_pointer(); id && overlay == Overlay::none) {
        if (b.down) {
            lay.toggle_maximized(id);
            arrange();
        }
        return;
    }
    // Super+Space held: a drag anywhere scrolls or pans (design 2026-10-20 WuXian §3.12).
    if (hand && overlay == Overlay::none && b.button == opensal::Button::Left) {
        if (b.down) {
            hand_used = true;
            drag = Drag::pan;
            drag_x = cursor->x;
            drag_y = cursor->y;
        }
        return;
    }
    // The WuXian view: a workspace's label, its corner, its plate, WuXian itself.
    if (overlay == Overlay::none && wuxian_button(b)) return;
    // A border: dragged, the window's size.
    if (overlay == Overlay::none && b.button == opensal::Button::Left && b.down)
        if (View* v = view_at(cursor->x, cursor->y))
            if (const auto e = edges_at(*v)) {
                resize_start(v, e);
                return;
            }
    // Between two windows of a column: dragged, their heights.
    if (overlay == Overlay::none && live() && b.button == opensal::Button::Left && b.down)
        if (const auto row = lay.row_gap_at(cursor->x, cursor->y)) {
            drag = Drag::row_gap;
            row_gap = *row;
            drag_x = cursor->x;
            drag_y = cursor->y;
            return;
        }
    // A gap between columns: dragged to resize, clicked twice to even them out.
    if (overlay == Overlay::none && live() && b.button == opensal::Button::Left)
        if (const auto gap = lay.gap_at(cursor->x, cursor->y)) {
            if (b.down) {
                const auto now = std::chrono::steady_clock::now();
                if (now - gap_pressed < std::chrono::milliseconds(400) && gap_column == *gap) {
                    lay.even_widths();
                    arrange();
                } else {
                    drag = Drag::gap;
                    gap_column = *gap;
                    drag_x = cursor->x;
                    drag_y = cursor->y;
                }
                gap_pressed = now;
            }
            return;
        }
    // An overlay is the pointer's while it is up: a click chooses, or closes it.
    if (overlay != Overlay::none) {
        if (b.down) overlay_click(cursor->x, cursor->y);
        return;
    }
    // One set of gestures in every arrangement (design 2026-10-20 §6): the
    // title bar, or Super anywhere, moves; Super with the right button
    // resizes; the background or the middle button pans; a click focuses.
    if (b.down) {
        View* v = view_at(cursor->x, cursor->y);
        // The minimap: the view goes where it is clicked.
        if (minimap_shown.per_pixel > 0 && !minimap_shown.picture.areas.empty() && b.button == opensal::Button::Left
            && shell::hit(minimap_shown.picture, cursor->x, cursor->y)) {
            minimap_press();
            return;
        }
        if (!live() && v && b.button == opensal::Button::Left && !held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO)) {
            // Zoomed far out (the WuXian view), windows are pictures (design 2026-10-20 §9.1):
            // a click selects one -- the view stays -- a second shows it 1:1,
            // and the whole picture moves it (design 2026-10-21 §8.7).
            const auto now = std::chrono::steady_clock::now();
            if (title_pressed_id == v->id && now - title_pressed < std::chrono::milliseconds(400)) {
                title_pressed_id = 0;
                lay.center(v->id);
                focus(v);
                arrange();
                return;
            }
            title_pressed_id = v->id;
            title_pressed = now;
            focus(v);
            drag_start(v, false);
            return;
        }
        const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO);
        const bool title = v && on_title(*v, cursor->y);
        // In the settings' window (not its title bar): the settings' (design 2026-10-24 §9.6).
        if (v && v->id == settings_view && !title && !super && settings_window_pointer(cursor->x, cursor->y, b)) return;
        // The title bar's other button: the window's menu (design 2026-10-19 §18.1).
        if (title && b.button == opensal::Button::Right && !super) {
            focus(v);
            open_menu(v, cursor->x, cursor->y);
            return;
        }
        // Its fold mark (a free window in the WuXian view, design 2026-10-24
        // §9.3): folded, or unfolded, at once.
        if (title && b.button == opensal::Button::Left && !super && wuxian() && lay.is_free(v->id)) {
            const auto m = shell::fold_button();
            const double x = (cursor->x - v->frame->node.x) / std::max(0.01, v->zoom);
            if (x >= m.x && x < m.x + m.width) {
                title_pressed_id = 0;
                collapse_window(v->id);
                return;
            }
        }
        // Twice on a title bar: maximized, or back -- a free window in the
        // WuXian view: collapsed, or expanded (design 2026-10-21 §8.5).
        if (title && b.button == opensal::Button::Left && !super) {
            const auto now = std::chrono::steady_clock::now();
            if (title_pressed_id == v->id && now - title_pressed < std::chrono::milliseconds(400)) {
                title_pressed_id = 0;
                if (wuxian() && lay.is_free(v->id)) {
                    collapse_window(v->id);
                    return;
                }
                lay.toggle_maximized(v->id);
                arrange();
                focus(v);
                return;
            }
            title_pressed_id = v->id;
            title_pressed = now;
        }
        // The background's other button: its menu (the workspace view).
        if (!v && b.button == opensal::Button::Right && !wuxian()) {
            open_desktop_menu(cursor->x, cursor->y);
            return;
        }
        // The background (or the middle button there, or with Super): it
        // scrolls; the middle button on a window is the application's (H10).
        if ((!v && (b.button == opensal::Button::Left || b.button == opensal::Button::Middle))
            || (b.button == opensal::Button::Middle && super)) {
            drag = Drag::pan;
            drag_x = cursor->x;
            drag_y = cursor->y;
            return;
        }
        if (v && super && b.button == opensal::Button::Right) {
            if (resize_start(v)) return;
        }
        if (v && (super || title) && b.button == opensal::Button::Left) {
            // A window moved by its title (or anywhere with Super held): it
            // follows the pointer, where it would land shown under it. A
            // floating or stacked one moves itself in the workspace view.
            drag_start(v, !wuxian() && (lay.floating(v->id) || rule_of(v->id) == layout::Rule::stack));
            return;
        }
        if (v) { focus(v); arrange(); }
    }
    wlr_seat_pointer_notify_button(seat, now_ms(), opensal::keys::evdev_button(b.button),
                                   b.down ? wl_pointer_button_state::WL_POINTER_BUTTON_STATE_PRESSED
                                          : wl_pointer_button_state::WL_POINTER_BUTTON_STATE_RELEASED);
}

void Server::wheel(const opensal::Wheel& w) {
    // The wheel with Super is not Super alone: nothing opens when it is let
    // go, and its keys' sheet does not come (H8).
    super_alone = false;
    if (hold_timer) wl_event_source_timer_update(hold_timer, 0);
    // Dragged onto a new column: the wheel is its width.
    if (w.dy != 0 && drag_width(w.dy > 0 ? 1 : -1, 0)) return;
    const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO);
    const bool over_window = view_at(cursor->x, cursor->y) != nullptr;
    // How far (design 2026-10-20 WuXian §4.4): a wheel's step slow is a
    // column, faster ones further; a touchpad's scroll is as it comes.
    const auto now = std::chrono::steady_clock::now();
    const double since = last_wheel ? std::chrono::duration<double, std::milli>(now - *last_wheel).count() : 0;
    last_wheel = now;
    const bool continuous = w.dy != std::round(w.dy) || w.dx != std::round(w.dx);
    const layout::WheelSpeed speed{.factor = wheel_factor};
    auto distance = [&](double steps) { return continuous ? steps * 60 * wheel_factor : layout::wheel_distance(speed, steps, since); };
    if (wuxian()) view_moved();
    if (wuxian() && super) {
        // The WuXian view zooms only when asked (design 2026-10-20 §9.2):
        // Super and the wheel, around the pointer, so the point under it stays put.
        if (w.dy != 0) lay.zoom_at(w.dy > 0 ? 1.1 : 1 / 1.1, cursor->x, cursor->y);
        arrange();
        return;
    }
    if (wuxian() && (!over_window || !live())) {
        // On its background (or over pictures): the wheel pans -- up and
        // down, Shift or a sideways wheel left and right.
        if (held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)) lay.pan(distance(w.dy), 0);
        else lay.pan(-distance(w.dx), distance(w.dy));
        arrange();
        return;
    }
    if (!wuxian() && lay.rule() == layout::Rule::tile && (super || !over_window)) {
        // A tiled strip: a slow step a column, landing on it; faster, further,
        // and back on a column once it stops.
        if (!continuous && since >= speed.slow_ms && snap_columns) lay.scroll_columns(w.dy > 0 ? -1 : 1);
        else {
            const double d = distance(w.dy != 0 ? w.dy : -w.dx);
            lay.pan(d, d);
            if (snap_columns && !continuous && wheel_timer) wl_event_source_timer_update(wheel_timer, 220);
        }
        arrange();
        return;
    }
    auto axis = [&](wl_pointer_axis orientation, double steps) {
        if (steps == 0) return;
        wlr_seat_pointer_notify_axis(seat, now_ms(), orientation, steps * 15.0, static_cast<std::int32_t>(steps * 120),
                                     wl_pointer_axis_source::WL_POINTER_AXIS_SOURCE_WHEEL,
                                     wl_pointer_axis_relative_direction::WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    };
    axis(wl_pointer_axis::WL_POINTER_AXIS_VERTICAL_SCROLL, -w.dy);
    axis(wl_pointer_axis::WL_POINTER_AXIS_HORIZONTAL_SCROLL, w.dx);
}

void Server::input(Link& from, const opensal::Event& ev) {
    const bool primary_link = &from == primary();
    // Somebody is here: a paused wallpaper runs again (design 2026-10-24 §9.7).
    if (!std::holds_alternative<opensal::FramePresented>(ev) && !std::holds_alternative<opensal::Shown>(ev)) {
        last_input = std::chrono::steady_clock::now();
        if (wallpaper_paused) wallpaper_pause_check();
    }
    if (const auto* k = std::get_if<opensal::Key>(&ev)) {
        key(*k);
    } else if (const auto* m = std::get_if<opensal::PointerMotion>(&ev)) {
        wlr_cursor_warp_closest(cursor, nullptr, m->x / scale, m->y / scale);   // device pixels to logical
        // Super held while the pointer goes more than a little: not Super alone (H8).
        if (super_alone && std::hypot(cursor->x - super_x, cursor->y - super_y) > 4) {
            super_alone = false;
            if (hold_timer) wl_event_source_timer_update(hold_timer, 0);
        }
        pointer_moved();
        wlr_seat_pointer_notify_frame(seat);
    } else if (const auto* b = std::get_if<opensal::PointerButton>(&ev)) {
        button(*b);
        wlr_seat_pointer_notify_frame(seat);
    } else if (const auto* w = std::get_if<opensal::Wheel>(&ev)) {
        wheel(*w);
        wlr_seat_pointer_notify_frame(seat);
    } else if (const auto* r = std::get_if<opensal::Resized>(&ev)) {
        // The primary's size is the output's; a mirror's is its own business
        // (kept, for when it is the primary).
        from.own = r->size;
        if (primary_link) resize(r->size);
    } else if (const auto* d = std::get_if<opensal::DensityChanged>(&ev)) {
        from.density = d->scale;
        if (primary_link) set_scale(d->scale);
    } else if (const auto* t = std::get_if<opensal::TextCommit>(&ev)) {
        View* sw = settings_window();
        if (launcher.is_open()) { launcher.type(t->text); draw_launcher(); }
        else if (overlay == Overlay::naming) naming_type(t->text);
        else if (overlay == Overlay::settings || (sw && focused() == sw && overlay == Overlay::none)) {
            // An input method's text: the keys page's search takes it (H14); elsewhere nothing does.
            settings_preedit.clear();
            if (settings_page == SettingsPage::keys) {
                settings_search += t->text;
                settings_row = 0;
            }
            draw_overlay();
        }
        else text_commit(t->text);
    } else if (const auto* p = std::get_if<opensal::Preedit>(&ev)) {
        if (overlay == Overlay::settings || (settings_window() && focused() == settings_window() && overlay == Overlay::none)) {
            settings_preedit = settings_page == SettingsPage::keys ? p->text : std::string{};
            draw_overlay();
        } else {
            preedit(*p);
        }
    } else if (const auto* c = std::get_if<opensal::ClipboardText>(&ev)) {
        host_clipboard(c->text);
    } else if (const auto* sh = std::get_if<opensal::Shown>(&ev)) {
        // Not seen: nothing drawn, and the clients' frames wait (design
        // 2026-10-20 §13.2). Seen again: all of it at once.
        from.hidden = !sh->shown;
        if (!from.hidden) {
            from.whole = true;
            wlr_output_schedule_frame(output);
        }
    } else if (std::holds_alternative<opensal::FramePresented>(ev)) {
        // Shown: what changed meanwhile goes now.
        from.in_flight.reset();
        wlr_output_schedule_frame(output);
    } else if (const auto* f = std::get_if<opensal::Focus>(&ev)) {
        // Focus left the window: nothing it holds stays pressed here.
        if (!f->in) {
            drag = Drag::none;
            for (std::uint32_t k = 0; k < keyboard.num_keycodes; ++k) {
                wlr_keyboard_key_event up{.time_msec = now_ms(), .keycode = keyboard.keycodes[k], .update_state = true,
                                          .state = wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_RELEASED};
                wlr_keyboard_notify_key(&keyboard, &up);
            }
        }
    }
}

void Server::provider(Link& l, std::string_view bytes) {
    l.decoder.feed(bytes);
    for (;;) {
        auto m = l.decoder.next();
        if (!m) {
            log(std::format("display {}: its stream is not opensal.wire", l.number));
            l.gone = true;
            drop_gone();
            return;
        }
        if (!*m) return;
        const auto& msg = **m;
        if (msg.kind == 'H') {
            std::istringstream in(msg.payload);
            std::uint32_t bits = 0, w = 0, h = 0;
            double density = 1;
            int pacing = 0;
            if (in >> bits >> w >> h) {
                if (in >> density >> pacing) l.paced = pacing == 1;
                // A display that comes to show the desktop at its own size
                // (opensal 0.7: "primary" after its pacing -- a window come to
                // a desktop that ran on a virtual display): it is the primary
                // now, the one before it a mirror.
                std::string asked;
                if (in >> asked && asked == "primary" && &l != primary()) {
                    for (auto it = links.begin(); it != links.end(); ++it)
                        if (&*it == &l) { links.splice(links.begin(), links, it); break; }
                    log(std::format("display {} is the primary (it asked)", l.number));
                }
                l.provided = bits;
                l.hello = true;
                l.whole = true;
                l.own = {w, h};
                if (density > 0) l.density = density;
                send_locate(&l);
                if (&l == primary()) {
                    provided = bits;
                    pointer_mode();
                    resize({w, h});
                    if (density > 0) set_scale(density);
                }
                tell_roles();
                wlr_output_schedule_frame(output);
            }
        } else if (msg.kind == 'E') {
            if (auto e = opensal::wire::decode_event(msg.payload)) input(l, *e);
        }
    }
}

}  // namespace xdesk::compositor
