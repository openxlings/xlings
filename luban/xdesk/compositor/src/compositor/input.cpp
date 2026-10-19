// The window's input: keys, the pointer, the wheel, and the desktop's own chords (server.cppm).
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import opensal.keys;
import xdesk.os;
import xdesk.layout;
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
    // A digit by its place (Shift changes what it types): evdev 2-10 are 1-9.
    if (code >= 2 && code <= 10) pressed.digit = static_cast<int>(code) - 1;
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
        const bool super_key = sym == key::Super_L || sym == key::Super_R;
        // Alt with Shift is Meta in the usual keymaps.
        const bool alt_key = sym == key::Alt_L || sym == key::Alt_R || sym == key::Meta_L || sym == key::Meta_R;
        const bool modifier = super_key || alt_key;
        // Super let go: what the chooser or the applications hold is chosen,
        // Alt let go: the window Alt+Tab holds; Super pressed and let go
        // alone opens the apps (design 2026-10-19 §16.3).
        if (!down && modifier) {
            if (super_key && (overlay == Overlay::chooser || overlay == Overlay::switcher)) close_overlay(true);
            else if (alt_key && overlay == Overlay::spread) close_overlay(true);
            else if (super_key && super_alone && overlay == Overlay::none) act(Action::apps);
        }
        if (down) super_alone = super_key;
        // Dragged onto a new column: 1-4 is its width (evdev 2-5).
        if (down && code >= 2 && code <= 5 && drag_width(0, static_cast<int>(code) - 1)) return;
        // An overlay takes the keys (a modifier still reaches the windows: they track it).
        if (!modifier && overlay != Overlay::none) {
            overlay_key(sym, down);
            return;
        }
        if (down && shortcut(code)) return;
        if (launcher.is_open()) {
            if (down) launcher_key(code);
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
    if (drag == Drag::resize) { resize_motion(); return; }
    if (drag != Drag::none) {
        const double dx = cursor->x - drag_x, dy = cursor->y - drag_y;
        drag_x = cursor->x;
        drag_y = cursor->y;
        if (drag == Drag::pan) {
            pointer_shape("grabbing");
            lay.pan(dx, dy);
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
    // The switchers: the pointer chooses among what they show.
    if (overlay == Overlay::spread || overlay == Overlay::switcher) {
        switched_hover(cursor->x, cursor->y);
        pointer_shape("default");
        wlr_seat_pointer_clear_focus(seat);
        return;
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
    // Super with a click (a drag, a pan) is not Super alone: its release opens nothing.
    if (b.down) super_alone = false;
    if (!b.down && drag != Drag::none) {
        // Let go: a window lands where its preview was; the window under the
        // pointer says its pointer again.
        if (drag == Drag::move) drag_end();
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
            if (a->target == shell::Target::launcher) open_launcher();
            else if (a->target == shell::Target::workspace) { lay.show(a->index); arrange(); follow_focus(); }
            else if (a->target == shell::Target::overview) { lay.overview(!lay.in_overview()); arrange(); }
        }
        return;
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
            lay.look_at(minimap_shown.origin_x + (cursor->x - minimap_shown.picture.x) * minimap_shown.per_pixel,
                        minimap_shown.origin_y + (cursor->y - minimap_shown.picture.y) * minimap_shown.per_pixel);
            arrange();
            return;
        }
        if (!live() && v && b.button == opensal::Button::Left && !held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO)) {
            // Zoomed far out, windows are pictures (design 2026-10-20 §9.1):
            // a click selects one -- the view stays -- and a second goes in.
            const auto now = std::chrono::steady_clock::now();
            if (title_pressed_id == v->id && now - title_pressed < std::chrono::milliseconds(400)) {
                title_pressed_id = 0;
                lay.center(v->id);
            } else {
                title_pressed_id = v->id;
                title_pressed = now;
            }
            focus(v);
            arrange();
            return;
        }
        const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO);
        const bool title = v && on_title(*v, cursor->y);
        // The title bar's other button: the window's menu (design 2026-10-19 §18.1).
        if (title && b.button == opensal::Button::Right && !super) {
            focus(v);
            open_menu(v, cursor->x, cursor->y);
            return;
        }
        // Twice on a title bar: maximized, or back.
        if (title && b.button == opensal::Button::Left && !super) {
            const auto now = std::chrono::steady_clock::now();
            if (title_pressed_id == v->id && now - title_pressed < std::chrono::milliseconds(400)) {
                title_pressed_id = 0;
                lay.toggle_maximized(v->id);
                arrange();
                focus(v);
                return;
            }
            title_pressed_id = v->id;
            title_pressed = now;
        }
        if (b.button == opensal::Button::Middle || (!v && b.button == opensal::Button::Left)) {
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
            // floating or stacked one, or a canvas group, moves itself.
            drag_start(v, lay.floating(v->id) || lay.mode() == layout::Mode::stack || lay.mode() == layout::Mode::canvas);
            return;
        }
        if (v) { focus(v); arrange(); }
    }
    wlr_seat_pointer_notify_button(seat, now_ms(), opensal::keys::evdev_button(b.button),
                                   b.down ? wl_pointer_button_state::WL_POINTER_BUTTON_STATE_PRESSED
                                          : wl_pointer_button_state::WL_POINTER_BUTTON_STATE_RELEASED);
}

void Server::wheel(const opensal::Wheel& w) {
    // Dragged onto a new column: the wheel is its width.
    if (w.dy != 0 && drag_width(w.dy > 0 ? 1 : -1, 0)) return;
    const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO);
    const bool over_window = view_at(cursor->x, cursor->y) != nullptr;
    if (lay.mode() == layout::Mode::canvas && super) {
        // The canvas zooms only when asked (design 2026-10-20 §9.2): Super
        // and the wheel, around the pointer, so the point under it stays put.
        if (w.dy != 0) lay.zoom_at(w.dy > 0 ? 1.1 : 1 / 1.1, cursor->x, cursor->y);
        arrange();
        return;
    }
    if (lay.mode() == layout::Mode::canvas && (!over_window || !live())) {
        // On its background (or over pictures): the wheel pans -- up and
        // down, Shift or a sideways wheel left and right.
        if (held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT)) lay.pan(w.dy * 60, 0);
        else lay.pan(-w.dx * 60, w.dy * 60);
        arrange();
        return;
    }
    if (lay.mode() == layout::Mode::tile && super) {
        lay.pan(w.dy * 60, w.dy * 60);
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

void Server::input(const opensal::Event& ev) {
    if (const auto* k = std::get_if<opensal::Key>(&ev)) {
        key(*k);
    } else if (const auto* m = std::get_if<opensal::PointerMotion>(&ev)) {
        wlr_cursor_warp_closest(cursor, nullptr, m->x / scale, m->y / scale);   // device pixels to logical
        pointer_moved();
        wlr_seat_pointer_notify_frame(seat);
    } else if (const auto* b = std::get_if<opensal::PointerButton>(&ev)) {
        button(*b);
        wlr_seat_pointer_notify_frame(seat);
    } else if (const auto* w = std::get_if<opensal::Wheel>(&ev)) {
        wheel(*w);
        wlr_seat_pointer_notify_frame(seat);
    } else if (const auto* r = std::get_if<opensal::Resized>(&ev)) {
        resize(r->size);
    } else if (const auto* d = std::get_if<opensal::DensityChanged>(&ev)) {
        set_scale(d->scale);
    } else if (const auto* t = std::get_if<opensal::TextCommit>(&ev)) {
        if (launcher.is_open()) { launcher.type(t->text); draw_launcher(); }
        else text_commit(t->text);
    } else if (const auto* p = std::get_if<opensal::Preedit>(&ev)) {
        preedit(*p);
    } else if (const auto* c = std::get_if<opensal::ClipboardText>(&ev)) {
        host_clipboard(c->text);
    } else if (const auto* sh = std::get_if<opensal::Shown>(&ev)) {
        // Not seen: nothing drawn, and the clients' frames wait (design
        // 2026-10-20 §13.2). Seen again: all of it at once.
        hidden = !sh->shown;
        if (!hidden) {
            send_whole = true;
            wlr_output_schedule_frame(output);
        }
    } else if (std::holds_alternative<opensal::FramePresented>(ev)) {
        // Shown: what changed meanwhile goes now.
        in_flight.reset();
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

void Server::provider(std::string_view bytes) {
    decoder.feed(bytes);
    for (;;) {
        auto m = decoder.next();
        if (!m) { log("the provider's stream is not opensal.wire"); stop = true; return; }
        if (!*m) return;
        const auto& msg = **m;
        if (msg.kind == 'H') {
            std::istringstream in(msg.payload);
            std::uint32_t bits = 0, w = 0, h = 0;
            double density = 1;
            int pacing = 0;
            if (in >> bits >> w >> h) {
                if (in >> density >> pacing) paced = pacing == 1;
                provided = bits;
                pointer_mode();
                resize({w, h});
                if (density > 0) set_scale(density);
            }
        } else if (msg.kind == 'E') {
            if (auto e = opensal::wire::decode_event(msg.payload)) input(*e);
        }
    }
}

}  // namespace xdesk::compositor
