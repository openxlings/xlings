// A window dragged, a gap between columns resized, the maximize button's
// layouts (server.cppm; design 2026-10-19 §17.9, §17.10). The window follows
// the pointer, see-through; where it would land shows under it -- the drop
// made on a copy of the layout, so the preview is the result; resting on a
// new column offers its width. Let go, it glides there.
module xdesk.compositor;

import std;
import opensal;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

namespace layout = xdesk::layout;
namespace shell = xdesk::shell;

namespace {

// The widths a new column is offered, and what they are called.
constexpr std::array<double, 4> kWidths{1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0};
const std::vector<std::string> kWidthLabels{"1/3", "1/2", "2/3", "1"};

}  // namespace

void Server::set_opacity(View& v, float opacity) {
    wlr_scene_node_for_each_buffer(&v.frame->node, [](wlr_scene_buffer* b, int, int, void* data) {
        wlr_scene_buffer_set_opacity(b, *static_cast<float*>(data));
    }, &opacity);
}

void Server::drag_start(View* v, bool live) {
    const auto r = placement_of(v->id);
    if (!r) return;
    focus(v);
    drag = Drag::move;
    dragged = v->id;
    drag_live = live;
    drag_x = cursor->x;
    drag_y = cursor->y;
    drag_dx = cursor->x - r->x;
    drag_dy = cursor->y - r->y;
    drop = {};
    drop_width = 0;
    sizes_shown = false;
    if (!live) set_opacity(*v, 0.75f);
    wlr_scene_node_raise_to_top(&v->frame->node);
}

void Server::drag_motion() {
    View* v = view_of(dragged);
    if (!v) return;
    const double dx = cursor->x - drag_x, dy = cursor->y - drag_y;
    drag_x = cursor->x;
    drag_y = cursor->y;
    pointer_shape("grabbing");
    if (drag_live) {
        if (const auto p = placement_of(dragged)) lay.place(dragged, {p->x + dx, p->y + dy, p->width, p->height});
        arrange(true);
    } else {
        // The frame follows the pointer; the rest stay where they are until it is let go.
        v->x.value = v->x.target = cursor->x - drag_dx;
        v->y.value = v->y.target = cursor->y - drag_dy;
        v->moving = false;
        wlr_scene_node_set_position(&v->frame->node, static_cast<int>(std::lround(v->x.value)), static_cast<int>(std::lround(v->y.value)));
    }
    // A floating window, a group moved whole: they land where they are.
    if (lay.floating(dragged) || (canvas() && drag_live)) { hide_drop(); return; }
    const auto d = lay.drop_at(dragged, cursor->x, cursor->y, held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT));
    if (!(d == drop)) {
        drop = d;
        drop_width = 0;
        sizes_shown = false;
        if (drop_timer) wl_event_source_timer_update(drop_timer, 300);
    }
    // Resting on a new column: its widths, the one under the pointer taken.
    if (sizes_shown)
        for (const auto& a : sizes_picture.areas)
            if (cursor->x >= a.rect.x && cursor->y >= a.rect.y && cursor->x < a.rect.x + a.rect.width && cursor->y < a.rect.y + a.rect.height)
                drop_width = kWidths[static_cast<std::size_t>(a.index)];
    show_drop();
}

void Server::show_drop() {
    View* v = view_of(dragged);
    const auto r = v ? lay.preview(dragged, drop, drop_width) : std::nullopt;
    if (!r) { hide_drop(); return; }
    wlr_scene_rect_set_size(drop_box, std::max(1, static_cast<int>(std::lround(r->width))), std::max(1, static_cast<int>(std::lround(r->height))));
    wlr_scene_node_set_position(&drop_box->node, static_cast<int>(std::lround(r->x)), static_cast<int>(std::lround(r->y)));
    wlr_scene_node_set_enabled(&drop_box->node, true);
    wlr_scene_node_place_below(&drop_box->node, &v->frame->node);
    if (sizes_shown && drop.kind == layout::Drop::Kind::column) {
        std::size_t selected = std::string::npos;
        for (std::size_t k = 0; k < kWidths.size(); ++k)
            if (std::abs(kWidths[k] - drop_width) < 1e-6) selected = k;
        sizes_picture = shell::sizes(ui(), theme, kWidthLabels, selected, r->x + r->width / 2, r->y + 16, logical_w(), logical_h(), scale);
        wlr_scene_node_set_position(&sizes_node->node, static_cast<int>(sizes_picture.x), static_cast<int>(sizes_picture.y));
        show(sizes_node, std::move(sizes_picture.image), scale);
        sizes_picture.image = {};
        wlr_scene_node_set_enabled(&sizes_node->node, true);
    } else {
        wlr_scene_node_set_enabled(&sizes_node->node, false);
    }
}

void Server::hide_drop() {
    wlr_scene_node_set_enabled(&drop_box->node, false);
    wlr_scene_node_set_enabled(&sizes_node->node, false);
}

void Server::drag_end() {
    View* v = view_of(dragged);
    hide_drop();
    if (v) {
        set_opacity(*v, 1.0f);
        if (!lay.floating(dragged) && !(canvas() && drag_live) && drop.kind != layout::Drop::Kind::none) {
            const auto p = placement_of(dragged);
            const layout::Rect here { v->x.value, v->y.value, p ? p->width : 0, p ? p->height : 0 };
            lay.drop(dragged, drop, drop_width);
            // Let go in the open: where it is (a canvas group's window out of it first).
            if ((drop.kind == layout::Drop::Kind::free || drop.kind == layout::Drop::Kind::floating) && here.width > 0) lay.place(dragged, here);
        }
    }
    drag = Drag::none;
    drop = {};
    sizes_shown = false;
    arrange();
}

layout::Id Server::maximize_under_pointer() const {
    if (!live()) return 0;
    const auto id = lay.at(cursor->x, cursor->y);
    if (!id) return 0;
    const auto r = placement_of(*id);
    if (!r) return 0;
    const auto b = shell::maximize_button(static_cast<int>(r->width));
    const double x = cursor->x - r->x, y = cursor->y - r->y;
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height ? *id : 0;
}

void Server::open_layouts(layout::Id id) {
    const auto r = placement_of(id);
    if (!r) return;
    close_overlay(false);
    layouts_for = id;
    overlay = Overlay::layouts;
    const auto b = shell::maximize_button(static_cast<int>(r->width));
    overlay_picture.x = r->x + b.x + b.width;
    overlay_picture.y = r->y + kTitle;
    draw_overlay();
}

}  // namespace xdesk::compositor
