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

// A chord the desktop keeps for itself: Super, or Alt where the host
// keeps Super (docs/luban/08-desktop.md lists them).
bool Server::shortcut(std::uint32_t code) {
    const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO);
    const bool alt = held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
    const bool shift = held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT);
    if (!super && !alt) return false;
    // Workspaces by the digit keys' codes (Shift changes their symbols).
    if (code >= 2 && code <= 5) {
        const int n = static_cast<int>(code) - 2;
        if (shift) { if (const auto f = lay.focused()) lay.send_to(*f, n); }
        else lay.show(n);
        arrange();
        follow_focus();
        return true;
    }
    const xkb_keysym_t* syms = nullptr;
    const int n = xkb_state_key_get_syms(keyboard.xkb_state, code + 8, &syms);
    for (int k = 0; k < n; ++k) {
        const auto sym = xkb_keysym_to_lower(syms[k]);
        auto way = [&](layout::Direction d) {
            if (shift) lay.move(d); else lay.move_focus(d);
            arrange();
            follow_focus();
            return true;
        };
        switch (sym) {
            case key::Return: launch(terminal_argv({})); return true;
            case key::space: case key::d:
                if (launcher.is_open()) { launcher.close(); draw_launcher(); }
                else open_launcher();
                return true;
            case key::q: if (focused()) wlr_xdg_toplevel_send_close(focused()->toplevel); return true;
            case key::Left: case key::h: return way(layout::Direction::left);
            case key::Right: case key::l: return way(layout::Direction::right);
            case key::Up: case key::k: return way(layout::Direction::up);
            case key::Down: case key::j: return way(layout::Direction::down);
            case key::f:
                // Stacked: the whole output and back; else floating and back.
                if (const auto f = lay.focused()) {
                    if (lay.mode() == layout::Mode::stack && !lay.floating(*f)) lay.toggle_maximized(*f);
                    else lay.toggle_floating(*f);
                    arrange();
                }
                return true;
            case key::r:
                if (shift) lay.cycle_height(); else lay.cycle_width();
                arrange();
                return true;
            case key::m:
                // The next arrangement for this workspace (design 2026-10-19 §17.3).
                lay.set_mode(layout::next(lay.mode()));
                arrange();
                draw_shell();
                follow_focus();
                return true;
            case key::bracketleft: lay.join(); arrange(); follow_focus(); return true;
            case key::bracketright: lay.split(); arrange(); follow_focus(); return true;
            case key::minus: case key::equal: case key::plus: {
                const bool in = sym != key::minus;
                // The canvas: the view; else the focused column or window.
                if (canvas()) lay.zoom_at(in ? 1.1 : 1 / 1.1, logical_w() / 2.0, logical_h() / 2.0);
                else lay.resize(in ? 0.05 : -0.05);
                arrange();
                return true;
            }
            case key::o: lay.overview(!lay.in_overview()); arrange(); return true;
            case key::Tab:
                if (!alt) break;
                // The least recently focused window to the front.
                for (auto it = views.rbegin(); it != views.rend(); ++it)
                    if (it->mapped && &*it != focused() && lay.workspace_of(it->id) == lay.workspace()) {
                        focus(&*it);
                        arrange();
                        return true;
                    }
                return true;
            default: break;
        }
    }
    return false;
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
    if (drag != Drag::none) {
        const double dx = cursor->x - drag_x, dy = cursor->y - drag_y;
        drag_x = cursor->x;
        drag_y = cursor->y;
        pointer_shape("grabbing");
        if (drag == Drag::pan) {
            lay.pan(dx, dy);
        } else if (const auto p = placement_of(dragged)) {
            lay.place(dragged, {p->x + dx, p->y + dy, p->width, p->height});
        }
        arrange(true);
        return;
    }
    if (const auto hover = close_under_pointer(); hover != close_hover) {
        close_hover = hover;
        arrange();
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
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, now_ms(), sx, sy);
}

std::optional<layout::Rect> Server::placement_of(layout::Id id) const {
    for (const auto& p : placements)
        if (p.id == id) return p.rect;
    return std::nullopt;
}

// The window whose close button is under the pointer (at zoom 1).
layout::Id Server::close_under_pointer() const {
    if (!live()) return 0;
    const auto id = lay.at(cursor->x, cursor->y);
    if (!id) return 0;
    const auto r = placement_of(*id);
    if (!r) return 0;
    const auto b = shell::close_button(static_cast<int>(r->width));
    const double x = cursor->x - r->x, y = cursor->y - r->y;
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height ? *id : 0;
}

void Server::button(const opensal::PointerButton& b) {
    if (!b.down && drag != Drag::none) {
        // Let go: the window under the pointer says its pointer again.
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
    // A title bar's close button.
    if (const auto id = close_under_pointer()) {
        if (b.down)
            if (View* v = view_of(id)) wlr_xdg_toplevel_send_close(v->toplevel);
        return;
    }
    const auto hit = lay.at(cursor->x, cursor->y);
    if (b.down) {
        View* v = hit ? view_of(*hit) : nullptr;
        if (!live()) {
            // Zoomed out: a click goes in to the window under it.
            if (v) { lay.center(v->id); arrange(); focus(v); }
            return;
        }
        const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO) || held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
        const bool on_title = v && cursor->y - v->frame->node.y < kTitle;
        if (lay.mode() == layout::Mode::canvas && (super || !v)) {
            // The canvas: dragging its background -- or anywhere, with Super -- pans.
            drag = Drag::pan;
            drag_x = cursor->x;
            drag_y = cursor->y;
            return;
        }
        if (v && (super || on_title) && (lay.floating(v->id) || lay.mode() != layout::Mode::tile)) {
            // A window moved by its title (or anywhere with Super held).
            focus(v);
            drag = Drag::move;
            dragged = v->id;
            drag_x = cursor->x;
            drag_y = cursor->y;
            return;
        }
        if (v) { focus(v); arrange(); }
    }
    wlr_seat_pointer_notify_button(seat, now_ms(), opensal::keys::evdev_button(b.button),
                                   b.down ? wl_pointer_button_state::WL_POINTER_BUTTON_STATE_PRESSED
                                          : wl_pointer_button_state::WL_POINTER_BUTTON_STATE_RELEASED);
}

void Server::wheel(const opensal::Wheel& w) {
    const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO) || held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
    const bool over_window = lay.at(cursor->x, cursor->y).has_value();
    if (lay.mode() == layout::Mode::canvas && (super || !over_window || !live())) {
        // The canvas: the wheel on its background (or with Super, anywhere)
        // zooms around the pointer, so the point under it stays put; a
        // sideways scroll pans.
        if (w.dy != 0) lay.zoom_at(w.dy > 0 ? 1.1 : 1 / 1.1, cursor->x, cursor->y);
        if (w.dx != 0) lay.pan(-w.dx * 40, 0);
        arrange();
        return;
    }
    if (lay.mode() == layout::Mode::tile && super) {
        lay.pan(w.dy * 60, 0);
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
