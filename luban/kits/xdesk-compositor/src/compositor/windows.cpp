// The windows: views and popups, focus, and where the layout puts them (server.cppm).
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

View* Server::view_of(layout::Id id) {
    for (auto& v : views) if (v.id == id) return &v;
    return nullptr;
}

void Server::focus(View* v) {
    if (!v || !v->mapped) return;
    if (View* was = focused(); was && was != v) wlr_xdg_toplevel_set_activated(was->toplevel, false);
    // To the front of the list and of the scene.
    for (auto it = views.begin(); it != views.end(); ++it)
        if (&*it == v) { views.splice(views.begin(), views, it); break; }
    wlr_scene_node_raise_to_top(&v->frame->node);
    wlr_xdg_toplevel_set_activated(v->toplevel, true);
    wlr_seat_keyboard_notify_enter(seat, v->toplevel->base->surface, keyboard.keycodes, keyboard.num_keycodes,
                                   &keyboard.modifiers);
    if (v->laid && lay.focused() != v->id) lay.focus(v->id);
    text_focus(v->toplevel->base->surface);
}

// The layout's focus, given to its window.
void Server::follow_focus() {
    if (const auto id = lay.focused())
        if (View* v = view_of(*id); v && v != focused()) focus(v);
}

// Where the layout puts everything: sizes now, positions by springs
// (at once when `instant`: a window dragged follows the pointer).
void Server::arrange(bool instant) {
    placements = lay.arrange(output_size(), reserved());
    const double zoom = lay.zoom();
    for (auto& v : views) {
        const auto p = std::ranges::find(placements, v.id, &layout::Placement::id);
        wlr_scene_node_set_enabled(&v.frame->node, v.mapped && p != placements.end());
        if (p == placements.end()) continue;
        const int title = static_cast<int>(std::lround(kTitle * zoom));
        draw_title(v, static_cast<int>(std::lround(p->rect.width / zoom)), p->focused);
        wlr_scene_buffer_set_dest_size(v.title, std::max(1, static_cast<int>(std::lround(p->rect.width))), title);
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
    zoom_surfaces(zoom);
    animate();
    draw_shell();
}

// The canvas's zoom on each window's surfaces (a surface's own commit
// sets its size back, so this runs before each frame while zoomed).
void Server::zoom_surfaces(double zoom) {
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
void Server::animate() {
    bool any = false;
    for (const auto& v : views) any = any || v.moving;
    if (any && tick) wl_event_source_timer_update(tick, 16);
}

void Server::step() {
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

void Server::lay_out(View& v) {
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

void Server::add_view(wlr_xdg_toplevel* toplevel) {
    auto& v = views.emplace_back();
    v.server = this;
    v.id = next_id++;
    v.toplevel = toplevel;
    v.frame = wlr_scene_tree_create(window_layer);
    wlr_scene_node_set_enabled(&v.frame->node, false);
    v.title = wlr_scene_buffer_create(v.frame, nullptr);
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
    v.set_title.connect(&toplevel->events.set_title, [this, self](void*) {
        if (self->mapped) arrange();
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
                it->request_maximize.disconnect(); it->request_fullscreen.disconnect(); it->set_title.disconnect();
                wlr_scene_node_destroy(&it->frame->node);
                views.erase(it);
                break;
            }
        arrange();
    });
}

void Server::add_popup(wlr_xdg_popup* popup) {
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

}  // namespace xdesk::compositor
