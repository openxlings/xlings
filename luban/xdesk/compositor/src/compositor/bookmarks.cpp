// Bookmarks (server.cppm; design 2026-10-21 §8.4, 2026-10-24 §9.4, openwx
// §5.1): an item of which nothing shows -- a workspace or a free window on the
// plane, a window of a stacked workspace -- is said by a bookmark that comes
// out from under the top-most item over its centre, at its right edge, as a
// ribbon pressed under a book's cover: drawn under that item and over the
// covered one, in its own colour with its icon and short title; five at
// most, then "+N", whose list is a menu. Hovering one lights it and outlines
// the covered item, clicking it raises and focuses it. Recomputed after each
// still arrangement; hidden while something is dragged or the view moves.
module xdesk.compositor;

import std;
import xdesk.words;
import openwx;
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
namespace draw = xdesk::draw;

namespace {

constexpr std::size_t kShownBookmarks = 5;
constexpr int kStillMs = 250;   // the view still this long: bookmarks again

struct Item {
    layout::Rect rect;          // its frame, its label or title bar included
    layout::Id window { 0 };
    int workspace { 0 };
};

}  // namespace

void Server::hide_bookmarks() {
    for (auto* n : bookmark_pool) wlr_scene_node_set_enabled(&n->node, false);
    for (auto* r : bookmark_outline)
        if (r) wlr_scene_node_set_enabled(&r->node, false);
    bookmarks.clear();
    bookmark_lit = -1;
}

void Server::update_bookmarks() {
    const auto now = std::chrono::steady_clock::now();
    const bool still = now >= view_still;
    if (!bookmarks_on || drag != Drag::none || view_move || !still || !live_bookmarks()) {
        hide_bookmarks();
        if (!still && bookmark_timer) wl_event_source_timer_update(bookmark_timer, kStillMs);
        return;
    }
    const layout::Rect screen{0, reserved().top, double(logical_w()), logical_h() - reserved().top};
    std::vector<Item> items;
    if (wuxian()) {
        // The plane's order: workspaces and free windows by their layer.
        auto layers = lay.minimap().layers;
        std::ranges::stable_sort(layers, {}, &openwx::Layer::z);
        const auto cam = lay.camera();
        for (const auto& l : layers) {
            auto r = openwx::to_screen(cam, l.world, {screen.x, screen.y});
            if (l.kind == openwx::Layer::Kind::workspace) {
                // Its label above its top-left is part of it.
                r.y -= shell::kLabelHeight + 5;
                r.height += shell::kLabelHeight + 5;
                items.push_back({.rect = r, .workspace = l.workspace});
            } else {
                items.push_back({.rect = r, .window = l.window});
            }
        }
    } else if (lay.rule() == layout::Rule::stack) {
        // A stacked workspace's windows, back to front.
        for (const auto& p : placements)
            if (p.visible && !p.pinned && !p.fullscreen) items.push_back({.rect = p.rect, .window = p.id});
    }
    std::vector<layout::Rect> rects;
    for (const auto& i : items) rects.push_back(i.rect);
    const auto covered = layout::covered(rects, screen);
    // By the item that carries them, in order.
    std::map<std::size_t, std::vector<std::size_t>> by_host;
    for (const auto& c : covered) by_host[c.host].push_back(c.item);
    std::vector<Bookmark> next;
    for (const auto& [host, under] : by_host) {
        const auto& h = items[host].rect;
        const double top = h.y + (items[host].workspace ? shell::kLabelHeight + 13 : 8);
        for (std::size_t k = 0; k < under.size() && k <= kShownBookmarks; ++k) {
            Bookmark b;
            if (k == kShownBookmarks) {
                // The rest, one "+N" whose list is a menu.
                for (std::size_t j = k; j < under.size(); ++j) b.more.push_back({items[under[j]].window, items[under[j]].workspace});
                b.label = std::format("+{}", under.size() - k);
            } else {
                const auto& it = items[under[k]];
                b.window = it.window;
                b.workspace = it.workspace;
                b.item = it.rect;
                if (it.window) {
                    View* v = view_of(it.window);
                    b.label = v ? name_of(*v) : std::string("?");
                } else if (const auto* w = lay.workspace_info(it.workspace)) {
                    b.label = w->name.empty() ? i18n::trf("Workspace {}", w->id) : std::format("{} · {}", w->id, w->name);
                    b.colour = draw::Color::rgb(w->color);
                }
            }
            // Its colour and icon: a window's application's (its icon's colour).
            if (b.window)
                if (View* v = view_of(b.window); v && v->toplevel->app_id) {
                    b.icon = icon_of(v->toplevel->app_id, 16);
                    if (b.icon && !b.colour) b.colour = shell::icon_colour(*b.icon);
                }
            if (!b.colour && b.more.empty()) b.colour = draw::Color::rgb(0x3d4f6a);
            // Under the item over it: a window's frame, a workspace's plate.
            if (items[host].window) {
                if (View* hv = view_of(items[host].window)) b.under = &hv->frame->node;
            } else if (const auto n = std::ranges::find(ws_nodes, items[host].workspace, &WorkspaceNodes::id); n != ws_nodes.end()) {
                b.under = &n->plate->node;
            }
            // Staggered down the edge, its root under the item.
            b.tab = {h.x + h.width - shell::kBookmarkRoot, top + static_cast<double>(k) * (shell::kBookmarkHeight + 4), 0,
                     double(shell::kBookmarkHeight)};
            next.push_back(std::move(b));
        }
    }
    // Drawn: one buffer each, from a pool kept.
    // In the windows' layer: each goes just under the item over it.
    while (bookmark_pool.size() < next.size()) {
        auto* n = wlr_scene_buffer_create(window_layer, nullptr);
        wlr_scene_node_set_enabled(&n->node, false);
        bookmark_pool.push_back(n);
    }
    for (std::size_t k = 0; k < next.size(); ++k) {
        auto& b = next[k];
        auto pic = shell::bookmark(ui(), theme, b.label, b.colour, static_cast<int>(k) == bookmark_lit, scale, b.icon);
        b.tab.width = pic.image.width() / scale;
        // Past the item's right edge; inside it, over it, where the screen ends.
        if (b.tab.x + b.tab.width > logical_w()) {
            b.tab.x = std::max(0.0, b.tab.x + shell::kBookmarkRoot - b.tab.width - 4);
            b.inside = true;
        }
        auto* n = bookmark_pool[k];
        show(n, std::move(pic.image), scale);
        wlr_scene_node_set_position(&n->node, static_cast<int>(std::lround(b.tab.x)), static_cast<int>(std::lround(b.tab.y)));
        wlr_scene_node_set_enabled(&n->node, true);
        if (b.under && !b.inside && b.under->parent == window_layer) wlr_scene_node_place_below(&n->node, b.under);
        else wlr_scene_node_raise_to_top(&n->node);
    }
    for (std::size_t k = next.size(); k < bookmark_pool.size(); ++k) wlr_scene_node_set_enabled(&bookmark_pool[k]->node, false);
    bookmarks = std::move(next);
    if (bookmark_lit >= static_cast<int>(bookmarks.size())) bookmark_lit = -1;
}

// The pointer over a bookmark: it lit, the item it is for outlined.
bool Server::bookmark_hover(double x, double y) {
    int at = -1;
    for (std::size_t k = 0; k < bookmarks.size(); ++k)
        if (bookmarks[k].shown().contains(x, y)) at = static_cast<int>(k);
    if (at == bookmark_lit) return at >= 0;
    bookmark_lit = at;
    if (!bookmark_outline[0]) {
        const float c[4] = {0, 0, 0, 0};
        for (auto*& r : bookmark_outline) r = wlr_scene_rect_create(shell_layer, 1, 1, c);
    }
    const bool show_outline = at >= 0 && bookmarks[static_cast<std::size_t>(at)].more.empty();
    const auto& it = show_outline ? bookmarks[static_cast<std::size_t>(at)].item : layout::Rect{};
    const float a[4] = {theme.accent.r / 255.0f, theme.accent.g / 255.0f, theme.accent.b / 255.0f, 1};
    const std::array<layout::Rect, 4> edges{{{it.x - 2, it.y - 2, it.width + 4, 2}, {it.x - 2, it.y + it.height, it.width + 4, 2},
                                             {it.x - 2, it.y, 2, it.height}, {it.x + it.width, it.y, 2, it.height}}};
    for (std::size_t e = 0; e < 4; ++e) {
        auto* r = bookmark_outline[e];
        wlr_scene_rect_set_color(r, a);
        wlr_scene_rect_set_size(r, std::max(1, int(std::lround(edges[e].width))), std::max(1, int(std::lround(edges[e].height))));
        wlr_scene_node_set_position(&r->node, int(std::lround(edges[e].x)), int(std::lround(edges[e].y)));
        wlr_scene_node_set_enabled(&r->node, show_outline);
        wlr_scene_node_raise_to_top(&r->node);
    }
    // Lit or not: drawn again.
    for (std::size_t k = 0; k < bookmarks.size() && k < bookmark_pool.size(); ++k) {
        const auto& b = bookmarks[k];
        auto pic = shell::bookmark(ui(), theme, b.label, b.colour, static_cast<int>(k) == bookmark_lit, scale, b.icon);
        show(bookmark_pool[k], std::move(pic.image), scale);
    }
    return at >= 0;
}

// Pressed on a bookmark: what it is for, raised and focused; "+N": the list.
bool Server::bookmark_press(double x, double y) {
    for (const auto& b : bookmarks) {
        if (!b.shown().contains(x, y)) continue;
        if (!b.more.empty()) {
            close_overlay(false);
            menu_of = MenuOf::bookmarks;
            menu_more = b.more;
            overlay = Overlay::menu;
            overlay_picture.x = b.tab.x;
            overlay_picture.y = b.tab.y + b.tab.height;
            draw_overlay();
            return true;
        }
        bookmark_choose(b.window, b.workspace);
        return true;
    }
    return false;
}

void Server::bookmark_choose(layout::Id window, int workspace) {
    publish("bookmark", {{"window", std::to_string(window)}, {"workspace", std::to_string(workspace)}});
    if (window) {
        if (lay.collapsed(window)) lay.toggle_collapsed(window);
        lay.focus(window);
        arrange();
        if (View* v = view_of(window)) focus(v);
        return;
    }
    if (workspace) {
        lay.raise(workspace);
        ws_selected = workspace;
        arrange();
    }
}

}  // namespace xdesk::compositor
