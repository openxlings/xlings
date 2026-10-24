// The windows: views and popups, focus, and where the layout puts them (server.cppm).
module xdesk.compositor;

import std;
import xdesk.words;
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

namespace {
// A client's credentials' types, as libwayland names them (no system header here).
template <class> struct CredentialTypes;
template <class C, class P, class U, class G> struct CredentialTypes<void (*)(C, P*, U*, G*)> { using pid = P; using uid = U; using gid = G; };
using Credentials = CredentialTypes<decltype(&wl_client_get_credentials)>;

// The backdrop's colour: an idle title's, so a short client reads as its frame.
std::array<float, 4> backdrop_colour_(const shell::Theme& t) {
    return {t.title_idle.r / 255.0f, t.title_idle.g / 255.0f, t.title_idle.b / 255.0f, 1.0f};
}
}  // namespace

View* Server::view_of(layout::Id id) {
    for (auto& v : views) if (v.id == id) return &v;
    return nullptr;
}

void Server::focus(View* v) {
    if (!v || !v->mapped) return;
    // Collapsed: no input -- the focus passes it by (design 2026-10-21 §8.5).
    if (lay.collapsed(v->id)) return;
    plane_focus = false;
    if (View* was = focused(); was && was != v) wlr_xdg_toplevel_set_activated(was->toplevel, false);
    // To the front of the list and of the scene.
    for (auto it = views.begin(); it != views.end(); ++it)
        if (&*it == v) { views.splice(views.begin(), views, it); break; }
    wlr_scene_node_raise_to_top(&v->frame->node);
    wlr_xdg_toplevel_set_activated(v->toplevel, true);
    publish("focus", {{"id", std::to_string(v->id)}, {"app_id", v->toplevel->app_id ? v->toplevel->app_id : ""}});
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
    apply(placements, instant);
    // Kept a moment later (what it says compared before it is written).
    if (wuxian_timer) wl_event_source_timer_update(wuxian_timer, 1000);
}

void Server::apply(const std::vector<layout::Placement>& ps, bool instant, layout::Id skip) {
    for (auto& v : views) {
        if (v.id == skip) continue;
        const auto p = std::ranges::find(ps, v.id, &layout::Placement::id);
        wlr_scene_node_set_enabled(&v.frame->node, v.mapped && p != ps.end());
        if (p == ps.end()) continue;
        // Its scale in the WuXian view, and the overview's of it (switchers.cpp).
        const auto spread = spread_zoom.find(v.id);
        const double zoom = p->zoom * (spread != spread_zoom.end() ? spread->second : 1);
        v.zoom = zoom;
        // Collapsed: its title bar alone, the client kept at its size, unseen.
        // A window of the desktop's own shows what the desktop draws instead.
        wlr_scene_node_set_enabled(&v.content->node, !p->collapsed && !v.own);
        if (v.own) wlr_scene_node_set_enabled(&v.own->node, !p->collapsed);
        if (v.backdrop) wlr_scene_node_set_enabled(&v.backdrop->node, !p->collapsed);
        // Full screen: no title bar, all of the output the client's.
        const int bar = p->fullscreen ? 0 : kTitle;
        const int title = static_cast<int>(std::lround(bar * zoom));
        wlr_scene_node_set_enabled(&v.title->node, bar > 0);
        if (bar > 0) {
            // Square where it fills the screen: maximized, or a tiled one as wide as it.
            const bool fills = p->maximized || (p->tiled && lay.options().outer_gap == 0 && p->rect.width >= logical_w() - 1);
            draw_title(v, static_cast<int>(std::lround(p->rect.width / zoom)), p->focused, !fills);
            wlr_scene_buffer_set_dest_size(v.title, std::max(1, static_cast<int>(std::lround(p->rect.width))), title);
        }
        wlr_scene_node_set_position(&v.content->node, 0, title);
        // The client is asked for its size at zoom 1 (the WuXian view scales it).
        const int cw = std::max(1, static_cast<int>(std::lround(p->rect.width / zoom)));
        const int ch = std::max(1, static_cast<int>(std::lround(p->rect.height / zoom)) - bar);
        if (v.toplevel->base->initialized && !p->collapsed) {
            // Only what changed: each of these sends a configure, and a
            // client draws again for one -- every frame of a glide or of the
            // view's change otherwise.
            const auto& was = v.toplevel->scheduled;
            if (was.width != cw || was.height != ch) wlr_xdg_toplevel_set_size(v.toplevel, cw, ch);
            // Tiled (design 2026-10-20 §7.1): the client takes the size as
            // given -- foot, told nothing, rounds down to its cells.
            const std::uint32_t tiled = p->tiled || p->maximized ? kAllEdges : 0;
            if (was.tiled != tiled) wlr_xdg_toplevel_set_tiled(v.toplevel, tiled);
            if (was.maximized != p->maximized) wlr_xdg_toplevel_set_maximized(v.toplevel, p->maximized);
            if (was.fullscreen != p->fullscreen) wlr_xdg_toplevel_set_fullscreen(v.toplevel, p->fullscreen);
        }
        if (v.backdrop) {
            wlr_scene_rect_set_size(v.backdrop, std::max(1, static_cast<int>(std::lround(p->rect.width))),
                                    std::max(1, static_cast<int>(std::lround(p->rect.height)) - title));
            wlr_scene_node_set_position(&v.backdrop->node, 0, title);
        }
        lay_outline(v, p->rect.width, p->rect.height);
        // The first place is taken at once; after that the frame glides.
        v.x.target = p->rect.x;
        v.y.target = p->rect.y;
        if (!v.placed || instant || reduce_motion) { v.x.snap(); v.y.snap(); }
        v.placed = v.placed || v.mapped;
        v.moving = v.x.value != v.x.target || v.y.value != v.y.target;
        wlr_scene_node_set_position(&v.frame->node, static_cast<int>(std::lround(v.x.value)), static_cast<int>(std::lround(v.y.value)));
    }
    // Stacking as the layout orders them: stacked ones back to front, then
    // floating ones, then those always on top (one carried by the pointer
    // above all). In the WuXian view each workspace's plate goes just under
    // its windows (an empty one's under everything).
    update_frames(ps);
    auto raise_plate = [&](int id) {
        if (const auto n = std::ranges::find(ws_nodes, id, &WorkspaceNodes::id); n != ws_nodes.end()) {
            wlr_scene_node_raise_to_top(&n->plate->node);
            for (auto* b : n->border) wlr_scene_node_raise_to_top(&b->node);
        }
    };
    for (const auto& n : ws_nodes)
        if (std::ranges::none_of(ps, [&](const layout::Placement& p) { return p.workspace == n.id; })) raise_plate(n.id);
    int plate_for = -1;
    for (const auto& p : ps) {
        if (!ws_nodes.empty() && p.workspace != 0 && p.workspace != plate_for && !p.pinned) {
            plate_for = p.workspace;
            raise_plate(p.workspace);
        }
        if (View* v = view_of(p.id); v && v->id != skip) wlr_scene_node_raise_to_top(&v->frame->node);
    }
    for (auto* r : {drop_place, drop_half, drop_box}) wlr_scene_node_raise_to_top(&r->node);
    if (View* carried = skip ? view_of(skip) : nullptr) wlr_scene_node_raise_to_top(&carried->frame->node);
    zoom_surfaces();
    animate();
    draw_shell();
    update_bookmarks();
    wallpaper_update();
    // The settings' window drawn again at a new size or scale.
    if (View* sw = settings_window())
        if (const auto r = placement_of(sw->id)) {
            auto key = std::format("{}x{}@{}*{}", std::lround(r->width), std::lround(r->height), sw->zoom, scale);
            if (key != settings_window_drawn) {
                settings_window_drawn = std::move(key);
                draw_settings_window();
            }
        }
}

View* Server::view_at(double x, double y) {
    double sx = 0, sy = 0;
    wlr_scene_node* node = wlr_scene_node_at(&window_layer->node, x, y, &sx, &sy);
    // Up to the window's frame: a child of the window layer.
    while (node && node->parent && node->parent != window_layer) node = &node->parent->node;
    if (!node) return nullptr;
    for (auto& v : views)
        if (v.mapped && &v.frame->node == node) return &v;
    return nullptr;
}

bool Server::on_title(const View& v, double y) const {
    // Its title bar is drawn at its zoom; however small, at least 24 screen
    // pixels of it are taken (design 2026-10-21 §8.7).
    constexpr double kTitleReach = 24;
    return y - v.frame->node.y < std::max(kTitle * v.zoom, kTitleReach);
}

void Server::collapse_window(layout::Id id) {
    if (!lay.is_free(id)) return;
    lay.toggle_collapsed(id);
    const bool now = lay.collapsed(id);
    publish("collapse", {{"id", std::to_string(id)}, {"collapsed", now ? "true" : "false"}});
    // Collapsed, it lets the focus go; expanded, it takes it.
    if (now && lay.focused() == id) {
        if (View* v = view_of(id)) wlr_xdg_toplevel_set_activated(v->toplevel, false);
        wlr_seat_keyboard_notify_clear_focus(seat);
    }
    remember_arrangement();
    arrange();
    if (!now)
        if (View* v = view_of(id)) focus(v);
}

void Server::collapse_workspace(int id) {
    if (!lay.workspace_info(id)) return;
    lay.toggle_workspace_collapsed(id);
    publish("collapse", {{"workspace", std::to_string(id)}, {"collapsed", lay.workspace_collapsed(id) ? "true" : "false"}});
    remember_arrangement();
    arrange();
}

void Server::collapse_focused() {
    if (!wuxian()) {
        show_hint(i18n::tr("Collapsing is the WuXian view's (Super+O)"), 2500);
        return;
    }
    if (const auto f = lay.focused(); f && lay.is_free(*f)) return collapse_window(*f);
    collapse_workspace(ws_selected ? ws_selected : lay.focused_workspace());
}

// Each window's zoom on its surfaces (a surface's own commit sets its size
// back, so this runs before each frame while one is zoomed).
void Server::zoom_surfaces() {
    // While the view changes, scaled by the nearest pixel (a frame shown for
    // 16 ms; in software, filtering is most of its cost -- design 2026-10-20
    // §13.2); once still, filtered.
    struct Zoom {
        double z;
        bool moving;
    };
    for (auto& v : views) {
        if (v.zoom == 1 && v.zoom_shown == 1) continue;
        Zoom zoom{v.zoom, view_move.has_value()};
        wlr_scene_buffer_set_filter_mode(v.title, zoom.moving && zoom.z != 1 ? wlr_scale_filter_mode::WLR_SCALE_FILTER_NEAREST
                                                                             : wlr_scale_filter_mode::WLR_SCALE_FILTER_BILINEAR);
        wlr_scene_node_for_each_buffer(&v.content->node, [](wlr_scene_buffer* b, int, int, void* data) {
            const auto& [z, moving] = *static_cast<Zoom*>(data);
            auto* s = wlr_scene_surface_try_from_buffer(b);
            if (!s) return;
            wlr_scene_buffer_set_filter_mode(b, moving && z != 1 ? wlr_scale_filter_mode::WLR_SCALE_FILTER_NEAREST
                                                                 : wlr_scale_filter_mode::WLR_SCALE_FILTER_BILINEAR);
            if (z == 1) wlr_scene_buffer_set_dest_size(b, s->surface->current.width, s->surface->current.height);
            else wlr_scene_buffer_set_dest_size(b, std::max(1, static_cast<int>(s->surface->current.width * z)),
                                                std::max(1, static_cast<int>(s->surface->current.height * z)));
        }, &zoom);
        v.zoom_shown = zoom.z;
    }
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
    lay.add(v.id, preferred, floats, {t->app_id ? t->app_id : "", t->title ? t->title : ""});
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
    {
        const auto c = backdrop_colour_(theme);
        v.backdrop = wlr_scene_rect_create(v.frame, 1, 1, c.data());
    }
    v.content = wlr_scene_xdg_surface_create(v.frame, toplevel->base);
    wlr_scene_node_set_position(&v.content->node, 0, kTitle);
    v.content->node.data = &v;
    for (auto& r : v.outline) {
        const float none[4] = {0, 0, 0, 0};
        r = wlr_scene_rect_create(v.frame, 1, 1, none);
        wlr_scene_node_set_enabled(&r->node, false);
    }
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
        // The settings' window, held by the process this desktop started for
        // it: its content is the desktop's to draw (design 2026-10-24 §9.6).
        if (settings_pid > 0 && !settings_view) {
            Credentials::pid pid = 0;
            Credentials::uid uid = 0;
            Credentials::gid gid = 0;
            wl_client_get_credentials(wl_resource_get_client(self->toplevel->base->resource), &pid, &uid, &gid);
            if (static_cast<int>(pid) == settings_pid) {
                settings_view = self->id;
                self->own = wlr_scene_buffer_create(self->frame, nullptr);
            }
        }
        lay_out(*self);
        // A floating window keeps the size it drew, centred where the layout put it.
        if (lay.floating(self->id)) {
            const wlr_box g = self->toplevel->base->geometry;
            const double w = std::max(g.width, 1), h = std::max(g.height, 1) + kTitle;
            lay.place(self->id, {(size.width - w) / 2, (size.height - h) / 2, w, h});
        }
        // The application's rules (always floating, a workspace, its size and place).
        apply_rules(*self);
        lay.focus(self->id);
        arrange();
        // Focused where it is shown (a rule may have sent it to another workspace).
        if (lay.focused() == self->id) focus(self);
        else follow_focus();
        // A new window among others: its outline lit a moment (design 2026-10-21 §8.2).
        if (lay.focused() == self->id && std::ranges::count_if(views, [](const View& o) { return o.mapped; }) > 1) start_highlight(self->id);
        publish("open", {{"id", std::to_string(self->id)}, {"app_id", self->toplevel->app_id ? self->toplevel->app_id : ""}});
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
        publish("close", {{"id", std::to_string(self->id)}, {"app_id", self->toplevel->app_id ? self->toplevel->app_id : ""}});
    });
    v.set_title.connect(&toplevel->events.set_title, [this, self](void*) {
        if (self->mapped) arrange();
    });
    v.request_maximize.connect(&toplevel->events.request_maximize, [self](void*) {
        if (self->toplevel->base->initialized) wlr_xdg_surface_schedule_configure(self->toplevel->base);
    });
    // A client asking for full screen (a video, a game) gets it, and back.
    v.request_fullscreen.connect(&toplevel->events.request_fullscreen, [this, self](void*) {
        if (self->laid && self->mapped) {
            lay.set_fullscreen(self->id, self->toplevel->requested.fullscreen);
            arrange();
        } else if (self->toplevel->base->initialized) {
            wlr_xdg_surface_schedule_configure(self->toplevel->base);
        }
    });
    // A client that draws its own frame asks to be moved or resized with the
    // button it holds (design 2026-10-20 §8).
    v.request_move.connect(&toplevel->events.request_move, [this, self](void*) {
        if (drag == Drag::none && self->mapped && live())
            drag_start(self, !wuxian() && (lay.floating(self->id) || rule_of(self->id) == layout::Rule::stack));
    });
    v.request_resize.connect(&toplevel->events.request_resize, [this, self](void* d) {
        const auto* ev = static_cast<wlr_xdg_toplevel_resize_event*>(d);
        if (drag == Drag::none && self->mapped && live()) resize_start(self, ev->edges);
    });
    v.destroy.connect(&toplevel->events.destroy, [this, self](void*) {
        if (self->id == settings_view) { settings_view = 0; settings_pid = -1; }
        if (self->laid) lay.remove(self->id);
        for (auto it = views.begin(); it != views.end(); ++it)
            if (&*it == self) {
                it->map.disconnect(); it->unmap.disconnect(); it->commit.disconnect(); it->destroy.disconnect();
                it->request_maximize.disconnect(); it->request_fullscreen.disconnect(); it->set_title.disconnect();
                it->request_move.disconnect(); it->request_resize.disconnect();
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
