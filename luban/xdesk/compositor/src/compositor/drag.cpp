// A window dragged, a gap between columns resized, the maximize button's
// layouts (server.cppm; design 2026-10-19 §17.9, §17.10; 2026-10-20 §7.5,
// §7.6). The window follows the pointer, see-through; where it would land
// shows under it and the others move to where the drop leaves them -- the
// drop made on a copy of the layout, so the preview is the result; a new
// column's width is the wheel's or a digit's. Let go, it glides there.
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
    if (!live) set_opacity(*v, 0.75f);
    wlr_scene_node_raise_to_top(&v->frame->node);
}

// The drop at the pointer, held while the pointer is within 12 pixels of
// where it still gives the drop shown (design 2026-10-20 §7.5): an edge
// between two zones does not flicker.
layout::Drop Server::drop_with_hysteresis(bool shift) const {
    const auto here = lay.drop_at(dragged, cursor->x, cursor->y, shift);
    if (here == drop || drop.kind == layout::Drop::Kind::none) return here;
    constexpr double kHold = 12;
    for (const auto [dx, dy] : {std::pair{-kHold, 0.0}, {kHold, 0.0}, {0.0, -kHold}, {0.0, kHold}})
        if (lay.drop_at(dragged, cursor->x + dx, cursor->y + dy, shift) == drop) return drop;
    return here;
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
        // The frame follows the pointer.
        v->x.value = v->x.target = cursor->x - drag_dx;
        v->y.value = v->y.target = cursor->y - drag_dy;
        v->moving = false;
        wlr_scene_node_set_position(&v->frame->node, static_cast<int>(std::lround(v->x.value)), static_cast<int>(std::lround(v->y.value)));
    }
    // A floating window, a group moved whole: they land where they are.
    if (lay.floating(dragged) || (canvas() && drag_live)) { hide_drop(); return; }
    const auto d = drop_with_hysteresis(held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT));
    if (!(d == drop)) {
        drop = d;
        show_drop();
    }
}

// Where it lands, and every other window where the drop leaves it -- moving
// there now, so what is seen is the result (design 2026-10-20 §7.5, G6).
void Server::show_drop() {
    View* v = view_of(dragged);
    if (!v) { hide_drop(); return; }
    const auto all = drop.kind == layout::Drop::Kind::none || drop.kind == layout::Drop::Kind::free || drop.kind == layout::Drop::Kind::floating
                         ? lay.arrange(output_size(), reserved())
                         : lay.preview_all(dragged, drop, drop_width);
    apply(all, false, dragged);
    std::optional<layout::Rect> r;
    for (const auto& p : all)
        if (p.id == dragged && drop.kind != layout::Drop::Kind::none && drop.kind != layout::Drop::Kind::free
            && drop.kind != layout::Drop::Kind::floating)
            r = p.rect;
    if (!r) { hide_drop(); return; }
    wlr_scene_rect_set_size(drop_box, std::max(1, static_cast<int>(std::lround(r->width))), std::max(1, static_cast<int>(std::lround(r->height))));
    wlr_scene_node_set_position(&drop_box->node, static_cast<int>(std::lround(r->x)), static_cast<int>(std::lround(r->y)));
    wlr_scene_node_set_enabled(&drop_box->node, true);
    wlr_scene_node_place_below(&drop_box->node, &v->frame->node);
    // A new column: its width, said in the box, chosen with the wheel or 1-4 (§7.6).
    if (drop.kind == layout::Drop::Kind::column) {
        const auto it = std::ranges::find_if(kWidths, [&](double w) { return std::abs(w - drop_width) < 1e-6; });
        const std::string label = std::format("{}{}", it == kWidths.end() ? (zh ? "自动" : "auto") : kWidthLabels[static_cast<std::size_t>(it - kWidths.begin())],
                                              zh ? " · 滚轮或 1–4 切换宽度" : " · wheel or 1-4: its width");
        sizes_picture = shell::sizes(ui(), theme, {label}, 0, r->x + r->width / 2, r->y + 16, logical_w(), logical_h(), scale);
        wlr_scene_node_set_position(&sizes_node->node, static_cast<int>(sizes_picture.x), static_cast<int>(sizes_picture.y));
        show(sizes_node, std::move(sizes_picture.image), scale);
        sizes_picture.image = {};
        wlr_scene_node_set_enabled(&sizes_node->node, true);
    } else {
        wlr_scene_node_set_enabled(&sizes_node->node, false);
    }
}

// Dragging onto a new column: the wheel or a digit 1-4 picks its width.
bool Server::drag_width(int step, int digit) {
    if (drag != Drag::move || drop.kind != layout::Drop::Kind::column) return false;
    if (digit >= 1 && digit <= 4) drop_width = kWidths[static_cast<std::size_t>(digit - 1)];
    else {
        auto it = std::ranges::find_if(kWidths, [&](double w) { return std::abs(w - drop_width) < 1e-6; });
        std::ptrdiff_t i = it == kWidths.end() ? 1 : it - kWidths.begin();
        i = std::clamp<std::ptrdiff_t>(i + step, 0, static_cast<std::ptrdiff_t>(kWidths.size()) - 1);
        drop_width = kWidths[static_cast<std::size_t>(i)];
    }
    show_drop();
    return true;
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
    drop_width = 0;
    arrange();
}

// ── resizing (design 2026-10-20 §8, G10) ─────────────────────────

namespace {
constexpr std::uint32_t kTop = 1, kBottom = 2, kLeft = 4, kRight = 8;   // wlr_edges
constexpr double kBorder = 6;
}  // namespace

// The edges of `v` within the border's reach of the pointer (stacked,
// floating and canvas windows: tiled ones are sized by their gaps).
std::uint32_t Server::edges_at(const View& v) const {
    if (!live() || (lay.mode() == layout::Mode::tile && !lay.floating(v.id))) return 0;
    const auto r = placement_of(v.id);
    if (!r) return 0;
    const double x = cursor->x - v.frame->node.x, y = cursor->y - v.frame->node.y;
    std::uint32_t e = 0;
    if (x < kBorder) e |= kLeft;
    else if (x > r->width - kBorder) e |= kRight;
    if (y > r->height - kBorder) e |= kBottom;
    else if (y < kBorder / 2) e |= kTop;   // the title bar is for moving: only its very edge resizes
    return e;
}

std::string_view Server::resize_shape(std::uint32_t e) {
    if (e == (kTop | kLeft)) return "nw-resize";
    if (e == (kTop | kRight)) return "ne-resize";
    if (e == (kBottom | kLeft)) return "sw-resize";
    if (e == (kBottom | kRight)) return "se-resize";
    if (e & (kLeft | kRight)) return "ew-resize";
    return "ns-resize";
}

bool Server::resize_start(View* v, std::uint32_t edges) {
    const auto r = placement_of(v->id);
    if (!r) return false;
    focus(v);
    const double x = cursor->x - v->frame->node.x, y = cursor->y - v->frame->node.y;
    if (lay.mode() == layout::Mode::tile && !lay.floating(v->id)) {
        // Tiled: the gap on the side nearest the pointer, between columns or
        // between the windows of its column.
        const double g = lay.options().inner_gap / 2 + 1;
        const double to_left = x, to_right = r->width - x, to_top = y, to_bottom = r->height - y;
        const double nearest = std::min({to_left, to_right, to_top, to_bottom});
        drag_x = cursor->x;
        drag_y = cursor->y;
        if (nearest == to_left || nearest == to_right) {
            const double gx = nearest == to_left ? r->x - g : r->x + r->width + g;
            if (const auto gap = lay.gap_at(gx, r->y + r->height / 2)) {
                drag = Drag::gap;
                gap_column = *gap;
                return true;
            }
        }
        const double gy = nearest == to_top ? r->y - g : r->y + r->height + g;
        if (const auto row = lay.row_gap_at(r->x + r->width / 2, gy)) {
            drag = Drag::row_gap;
            row_gap = *row;
            return true;
        }
        return false;
    }
    if (edges == 0)
        edges = (x < r->width / 2 ? kLeft : kRight) | (y < r->height / 2 ? kTop : kBottom);
    drag = Drag::resize;
    dragged = v->id;
    resize_edges = edges;
    resize_from = {v->x.value, v->y.value, r->width, r->height};
    drag_x = cursor->x;
    drag_y = cursor->y;
    pointer_shape(resize_shape(edges));
    return true;
}

void Server::resize_motion() {
    View* v = view_of(dragged);
    if (!v) return;
    const double dx = cursor->x - drag_x, dy = cursor->y - drag_y;
    // What the client allows (its frame: the title bar added), else a floor.
    const auto& st = v->toplevel->current;
    const double min_w = std::max(120.0, static_cast<double>(st.min_width));
    const double min_h = std::max(80.0, static_cast<double>(st.min_height + kTitle));
    const double max_w = st.max_width > 0 ? st.max_width : 1e9, max_h = st.max_height > 0 ? st.max_height + kTitle : 1e9;
    layout::Rect r = resize_from;
    if (resize_edges & kRight) r.width = std::clamp(resize_from.width + dx, min_w, max_w);
    if (resize_edges & kBottom) r.height = std::clamp(resize_from.height + dy, min_h, max_h);
    if (resize_edges & kLeft) {
        r.width = std::clamp(resize_from.width - dx, min_w, max_w);
        r.x = resize_from.x + resize_from.width - r.width;
    }
    if (resize_edges & kTop) {
        r.height = std::clamp(resize_from.height - dy, min_h, max_h);
        r.y = resize_from.y + resize_from.height - r.height;
    }
    pointer_shape(resize_shape(resize_edges));
    lay.place(dragged, r);
    arrange(true);
}

layout::Id Server::maximize_under_pointer() {
    if (!live()) return 0;
    View* v = view_at(cursor->x, cursor->y);
    const auto r = v ? placement_of(v->id) : std::nullopt;
    if (!r) return 0;
    const auto b = shell::maximize_button(static_cast<int>(r->width));
    const double x = cursor->x - v->frame->node.x, y = cursor->y - v->frame->node.y;
    return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height ? v->id : 0;
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
