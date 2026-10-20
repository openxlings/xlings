// The model's rules (openwx.cppm, SPEC.md §2, §3): names, transforms, the
// order of hits, numbers, where a new window goes.
module openwx;

import std;

namespace openwx {

std::string_view name(Rule r) { return r == Rule::stack ? "stack" : "tile"; }
std::string_view name(Flow f) { return f == Flow::vertical ? "vertical" : "horizontal"; }
std::string_view name(View v) { return v == View::wuxian ? "wuxian" : "workspace"; }

std::optional<Rule> rule_named(std::string_view s) {
    if (s == "tile") return Rule::tile;
    if (s == "stack") return Rule::stack;
    return std::nullopt;
}

std::optional<Flow> flow_named(std::string_view s) {
    if (s == "horizontal") return Flow::horizontal;
    if (s == "vertical") return Flow::vertical;
    return std::nullopt;
}

std::optional<View> view_named(std::string_view s) {
    if (s == "workspace") return View::workspace;
    if (s == "wuxian") return View::wuxian;
    return std::nullopt;
}

// ── transforms ───────────────────────────────────────────────────────

Rect world_rect(const Workspace& w, Size content) {
    return {w.origin.x, w.origin.y, content.width * w.scale, content.height * w.scale};
}

Point content_to_world(const Workspace& w, Point content) {
    return {w.origin.x + content.x * w.scale, w.origin.y + content.y * w.scale};
}

Point world_to_content(const Workspace& w, Point world) {
    const double s = w.scale > 0 ? w.scale : 1;
    return {(world.x - w.origin.x) / s, (world.y - w.origin.y) / s};
}

Point to_screen(const Camera& c, Point world, Point area) {
    return {area.x + (world.x - c.at.x) * c.zoom, area.y + (world.y - c.at.y) * c.zoom};
}

Rect to_screen(const Camera& c, Rect world, Point area) {
    const auto p = to_screen(c, Point{world.x, world.y}, area);
    return {p.x, p.y, world.width * c.zoom, world.height * c.zoom};
}

Point to_world(const Camera& c, Point screen, Point area) {
    const double z = c.zoom > 0 ? c.zoom : 1;
    return {c.at.x + (screen.x - area.x) / z, c.at.y + (screen.y - area.y) / z};
}

Camera framing(Rect world, Size view, double margin) {
    const double w = std::max(1.0, world.width + 2 * margin), h = std::max(1.0, world.height + 2 * margin);
    const double zoom = std::clamp(std::min(view.width / w, view.height / h), kMinZoom, kMaxZoom);
    const auto c = world.centre();
    return {.at = {c.x - view.width / 2 / zoom, c.y - view.height / 2 / zoom}, .zoom = zoom};
}

Camera zoomed(const Camera& c, double factor, Point screen, Point area) {
    if (factor <= 0) return c;
    const auto world = to_world(c, screen, area);
    Camera out = c;
    out.zoom = std::clamp(c.zoom * factor, kMinZoom, kMaxZoom);
    out.at = {world.x - (screen.x - area.x) / out.zoom, world.y - (screen.y - area.y) / out.zoom};
    return out;
}

// ── hits ─────────────────────────────────────────────────────────────

std::optional<Layer> hit(std::span<const Layer> layers, Point world) {
    const Layer* best = nullptr;
    for (const auto& l : layers) {
        if (!l.world.contains(world)) continue;
        if (!best || (l.pinned && !best->pinned) || (l.pinned == best->pinned && l.z >= best->z)) best = &l;
    }
    return best ? std::optional(*best) : std::nullopt;
}

int top_z(std::span<const Layer> layers) {
    int z = 0;
    for (const auto& l : layers) z = std::max(z, l.z + 1);
    return z;
}

// ── numbers and new windows ──────────────────────────────────────────

std::optional<int> next_id(std::span<const Workspace> ws, int limit) {
    for (int id = 1; limit <= 0 || id <= limit; ++id)
        if (std::ranges::none_of(ws, [&](const Workspace& w) { return w.id == id; })) return id;
    return std::nullopt;
}

std::uint32_t color_for(int id) {
    // Apart in hue, alike in lightness: none stands out over the others.
    static constexpr std::array<std::uint32_t, 9> kColors{0x5e9cff, 0x3fbfb0, 0xe0a94a, 0xd975b0, 0x8fc461,
                                                          0xa58cf0, 0xe0795a, 0x56b8e0, 0xc8b45a};
    return kColors[static_cast<std::size_t>((std::max(1, id) - 1) % static_cast<int>(kColors.size()))];
}

Destination new_window(View view, int current, std::span<const Layer> layers, Point view_centre) {
    if (view == View::workspace) return {.workspace = current};
    std::vector<Layer> spaces;
    for (const auto& l : layers)
        if (l.kind == Layer::Kind::workspace) spaces.push_back(l);
    if (const auto h = hit(spaces, view_centre)) return {.workspace = h->workspace};
    return {.plane = true};
}

}  // namespace openwx
