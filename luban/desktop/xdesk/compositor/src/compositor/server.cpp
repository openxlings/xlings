module;

#include <pixman.h>
#include <xkbcommon/xkbcommon.h>
// The wlroots module does not export these two creators yet (its generator
// misses a declaration whose return type is on the line before); the module
// and the header declare the same functions, so both may be seen.
#define WLR_USE_UNSTABLE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
extern "C" {
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
}

module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import opensal.keys;
import xdesk.os;
import xdesk.layout;
import wlroots;
import freedesktop.wayland.server;

namespace xdesk::compositor {

namespace {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace fs = std::filesystem;

// A wl_listener that calls a function: the C++ side of wl_signal.
struct Hook {
    wl_listener listener {};
    std::function<void(void*)> fn;
    static void notify(wl_listener* l, void* data) {
        // `listener` is the first member: the Hook is where it is.
        reinterpret_cast<Hook*>(l)->fn(data);
    }
    void connect(wl_signal* signal, std::function<void(void*)> f) {
        fn = std::move(f);
        listener.notify = &Hook::notify;
        wl_signal_add(signal, &listener);
    }
    void disconnect() {
        if (fn) { wl_list_remove(&listener.link); fn = nullptr; }
    }
    ~Hook() { disconnect(); }
};

constexpr int kTitle = 26;                 // a window's title bar, in pixels
constexpr float kBackground[4] = {0.106f, 0.118f, 0.145f, 1.0f};
constexpr float kTitleActive[4] = {0.196f, 0.282f, 0.408f, 1.0f};
constexpr float kTitleIdle[4] = {0.157f, 0.169f, 0.200f, 1.0f};
constexpr layout::Size kPreferred { 900, 600 };   // a new window's frame, until it says

struct Server;

struct View {
    Server* server { nullptr };
    layout::Id id { 0 };
    wlr_xdg_toplevel* toplevel { nullptr };
    wlr_scene_tree* frame { nullptr };          // the decoration and the content
    wlr_scene_rect* title { nullptr };
    wlr_scene_tree* content { nullptr };
    bool mapped { false };
    bool laid { false };                        // in the layout
    bool placed { false };                      // shown somewhere already (then it glides)
    bool moving { false };                      // its frame is on its way (springs)
    layout::Spring x, y;
    Hook map, unmap, commit, destroy, request_maximize, request_fullscreen;
};

struct Popup {
    wlr_xdg_popup* popup { nullptr };
    Hook commit, destroy;
};

// What a held button is doing.
enum class Drag { none, pan, move };

struct Server {
    const Options& o;
    wl_display* display { nullptr };
    wl_event_loop* loop { nullptr };
    wlr_backend* backend { nullptr };
    wlr_renderer* renderer { nullptr };
    wlr_allocator* allocator { nullptr };
    wlr_scene* scene { nullptr };
    wlr_scene_output_layout* scene_layout { nullptr };
    wlr_output_layout* outputs { nullptr };
    wlr_scene_tree* background_layer { nullptr };
    wlr_scene_tree* window_layer { nullptr };
    wlr_scene_rect* background { nullptr };
    wlr_output* output { nullptr };
    wlr_scene_output* scene_output { nullptr };
    wlr_xdg_shell* xdg_shell { nullptr };
    wlr_cursor* cursor { nullptr };
    wlr_xcursor_manager* cursors { nullptr };
    wlr_seat* seat { nullptr };
    wlr_keyboard keyboard {};
    xkb_context* xkb { nullptr };
    std::string socket;
    std::list<View> views;                       // front: focused most recently
    std::list<Popup> popups;
    std::vector<int> children;
    opensal::Size size;
    opensal::Size told;
    bool stop { false };
    std::chrono::steady_clock::time_point start { std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point last_tick { start };
    opensal::wire::Decoder decoder;
    Hook new_toplevel, new_popup, new_decoration, frame, request_cursor, request_selection;
    wl_event_source* input_source { nullptr };
    wl_event_source* tick { nullptr };
    layout::Layout lay;
    layout::Id next_id { 1 };
    double zoom_shown { 1 };                     // the zoom the windows were last scaled to
    Drag drag { Drag::none };
    layout::Id dragged { 0 };
    double drag_x { 0 }, drag_y { 0 };           // where the pointer was

    explicit Server(const Options& opts)
        : o(opts), size(opts.size),
          lay({.mode = opts.profile == "canvas" ? layout::Mode::canvas : layout::Mode::tile, .gap = 8}) {}

    std::uint32_t now_ms() const {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }
    void log(std::string_view s) const { if (o.log) o.log(s); }
    bool send(std::string_view bytes) {
        if (!os::write_all(o.out, bytes)) { stop = true; return false; }
        return true;
    }

    // ── windows ──────────────────────────────────────────────────────

    View* view_of(layout::Id id) {
        for (auto& v : views) if (v.id == id) return &v;
        return nullptr;
    }
    View* focused() { return views.empty() || !views.front().mapped ? nullptr : &views.front(); }

    void focus(View* v) {
        if (!v || !v->mapped) return;
        if (View* was = focused(); was && was != v) {
            wlr_xdg_toplevel_set_activated(was->toplevel, false);
            wlr_scene_rect_set_color(was->title, kTitleIdle);
        }
        // To the front of the list and of the scene.
        for (auto it = views.begin(); it != views.end(); ++it)
            if (&*it == v) { views.splice(views.begin(), views, it); break; }
        wlr_scene_node_raise_to_top(&v->frame->node);
        wlr_xdg_toplevel_set_activated(v->toplevel, true);
        wlr_scene_rect_set_color(v->title, kTitleActive);
        wlr_seat_keyboard_notify_enter(seat, v->toplevel->base->surface, keyboard.keycodes, keyboard.num_keycodes,
                                       &keyboard.modifiers);
        if (v->laid && lay.focused() != v->id) lay.focus(v->id);
    }

    // The layout's focus, given to its window.
    void follow_focus() {
        if (const auto id = lay.focused())
            if (View* v = view_of(*id); v && v != focused()) focus(v);
    }

    // Where the layout puts everything: sizes now, positions by springs
    // (at once when `instant`: a window dragged follows the pointer).
    void arrange(bool instant = false) {
        const auto placements = lay.arrange({static_cast<double>(size.width), static_cast<double>(size.height)});
        const double zoom = lay.zoom();
        for (auto& v : views) {
            const auto p = std::ranges::find(placements, v.id, &layout::Placement::id);
            wlr_scene_node_set_enabled(&v.frame->node, v.mapped && p != placements.end());
            if (p == placements.end()) continue;
            const int title = static_cast<int>(std::lround(kTitle * zoom));
            wlr_scene_rect_set_size(v.title, std::max(1, static_cast<int>(std::lround(p->rect.width))), title);
            wlr_scene_node_set_position(&v.content->node, 0, title);
            // The client is asked for its size at zoom 1 (the canvas scales it).
            const int cw = std::max(1, static_cast<int>(std::lround(p->rect.width / zoom)));
            const int ch = std::max(1, static_cast<int>(std::lround(p->rect.height / zoom)) - kTitle);
            if (v.toplevel->base->initialized) wlr_xdg_toplevel_set_size(v.toplevel, cw, ch);
            // The first place is taken at once; after that the frame glides.
            v.x.target = p->rect.x;
            v.y.target = p->rect.y;
            if (!v.placed || instant) { v.x.snap(); v.y.snap(); }
            v.placed = v.placed || v.mapped;
            v.moving = v.x.value != v.x.target || v.y.value != v.y.target;
            wlr_scene_node_set_position(&v.frame->node, static_cast<int>(std::lround(v.x.value)), static_cast<int>(std::lround(v.y.value)));
        }
        // Stacking: floating windows above the rest.
        for (const auto& p : placements)
            if (p.floating)
                if (View* v = view_of(p.id)) wlr_scene_node_raise_to_top(&v->frame->node);
        scale(zoom);
        animate();
    }

    // The canvas's zoom on each window's surfaces (a surface's own commit
    // sets its size back, so this runs before each frame while zoomed).
    void scale(double zoom) {
        if (zoom == 1 && zoom_shown == 1) return;
        for (auto& v : views) {
            wlr_scene_node_for_each_buffer(&v.content->node, [](wlr_scene_buffer* b, int, int, void* data) {
                const double z = *static_cast<double*>(data);
                auto* s = wlr_scene_surface_try_from_buffer(b);
                if (!s) return;
                if (z == 1) wlr_scene_buffer_set_dest_size(b, s->surface->current.width, s->surface->current.height);
                else wlr_scene_buffer_set_dest_size(b, std::max(1, static_cast<int>(s->surface->current.width * z)),
                                                    std::max(1, static_cast<int>(s->surface->current.height * z)));
            }, &zoom);
        }
        zoom_shown = zoom;
    }

    // Frames on their way: a step per tick until every spring has settled.
    void animate() {
        bool any = false;
        for (const auto& v : views) any = any || v.moving;
        if (any && tick) wl_event_source_timer_update(tick, 16);
    }

    void step() {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::min(0.05, std::chrono::duration<double>(now - last_tick).count());
        last_tick = now;
        bool any = false;
        for (auto& v : views) {
            if (!v.moving) continue;
            v.x.step(dt);
            v.y.step(dt);
            v.moving = !(v.x.value == v.x.target && v.y.value == v.y.target);
            wlr_scene_node_set_position(&v.frame->node, static_cast<int>(std::lround(v.x.value)), static_cast<int>(std::lround(v.y.value)));
            any = any || v.moving;
        }
        if (any) wl_event_source_timer_update(tick, 16);
    }

    void lay_out(View& v) {
        if (v.laid) return;
        auto* t = v.toplevel;
        // A dialog (it has a parent, or one fixed size) floats.
        const bool fixed = t->current.min_width > 0 && t->current.min_width == t->current.max_width
                           && t->current.min_height > 0 && t->current.min_height == t->current.max_height;
        const bool floats = t->parent != nullptr || fixed;
        layout::Size preferred = kPreferred;
        if (fixed) preferred = {static_cast<double>(t->current.min_width), static_cast<double>(t->current.min_height + kTitle)};
        lay.add(v.id, preferred, floats);
        v.laid = true;
    }

    void add_view(wlr_xdg_toplevel* toplevel) {
        auto& v = views.emplace_back();
        v.server = this;
        v.id = next_id++;
        v.toplevel = toplevel;
        v.frame = wlr_scene_tree_create(window_layer);
        wlr_scene_node_set_enabled(&v.frame->node, false);
        v.title = wlr_scene_rect_create(v.frame, 1, kTitle, kTitleIdle);
        v.content = wlr_scene_xdg_surface_create(v.frame, toplevel->base);
        wlr_scene_node_set_position(&v.content->node, 0, kTitle);
        v.content->node.data = &v;
        toplevel->base->data = v.content;
        View* self = &v;
        v.commit.connect(&toplevel->base->surface->events.commit, [this, self](void*) {
            // The first commit is the client asking what to be: the layout answers.
            if (self->toplevel->base->initial_commit) {
                lay_out(*self);
                wlr_xdg_toplevel_set_size(self->toplevel, 0, 0);
                arrange();
            }
        });
        v.map.connect(&toplevel->base->surface->events.map, [this, self](void*) {
            self->mapped = true;
            lay_out(*self);
            // A floating window keeps the size it drew, centred where the layout put it.
            if (lay.floating(self->id)) {
                const wlr_box g = self->toplevel->base->geometry;
                const double w = std::max(g.width, 1), h = std::max(g.height, 1) + kTitle;
                lay.place(self->id, {(size.width - w) / 2, (size.height - h) / 2, w, h});
            }
            lay.focus(self->id);
            arrange();
            focus(self);
        });
        v.unmap.connect(&toplevel->base->surface->events.unmap, [this, self](void*) {
            self->mapped = false;
            self->laid = false;
            self->placed = false;
            lay.remove(self->id);
            wlr_scene_node_set_enabled(&self->frame->node, false);
            arrange();
            follow_focus();
            if (!focused()) for (auto& other : views) if (other.mapped) { focus(&other); break; }
        });
        v.request_maximize.connect(&toplevel->events.request_maximize, [self](void*) {
            if (self->toplevel->base->initialized) wlr_xdg_surface_schedule_configure(self->toplevel->base);
        });
        v.request_fullscreen.connect(&toplevel->events.request_fullscreen, [self](void*) {
            if (self->toplevel->base->initialized) wlr_xdg_surface_schedule_configure(self->toplevel->base);
        });
        v.destroy.connect(&toplevel->events.destroy, [this, self](void*) {
            if (self->laid) lay.remove(self->id);
            for (auto it = views.begin(); it != views.end(); ++it)
                if (&*it == self) {
                    it->map.disconnect(); it->unmap.disconnect(); it->commit.disconnect(); it->destroy.disconnect();
                    it->request_maximize.disconnect(); it->request_fullscreen.disconnect();
                    wlr_scene_node_destroy(&it->frame->node);
                    views.erase(it);
                    break;
                }
            arrange();
        });
    }

    void add_popup(wlr_xdg_popup* popup) {
        // Into its parent's scene tree, where the parent's content is.
        wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
        if (!parent || !parent->data) return;
        auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
        popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);
        auto& p = popups.emplace_back();
        p.popup = popup;
        Popup* self = &p;
        p.commit.connect(&popup->base->surface->events.commit, [self](void*) {
            if (self->popup->base->initial_commit) wlr_xdg_surface_schedule_configure(self->popup->base);
        });
        p.destroy.connect(&popup->events.destroy, [this, self](void*) {
            for (auto it = popups.begin(); it != popups.end(); ++it)
                if (&*it == self) { it->commit.disconnect(); it->destroy.disconnect(); popups.erase(it); break; }
        });
    }

    // ── programs ─────────────────────────────────────────────────────

    void launch(const std::vector<std::string>& argv) {
        if (argv.empty()) return;
        auto env = os::environment();
        env["WAYLAND_DISPLAY"] = socket;
        env["XDG_SESSION_TYPE"] = "wayland";
        env["GDK_BACKEND"] = "wayland";
        env["QT_QPA_PLATFORM"] = "wayland";
        env["MOZ_ENABLE_WAYLAND"] = "1";
        env.erase("DISPLAY");
        const fs::path home = env.contains("HOME") ? fs::path(env.at("HOME")) : fs::path{};
        const int pid = os::spawn({.argv = argv, .env = env, .cwd = home});
        if (pid > 0) children.push_back(pid);
        else log("cannot start " + argv[0]);
    }

    // ── input ────────────────────────────────────────────────────────

    bool held(wlr_keyboard_modifier m) {
        return wlr_keyboard_get_modifiers(&keyboard) & static_cast<std::uint32_t>(m);
    }

    // A chord the desktop keeps for itself: Super, or Alt where the host
    // keeps Super (docs/luban/08-desktop.md lists them).
    bool shortcut(std::uint32_t code) {
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
                case XKB_KEY_Return: launch(o.terminal); return true;
                case XKB_KEY_q: if (focused()) wlr_xdg_toplevel_send_close(focused()->toplevel); return true;
                case XKB_KEY_Left: case XKB_KEY_h: return way(layout::Direction::left);
                case XKB_KEY_Right: case XKB_KEY_l: return way(layout::Direction::right);
                case XKB_KEY_Up: case XKB_KEY_k: return way(layout::Direction::up);
                case XKB_KEY_Down: case XKB_KEY_j: return way(layout::Direction::down);
                case XKB_KEY_f: if (const auto f = lay.focused()) { lay.toggle_floating(*f); arrange(); } return true;
                case XKB_KEY_r: lay.cycle_width(); arrange(); return true;
                case XKB_KEY_o: lay.overview(!lay.in_overview()); arrange(); return true;
                case XKB_KEY_Tab:
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

    void key(const opensal::Key& k) {
        if (k.repeat) return;   // clients repeat a held key themselves
        auto stroke = opensal::keys::evdev_key(k.keysym);
        if (!stroke) return;
        auto press = [this](std::uint32_t code, bool down) {
            wlr_keyboard_key_event ev{.time_msec = now_ms(), .keycode = code, .update_state = true,
                                      .state = down ? wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_PRESSED
                                                    : wl_keyboard_key_state::WL_KEYBOARD_KEY_STATE_RELEASED};
            wlr_keyboard_notify_key(&keyboard, &ev);
            if (down && shortcut(code)) return;
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

    // Pointer input goes to windows only at zoom 1 (zoomed out, a click picks one).
    bool live() const { return lay.zoom() == 1; }

    void pointer_moved() {
        if (drag != Drag::none) {
            const double dx = cursor->x - drag_x, dy = cursor->y - drag_y;
            drag_x = cursor->x;
            drag_y = cursor->y;
            if (drag == Drag::pan) {
                lay.pan(dx, dy);
            } else if (const auto p = placement_of(dragged)) {
                lay.place(dragged, {p->x + dx, p->y + dy, p->width, p->height});
            }
            arrange(true);
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
            wlr_cursor_set_xcursor(cursor, cursors, "default");
            wlr_seat_pointer_clear_focus(seat);
            return;
        }
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, now_ms(), sx, sy);
    }

    std::optional<layout::Rect> placement_of(layout::Id id) {
        for (const auto& p : lay.arrange({static_cast<double>(size.width), static_cast<double>(size.height)}))
            if (p.id == id) return p.rect;
        return std::nullopt;
    }

    void button(const opensal::PointerButton& b) {
        if (!b.down && drag != Drag::none) { drag = Drag::none; return; }
        const auto hit = lay.at(cursor->x, cursor->y);
        if (b.down) {
            View* v = hit ? view_of(*hit) : nullptr;
            if (!live()) {
                // Zoomed out: a click goes in to the window under it.
                if (v) { lay.focus(v->id); lay.overview(false); arrange(); focus(v); }
                return;
            }
            const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO) || held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
            const bool on_title = v && cursor->y - v->frame->node.y < kTitle;
            if (v && (super || on_title) && (lay.floating(v->id) || lay.mode() == layout::Mode::canvas)) {
                // A window moved by its title (or anywhere with Super held).
                focus(v);
                drag = Drag::move;
                dragged = v->id;
                drag_x = cursor->x;
                drag_y = cursor->y;
                return;
            }
            if (!v && lay.mode() == layout::Mode::canvas) {
                // The canvas's background: dragging it pans.
                drag = Drag::pan;
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

    void wheel(const opensal::Wheel& w) {
        const bool super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO) || held(wlr_keyboard_modifier::WLR_MODIFIER_ALT);
        const bool over_window = lay.at(cursor->x, cursor->y).has_value();
        if (lay.mode() == layout::Mode::canvas && (super || !over_window || !live())) {
            // The canvas: Super+wheel zooms around the pointer, the wheel elsewhere pans.
            if (super) lay.zoom_at(w.dy > 0 ? 1.1 : 1 / 1.1, cursor->x, cursor->y);
            else lay.pan(w.dx * 40, w.dy * 40);
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

    void input(const opensal::Event& ev) {
        if (const auto* k = std::get_if<opensal::Key>(&ev)) {
            key(*k);
        } else if (const auto* m = std::get_if<opensal::PointerMotion>(&ev)) {
            wlr_cursor_warp_closest(cursor, nullptr, m->x, m->y);
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

    void provider(std::string_view bytes) {
        decoder.feed(bytes);
        for (;;) {
            auto m = decoder.next();
            if (!m) { log("the provider's stream is not opensal.wire"); stop = true; return; }
            if (!*m) return;
            const auto& msg = **m;
            if (msg.kind == 'H') {
                std::istringstream in(msg.payload);
                std::uint32_t bits = 0, w = 0, h = 0;
                if (in >> bits >> w >> h) resize({w, h});
            } else if (msg.kind == 'E') {
                if (auto e = opensal::wire::decode_event(msg.payload)) input(*e);
            }
        }
    }

    // ── output ───────────────────────────────────────────────────────

    void resize(opensal::Size want) {
        if (want.width == 0 || want.height == 0 || want == size) return;
        wlr_output_state st;
        wlr_output_state_init(&st);
        wlr_output_state_set_custom_mode(&st, static_cast<std::int32_t>(want.width), static_cast<std::int32_t>(want.height), 0);
        if (wlr_output_commit_state(output, &st)) {
            size = want;
            wlr_scene_rect_set_size(background, static_cast<int>(want.width), static_cast<int>(want.height));
            arrange();
        } else {
            log(std::format("the output did not take {}x{}", want.width, want.height));
        }
        wlr_output_state_finish(&st);
    }

    // The frame wlroots rendered: its damaged rectangles, as opensal frames.
    void render() {
        wlr_output_state st;
        wlr_output_state_init(&st);
        if (zoom_shown != 1) scale(zoom_shown);
        if (!wlr_scene_output_build_state(scene_output, &st, nullptr) || !st.buffer) {
            wlr_output_state_finish(&st);
            return;
        }
        void* data = nullptr;
        std::uint32_t format = 0;
        std::size_t stride = 0;
        const int bw = st.buffer->width, bh = st.buffer->height;
        if (wlr_buffer_begin_data_ptr_access(st.buffer, wlr_buffer_data_ptr_access_flag::WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                             &data, &format, &stride)) {
            const opensal::Size s{static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh)};
            if (s != told) {
                send(opensal::wire::encode('s', std::format("{} {}", s.width, s.height)));
                told = s;
            }
            std::vector<opensal::Rect> rects;
            const bool whole = !(st.committed & static_cast<std::uint32_t>(wlr_output_state_field::WLR_OUTPUT_STATE_DAMAGE));
            if (!whole) {
                int n = 0;
                const pixman_box32_t* boxes = pixman_region32_rectangles(&st.damage, &n);
                for (int k = 0; k < n; ++k)
                    rects.push_back({boxes[k].x1, boxes[k].y1, static_cast<std::uint32_t>(boxes[k].x2 - boxes[k].x1),
                                     static_cast<std::uint32_t>(boxes[k].y2 - boxes[k].y1)});
            }
            if (whole || rects.size() > 8) rects = {opensal::Rect{0, 0, s.width, s.height}};
            const auto* px = static_cast<const std::uint8_t*>(data);
            for (const auto& r : rects) {
                if (r.width == 0 || r.height == 0) continue;
                std::string payload = std::format("{} {} {} {} {}\n", r.x, r.y, r.width, r.height, r.width * 4);
                const auto at = payload.size();
                payload.resize(at + std::size_t(r.width) * r.height * 4);
                for (std::uint32_t y = 0; y < r.height; ++y) {
                    auto* dst = payload.data() + at + std::size_t(y) * r.width * 4;
                    std::memcpy(dst, px + (std::size_t(r.y) + y) * stride + std::size_t(r.x) * 4, std::size_t(r.width) * 4);
                    // XRGB's fourth byte is undefined; opensal's pixels are opaque there.
                    for (std::uint32_t x = 0; x < r.width; ++x) dst[x * 4 + 3] = static_cast<char>(0xff);
                }
                if (!send(opensal::wire::encode('f', payload))) break;
            }
            wlr_buffer_end_data_ptr_access(st.buffer);
        }
        wlr_output_commit_state(output, &st);
        wlr_output_state_finish(&st);
        // Frame callbacks are timed on the monotonic clock.
        const auto t = std::chrono::steady_clock::now().time_since_epoch();
        std::timespec now{};
        now.tv_sec = static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(t).count());
        now.tv_nsec = static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count() % 1'000'000'000);
        wlr_scene_output_send_frame_done(scene_output, &now);
    }

    // ── start ────────────────────────────────────────────────────────

    bool start_() {
        display = wl_display_create();
        loop = wl_display_get_event_loop(display);
        backend = wlr_headless_backend_create(loop);
        renderer = wlr_pixman_renderer_create();
        if (!backend || !renderer) { log("no headless backend or software renderer"); return false; }
        wlr_renderer_init_wl_display(renderer, display);
        allocator = wlr_allocator_autocreate(backend, renderer);
        if (!allocator) { log("no allocator"); return false; }
        wlr_compositor_create(display, 6, renderer);
        wlr_subcompositor_create(display);
        wlr_data_device_manager_create(display);
        wlr_primary_selection_v1_device_manager_create(display);
        wlr_viewporter_create(display);
        wlr_single_pixel_buffer_manager_v1_create(display);
        wlr_fractional_scale_manager_v1_create(display, 1);
        wlr_presentation_create(display, backend, 2);

        outputs = wlr_output_layout_create(display);
        scene = wlr_scene_create();
        scene_layout = wlr_scene_attach_output_layout(scene, outputs);
        background_layer = wlr_scene_tree_create(&scene->tree);
        window_layer = wlr_scene_tree_create(&scene->tree);
        background = wlr_scene_rect_create(background_layer, static_cast<int>(size.width), static_cast<int>(size.height), kBackground);

        xdg_shell = wlr_xdg_shell_create(display, 6);
        new_toplevel.connect(&xdg_shell->events.new_toplevel, [this](void* d) { add_view(static_cast<wlr_xdg_toplevel*>(d)); });
        new_popup.connect(&xdg_shell->events.new_popup, [this](void* d) { add_popup(static_cast<wlr_xdg_popup*>(d)); });
        // Windows are decorated here: one look, whatever the toolkit.
        auto* decorations = wlr_xdg_decoration_manager_v1_create(display);
        new_decoration.connect(&decorations->events.new_toplevel_decoration, [](void* d) {
            auto* deco = static_cast<wlr_xdg_toplevel_decoration_v1*>(d);
            if (deco->toplevel->base->initialized)
                wlr_xdg_toplevel_decoration_v1_set_mode(deco, wlr_xdg_toplevel_decoration_v1_mode::WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        });

        cursor = wlr_cursor_create();
        wlr_cursor_attach_output_layout(cursor, outputs);
        cursors = wlr_xcursor_manager_create(nullptr, 24);
        wlr_xcursor_manager_load(cursors, 1);
        seat = wlr_seat_create(display, "seat0");
        wlr_seat_set_capabilities(seat, static_cast<std::uint32_t>(wl_seat_capability::WL_SEAT_CAPABILITY_POINTER)
                                        | static_cast<std::uint32_t>(wl_seat_capability::WL_SEAT_CAPABILITY_KEYBOARD));
        request_cursor.connect(&seat->events.request_set_cursor, [this](void* d) {
            auto* ev = static_cast<wlr_seat_pointer_request_set_cursor_event*>(d);
            if (seat->pointer_state.focused_client == ev->seat_client)
                wlr_cursor_set_surface(cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
        });
        request_selection.connect(&seat->events.request_set_selection, [this](void* d) {
            auto* ev = static_cast<wlr_seat_request_set_selection_event*>(d);
            wlr_seat_set_selection(seat, ev->source, ev->serial);
        });

        // The keyboard: opensal's keys, on the keymap the program carries.
        static const wlr_keyboard_impl impl{.name = "opensal-keyboard", .led_update = nullptr};
        wlr_keyboard_init(&keyboard, &impl, "opensal-keyboard");
        xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        const std::string text(builtin_keymap());
        xkb_keymap* keymap = xkb_keymap_new_from_string(xkb, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap) { log("the keymap does not compile"); return false; }
        wlr_keyboard_set_keymap(&keyboard, keymap);
        xkb_keymap_unref(keymap);
        wlr_keyboard_set_repeat_info(&keyboard, 30, 500);
        wlr_seat_set_keyboard(seat, &keyboard);

        if (!wlr_backend_start(backend)) { log("the backend did not start"); return false; }
        output = wlr_headless_add_output(backend, size.width, size.height);
        wlr_output_init_render(output, allocator, renderer);
        wlr_output_state on;
        wlr_output_state_init(&on);
        wlr_output_state_set_enabled(&on, true);
        wlr_output_commit_state(output, &on);
        wlr_output_state_finish(&on);
        wlr_output_layout_output* lo = wlr_output_layout_add_auto(outputs, output);
        scene_output = wlr_scene_output_create(scene, output);
        wlr_scene_output_layout_add_output(scene_layout, lo, scene_output);
        frame.connect(&output->events.frame, [this](void*) { render(); });

        const char* name = wl_display_add_socket_auto(display);
        if (!name) { log("no Wayland socket (XDG_RUNTIME_DIR?)"); return false; }
        socket = name;
        log("listening on " + socket);

        input_source = wl_event_loop_add_fd(loop, o.in, static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                            [](int, std::uint32_t mask, void* data) -> int {
                                                auto* self = static_cast<Server*>(data);
                                                std::array<char, 65536> buf{};
                                                auto n = os::read_some(self->o.in, buf);
                                                if (!n || *n == 0 || (mask & static_cast<std::uint32_t>(WL_EVENT_HANGUP_))) {
                                                    self->stop = true;
                                                    return 0;
                                                }
                                                self->provider(std::string_view(buf.data(), *n));
                                                return 0;
                                            },
                                            this);
        tick = wl_event_loop_add_timer(loop, [](void* data) -> int { static_cast<Server*>(data)->step(); return 0; }, this);
        using I = opensal::Interface;
        send(opensal::wire::encode('h', std::to_string(I::Surface | I::Mode | I::Keyboard | I::Pointer)));
        for (const auto& a : o.autostart) launch(a);
        return true;
    }
    static constexpr std::uint32_t WL_EVENT_READABLE_ = 0x01, WL_EVENT_HANGUP_ = 0x04;

    int run_() {
        if (!start_()) return 1;
        while (!stop) {
            wl_display_flush_clients(display);
            wl_event_loop_dispatch(loop, 100);
            std::erase_if(children, [](int pid) { return os::exited(pid).has_value(); });
        }
        return 0;
    }

    ~Server() {
        if (input_source) wl_event_source_remove(input_source);
        if (tick) wl_event_source_remove(tick);
        for (int pid : children) os::terminate(pid);
        new_toplevel.disconnect(); new_popup.disconnect(); new_decoration.disconnect(); frame.disconnect();
        request_cursor.disconnect(); request_selection.disconnect();
        views.clear();
        popups.clear();
        if (display) wl_display_destroy_clients(display);
        if (scene) wlr_scene_node_destroy(&scene->tree.node);
        if (cursors) wlr_xcursor_manager_destroy(cursors);
        if (cursor) wlr_cursor_destroy(cursor);
        if (keyboard.impl) wlr_keyboard_finish(&keyboard);
        if (xkb) xkb_context_unref(xkb);
        if (allocator) wlr_allocator_destroy(allocator);
        if (renderer) wlr_renderer_destroy(renderer);
        if (backend) wlr_backend_destroy(backend);
        if (display) wl_display_destroy(display);
    }
};

}  // namespace

int run(const Options& o) {
    os::ignore_broken_pipes();
    Server s(o);
    return s.run_();
}

}  // namespace xdesk::compositor
