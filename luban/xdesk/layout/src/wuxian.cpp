// WuXian (layout.cppm; design 2026-10-20 WuXian §3): the plane the desktop
// is, its camera and the two views -- the workspace view, the current
// workspace's content 1:1 on the output; the WuXian view, every workspace as
// its content scaled onto the plane and the free windows beside them, seen
// through a camera that pans and zooms. Arranging both is here.
module xdesk.layout;

import std;
import openwx;

namespace xdesk::layout {

using detail::overlaps;

namespace {

// Room around what the camera frames, as a part of the output.
constexpr double kFrameMargin = 0.08;

}  // namespace

// ── a workspace on the plane ─────────────────────────────────────────

// Its content laid out whole: one output (stack), or one output across and
// as long as its strip along its flow (tile), at least one output.
Size Layout::content_(const Space& s) const {
    const Rect area = area_();
    if (s.meta.rule != Rule::tile || s.columns.empty()) return {area.width, area.height};
    const bool vertical = s.meta.flow == Flow::vertical;
    const double along = vertical ? area.height : area.width;
    const auto ws = tile_widths_(s, along);
    double strip = 2 * o_.outer_gap + o_.inner_gap * static_cast<double>(ws.size() - 1);
    for (const double w : ws) strip += w;
    const double length = std::max(along, strip);
    return vertical ? Size{area.width, length} : Size{length, area.height};
}

Rect Layout::world_of_(const Space& s) const { return openwx::world_rect(s.meta, content_(s)); }

// Where a window is on the plane now: a free one's frame; a workspace's, its
// place in the content laid out whole, on the plane.
std::optional<Rect> Layout::world_rect_of_(Id id) const {
    if (const auto* f = free_of_(id)) return f->world;
    const auto* s = space_of_(id);
    if (!s) return std::nullopt;
    const Rect area = area_();
    Space copy = *s;
    for (const auto& p : const_cast<Layout*>(this)->arrange_space_(copy, area, true))
        if (p.id == id) {
            const auto at = openwx::content_to_world(s->meta, {p.rect.x - area.x, p.rect.y - area.y});
            return Rect{at.x, at.y, p.rect.width * s->meta.scale, p.rect.height * s->meta.scale};
        }
    return std::nullopt;
}

// Floating: in the middle of the output, as big as it asked (or fits).
void Layout::place_new_float_(Window& w) {
    const Rect area = area_();
    const double width = std::min(w.preferred.width, area.width - 2 * o_.inner_gap);
    const double height = std::min(w.preferred.height, area.height - 2 * o_.inner_gap);
    w.free = {area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height};
    w.placed = true;
}

// A free window's place (design 2026-10-20 §9.5): the free spot nearest
// `near` (else the middle of the view) -- beside it, then further out on a
// grid of its size -- in view when one is.
Rect Layout::free_place_(Size size, std::optional<Rect> near) const {
    const Rect area = area_();
    const double zoom = view_ == View::wuxian ? camera_.zoom : 1;
    const auto at = view_ == View::wuxian ? camera_.at : Point{0, 0};
    const Rect view{at.x, at.y, area.width / zoom, area.height / zoom};
    const double width = std::max(120.0, size.width), height = std::max(80.0, size.height);
    const double m = 3 * o_.inner_gap + 8;
    const auto ls = layers_();
    auto free = [&](const Rect& r) {
        const Rect padded{r.x - m / 2, r.y - m / 2, r.width + m, r.height + m};
        return std::ranges::none_of(ls, [&](const openwx::Layer& l) { return overlaps(l.world, padded); });
    };
    auto inside = [&](const Rect& r) {
        return r.x >= view.x && r.y >= view.y && r.x + r.width <= view.x + view.width && r.y + r.height <= view.y + view.height;
    };
    const Rect anchor = near.value_or(Rect{view.x + (view.width - width) / 2, view.y + (view.height - height) / 2, width, height});
    std::vector<Rect> places;
    if (near) {
        places.push_back({anchor.x + anchor.width + m, anchor.y, width, height});
        places.push_back({anchor.x, anchor.y + anchor.height + m, width, height});
        places.push_back({anchor.x - width - m, anchor.y, width, height});
        places.push_back({anchor.x, anchor.y - height - m, width, height});
    } else {
        places.push_back(anchor);
    }
    const auto c = anchor.centre();
    std::vector<std::pair<double, Rect>> ring;
    for (int i = -6; i <= 6; ++i)
        for (int j = -6; j <= 6; ++j) {
            const Rect r{c.x - width / 2 + i * (width + m), c.y - height / 2 + j * (height + m), width, height};
            ring.push_back({std::hypot(r.x + width / 2 - c.x, r.y + height / 2 - c.y), r});
        }
    std::ranges::stable_sort(ring, {}, &std::pair<double, Rect>::first);
    for (const auto& [_, r] : ring) places.push_back(r);
    for (const auto& r : places)
        if (free(r) && inside(r)) return r;
    for (const auto& r : places)
        if (free(r)) return r;
    return places.front();
}

// ── the views and the camera ─────────────────────────────────────────

void Layout::set_view(View v) {
    if (v == view_) return;
    if (v == View::wuxian) {
        view_ = View::wuxian;
        frame_workspace(current_);
        return;
    }
    view_ = View::workspace;
    // The workspace focused is the current one; a free window focused: back
    // to the workspace it was, to the window it had.
    if (focus_)
        if (const auto* s = space_of_(*focus_)) current_ = s->meta.id;
    if (!focus_ || is_free(*focus_)) focus_ = cur_().focus;
}

void Layout::look(const Camera& c) {
    camera_ = c;
    camera_.zoom = std::clamp(c.zoom, openwx::kMinZoom, openwx::kMaxZoom);
}

Camera Layout::locked_camera() const { return locked_camera(current_); }

int Layout::focused_workspace() const {
    if (focus_)
        if (const auto* s = space_of_(*focus_)) return s->meta.id;
    return current_;
}

Camera Layout::locked_camera(int id) const {
    const auto* found = space_(id);
    const auto& s = found ? *found : cur_();
    const double scale = s.meta.scale > 0 ? s.meta.scale : 1;
    Point at = s.meta.origin;
    if (s.meta.rule == Rule::tile) {
        if (s.meta.flow == Flow::vertical) at.y += s.scroll * scale;
        else at.x += s.scroll * scale;
    }
    return {.at = at, .zoom = 1 / scale};
}

void Layout::pan(double dx, double dy) {
    if (view_ == View::workspace) {
        auto& s = cur_();
        if (s.meta.rule != Rule::tile) return;
        s.scroll -= s.meta.flow == Flow::vertical ? dy : dx;
        s.scrolled = true;
        return;
    }
    camera_.at.x -= dx / camera_.zoom;
    camera_.at.y -= dy / camera_.zoom;
}

void Layout::zoom_at(double factor, double sx, double sy) {
    if (view_ != View::wuxian || factor <= 0) return;
    const Rect area = area_();
    camera_ = openwx::zoomed(camera_, factor, {sx, sy}, {area.x, area.y});
}

double Layout::zoom() const { return view_ == View::wuxian ? camera_.zoom : 1.0; }

void Layout::frame_workspace(int id) {
    const auto* s = space_(id);
    if (!s) return;
    const Rect area = area_();
    camera_ = openwx::framing(world_of_(*s), {area.width, area.height}, kFrameMargin * std::min(area.width, area.height));
}

void Layout::show_all() {
    std::optional<Rect> box;
    for (const auto& l : layers_()) box = box ? detail::bounds(*box, l.world) : l.world;
    if (!box) return;
    const Rect area = area_();
    camera_ = openwx::framing(*box, {area.width, area.height}, 40);
}

void Layout::center(Id id) {
    const auto r = world_rect_of_(id);
    if (!r) return;
    const Rect area = area_();
    camera_.zoom = 1;
    camera_.at = {r->x + r->width / 2 - area.width / 2, r->y + r->height / 2 - area.height / 2};
}

void Layout::look_at(double wx, double wy) {
    const Rect area = area_();
    camera_.at = {wx - area.width / camera_.zoom / 2, wy - area.height / camera_.zoom / 2};
}

std::optional<int> Layout::workspace_at(double sx, double sy) const {
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it)
        if (it->screen.contains(sx, sy)) return it->id;
    return std::nullopt;
}

// Keeps a window in view (the WuXian view): the least pan that shows it.
// The workspace view's tiled strip scrolls to the focus as it is arranged.
void Layout::reveal_(Id id) {
    if (view_ != View::wuxian) return;
    const auto r = world_rect_of_(id);
    if (!r) return;
    const Rect area = area_();
    const double vw = area.width / camera_.zoom, vh = area.height / camera_.zoom, m = 2 * o_.inner_gap / camera_.zoom;
    auto& c = camera_.at;
    if (r->x - m < c.x || r->width + 2 * m > vw) c.x = r->x - m;
    else if (r->x + r->width + m > c.x + vw) c.x = r->x + r->width + m - vw;
    if (r->y - m < c.y || r->height + 2 * m > vh) c.y = r->y - m;
    else if (r->y + r->height + m > c.y + vh) c.y = r->y + r->height + m - vh;
}

// ── snapping and the minimap (design 2026-10-20 §9.4, §9.6) ─────────

Snap Layout::snap(Id id, Rect screen, double reach) const {
    Snap out{.rect = screen};
    if (view_ == View::workspace && rule() != Rule::stack) return out;
    const auto mine = group_of(id);
    double best_x = reach + 1, best_y = reach + 1;
    const double g = o_.inner_gap;
    for (const auto& p : last_) {
        if (!p.visible || std::ranges::find(mine, p.id) != mine.end()) continue;
        const auto& o = p.rect;
        // Edges in line, or against each other a gap apart.
        for (const auto [want, have] : {std::pair{o.x, screen.x}, {o.x + o.width, screen.x + screen.width},
                                        {o.x + o.width + g, screen.x}, {o.x - g, screen.x + screen.width}})
            if (const double d = want - have; std::abs(d) < std::abs(best_x)) {
                best_x = d;
                out.guide_x = want;
            }
        for (const auto [want, have] : {std::pair{o.y, screen.y}, {o.y + o.height, screen.y + screen.height},
                                        {o.y + o.height + g, screen.y}, {o.y - g, screen.y + screen.height}})
            if (const double d = want - have; std::abs(d) < std::abs(best_y)) {
                best_y = d;
                out.guide_y = want;
            }
    }
    if (std::abs(best_x) <= reach) out.rect.x += best_x;
    else out.guide_x.reset();
    if (std::abs(best_y) <= reach) out.rect.y += best_y;
    else out.guide_y.reset();
    return out;
}

Minimap Layout::minimap() const {
    Minimap m;
    if (view_ != View::wuxian) return m;
    const Rect area = area_();
    m.view = {camera_.at.x, camera_.at.y, area.width / camera_.zoom, area.height / camera_.zoom};
    m.bounds = m.view;
    for (const auto& l : layers_()) {
        m.boxes.push_back(l.world);
        m.bounds = detail::bounds(m.bounds, l.world);
    }
    return m;
}

// ── arranging ────────────────────────────────────────────────────────

// A workspace's windows by its rule, then its floating ones, in the
// workspace view's pixels of `area` (`whole`: a tiled strip unscrolled).
std::vector<Placement> Layout::arrange_space_(Space& s, Rect area, bool whole) {
    s.shown[s.meta.rule == Rule::stack ? 1 : 0] = true;
    auto out = s.meta.rule == Rule::tile ? tile_(s, area, whole) : stack_(s, area);
    for (auto& w : s.windows) {
        if (!w.floating || w.pinned) continue;
        if (!w.placed) place_new_float_(w);
        out.push_back({.id = w.id, .rect = w.free, .visible = overlaps(w.free, area), .floating = true, .workspace = s.meta.id});
    }
    return out;
}

// The WuXian view through `cam`: the layers from the lowest -- each
// workspace its content on the plane, each free window -- with every
// workspace's frame on the screen.
std::vector<Placement> Layout::wuxian_(Rect area, const Camera& cam) {
    std::vector<Placement> out;
    struct Item {
        int z;
        Space* space;
        Free* free;
    };
    std::vector<Item> items;
    for (auto& s : spaces_) items.push_back({s.meta.z, &s, nullptr});
    for (auto& f : free_) items.push_back({f.z, nullptr, &f});
    std::ranges::stable_sort(items, {}, &Item::z);
    const Point origin{area.x, area.y};
    for (const auto& it : items) {
        if (it.free) {
            const auto r = openwx::to_screen(cam, it.free->world, origin);
            out.push_back({.id = it.free->w.id, .rect = r, .visible = overlaps(r, area), .zoom = cam.zoom});
            continue;
        }
        auto& s = *it.space;
        const double z = s.meta.scale * cam.zoom;
        auto ps = arrange_space_(s, area, true);
        // Those on every workspace, where they are on the current one.
        if (s.meta.id == current_)
            for (auto& w : sticky_) {
                if (w.pinned) continue;
                if (!w.placed) place_new_float_(w);
                ps.push_back({.id = w.id, .rect = w.free, .floating = true, .workspace = s.meta.id});
            }
        const auto frame = openwx::to_screen(cam, world_of_(s), origin);
        frames_.push_back({.id = s.meta.id, .screen = frame, .zoom = z, .current = s.meta.id == current_});
        for (auto p : ps) {
            const auto at = openwx::content_to_world(s.meta, {p.rect.x - area.x, p.rect.y - area.y});
            p.rect = openwx::to_screen(cam, Rect{at.x, at.y, p.rect.width * s.meta.scale, p.rect.height * s.meta.scale}, origin);
            p.visible = overlaps(p.rect, area);
            p.zoom = z;
            out.push_back(p);
        }
    }
    return out;
}

std::vector<Placement> Layout::arrange(Size output, Insets reserved, std::optional<Camera> through) {
    output_ = output;
    reserved_ = reserved;
    frames_.clear();
    const Rect area = area_();
    const bool wuxian = view_ == View::wuxian || through.has_value();
    std::vector<Placement> out = wuxian ? wuxian_(area, through.value_or(camera_)) : arrange_space_(cur_(), area, false);
    if (!wuxian)
        for (auto& w : sticky_) {
            if (w.pinned) continue;
            if (!w.placed) place_new_float_(w);
            out.push_back({.id = w.id, .rect = w.free, .visible = overlaps(w.free, area), .floating = true, .workspace = current_});
        }
    // Always on top, where it is on the screen whatever the view (design 2026-10-19 §18.2).
    for (auto* list : {&cur_().windows, &sticky_})
        for (auto& w : *list) {
            if (!w.floating || !w.pinned) continue;
            if (!w.placed) place_new_float_(w);
            out.push_back({.id = w.id, .rect = w.free, .visible = overlaps(w.free, area), .floating = true, .pinned = true,
                           .workspace = current_});
        }
    const auto f = focused();
    for (auto& p : out) p.focused = f == p.id;
    // Full screen (the workspace view): the whole output, last -- above everything.
    std::erase_if(fullscreen_, [&](Id id) { return !window_(id); });
    if (!wuxian)
        if (auto it = std::ranges::find_if(out, [&](const Placement& p) { return fullscreen_.contains(p.id); }); it != out.end()) {
            auto p = *it;
            out.erase(it);
            p.rect = {0, 0, output.width, output.height};
            p.visible = true;
            p.fullscreen = true;
            p.tiled = true;
            out.push_back(p);
        }
    last_ = out;
    return out;
}

}  // namespace xdesk::layout
