// The windows showing the desktop (server.cppm; design 2026-10-20 §11): the
// first is the primary -- the output is its size and density -- and every
// window that comes to the displays socket mirrors it: the same frames
// (render), its input the desktop's -- or, asking to extend it, is an output
// of its own right of the others (design 2026-10-24 §4.4). Each is told what
// it is ('d'); when the primary goes, the next one is the primary, and the
// desktop goes on.
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import xdesk.os;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import xdesk.layout;
import :server;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace layout = xdesk::layout;

std::string Server::hello() const {
    using I = opensal::Interface;
    return opensal::wire::encode('h', std::to_string(I::Surface | I::Mode | I::Keyboard | I::Pointer | I::Text | I::Clipboard
                                                     | I::ClipboardData | I::Cursor | I::CursorImage
                                                     | (region ? static_cast<opensal::Interfaces>(I::Region) : 0)));
}

Link& Server::add_link(int in, int out) {
    auto& l = links.emplace_back();
    l.in = in;
    l.out = out;
    std::uint32_t n = 1;
    while (std::ranges::any_of(links, [&](const Link& o) { return &o != &l && o.number == n; })) ++n;
    l.number = n;
    l.server = this;
    l.source = wl_event_loop_add_fd(loop, in, static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                    [](int, std::uint32_t mask, void* data) -> int {
                                        auto* link = static_cast<Link*>(data);
                                        std::array<char, 65536> buf{};
                                        auto n = os::read_some(link->in, buf);
                                        if (!n || *n == 0 || (mask & static_cast<std::uint32_t>(WL_EVENT_HANGUP_))) {
                                            link->gone = true;
                                            link->server->drop_gone();
                                            return 0;
                                        }
                                        link->server->provider(*link, std::string_view(buf.data(), *n));
                                        return 0;
                                    },
                                    &l);
    return l;
}

// Another window: a display that mirrors the primary.
void Server::accept_display() {
    const int fd = os::accept(displays_listener);
    if (fd < 0) return;
    auto& l = add_link(fd, fd);
    log(std::format("display {} joined (a mirror of display {})", l.number, links.front().number));
    if (!os::write_all(fd, hello())) { l.gone = true; drop_gone(); return; }
    // The pointer as the others have it (said to every window again).
    pointer_mode();
}

// Windows that went: their streams closed; when it was the primary, the next
// one is (the output takes its size), and with none the desktop ends.
void Server::drop_gone() {
    if (std::ranges::none_of(links, &Link::gone)) return;
    const bool primary_went = !links.empty() && links.front().gone;
    for (auto it = links.begin(); it != links.end();) {
        if (!it->gone) { ++it; continue; }
        log(std::format("display {} left", it->number));
        if (it->extend) extend_remove(*it);
        if (it->source) wl_event_source_remove(it->source);
        os::close(it->in);
        if (it->out != it->in) os::close(it->out);
        it = links.erase(it);
    }
    if (links.empty()) {
        stop = true;
        return;
    }
    if (primary_went) {
        // The primary is a display that shows the desktop's own output: one
        // that extended it gives its output up.
        auto first = std::ranges::find_if(links, [](const Link& l) { return !l.extend; });
        if (first == links.end()) first = links.begin();
        if (first != links.begin()) links.splice(links.begin(), links, first);
        auto& p = links.front();
        if (p.extend) extend_remove(p);
        provided = p.provided;
        pointer_mode();
        // The output is its size now (a window gone, the virtual display's
        // again: design 2026-10-24 §4.3).
        if (p.own.width > 0 && p.own.height > 0) resize(p.own);
        if (p.density > 0) set_scale(p.density);
        p.whole = true;
        log(std::format("display {} is the primary", p.number));
    }
    tell_roles();
}

// Each window what it is ('d', to those that take it): the primary, or a
// mirror of it at its size.
void Server::tell_roles() {
    if (links.empty()) return;
    const auto& p = links.front();
    for (auto& l : links) {
        if (!l.hello || !opensal::has(l.provided, opensal::Interface::Displays)) continue;
        opensal::DisplayRole r{.number = l.number};
        if (l.extend) {
            r.kind = opensal::DisplayRole::Kind::extend;
            r.x = static_cast<std::int32_t>(std::lround(l.extend->x));
        } else if (&l != &p) {
            r.kind = opensal::DisplayRole::Kind::mirror;
            r.of = p.number;
            r.size = size;
        }
        if (!os::write_all(l.out, opensal::wire::encode('d', opensal::wire::encode_display(r)))) l.gone = true;
    }
}

// ── extending outputs (design 2026-10-24 §4.4; openwx 0.2 §2.6) ─────

// A display that asked to extend the desktop: a headless output of its own
// at its size and density, right of the others; in the workspace view the
// workspace it showed when it was here before, else one nobody shows (made
// when there is none).
void Server::extend_add(Link& l) {
    auto& e = extends.emplace_back();
    e.link = &l;
    e.name = l.name.empty() ? std::format("display-{}", l.number) : l.name;
    e.size = l.own.width > 0 && l.own.height > 0 ? l.own : size;
    e.scale = l.density > 0 ? l.density : 1;
    l.extend = &e;
    e.output = wlr_headless_add_output(backend, e.size.width, e.size.height);
    wlr_output_init_render(e.output, allocator, renderer);
    wlr_output_state on;
    wlr_output_state_init(&on);
    wlr_output_state_set_enabled(&on, true);
    wlr_output_state_set_render_format(&on, kARGB8888);
    wlr_output_state_set_scale(&on, static_cast<float>(e.scale));
    wlr_output_commit_state(e.output, &on);
    wlr_output_state_finish(&on);
    e.scene_output = wlr_scene_output_create(scene, e.output);
    const float bg[4] = {theme.background.r / 255.0f, theme.background.g / 255.0f, theme.background.b / 255.0f, 1.0f};
    e.background = wlr_scene_rect_create(background_layer, 1, 1, bg);
    e.frame.connect(&e.output->events.frame, [this, &e](void*) { render_on(&e); });
    // Its workspace: as it was, else the lowest nobody shows, else a new one.
    const auto shown = [&](int id) {
        return id == lay.current() || std::ranges::any_of(extends, [&](const Extend& o) { return &o != &e && o.workspace == id; });
    };
    if (const auto was = extend_shown.find(e.name); was != extend_shown.end() && lay.workspace_info(was->second) && !shown(was->second))
        e.workspace = was->second;
    for (const int id : lay.workspaces())
        if (!e.workspace && !shown(id)) e.workspace = id;
    if (!e.workspace) e.workspace = lay.add_workspace(lay.rule());
    extends_place();
    log(std::format("display {} extends the desktop: {} ({}x{}) at {}, workspace {}", l.number, e.name, e.size.width, e.size.height, e.x,
                    e.workspace));
    publish("output", {{"change", "added"}, {"name", e.name}, {"workspace", std::to_string(e.workspace)}});
    arrange();
}

// Its display went (or became the primary): the output gone, the workspace it
// showed remembered by its name -- its windows stay in it, a key away.
void Server::extend_remove(Link& l) {
    auto* e = l.extend;
    if (!e) return;
    extend_shown[e->name] = e->workspace;
    publish("output", {{"change", "removed"}, {"name", e->name}, {"workspace", std::to_string(e->workspace)}});
    e->frame.disconnect();
    if (e->background) wlr_scene_node_destroy(&e->background->node);
    if (e->scene_output) wlr_scene_output_destroy(e->scene_output);
    if (e->layout_output) wlr_output_layout_remove(outputs, e->output);
    if (e->output) wlr_output_destroy(e->output);
    l.extend = nullptr;
    std::erase_if(extends, [&](const Extend& x) { return &x == e; });
    extends_place();
    arrange();
}

void Server::extend_resize(Extend& e, opensal::Size want, double density) {
    if (want.width == 0 || want.height == 0) want = e.size;
    if (density <= 0) density = e.scale;
    if (want == e.size && std::abs(density - e.scale) < 0.01) return;
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_custom_mode(&st, static_cast<std::int32_t>(want.width), static_cast<std::int32_t>(want.height), 0);
    wlr_output_state_set_scale(&st, static_cast<float>(density));
    if (wlr_output_commit_state(e.output, &st)) {
        e.size = want;
        e.scale = density;
        extends_place();
        arrange();
    }
    wlr_output_state_finish(&st);
}

// Left to right after the primary, in the order they came; each told where it is.
void Server::extends_place() {
    double x = logical_w();
    for (auto& e : extends) {
        e.x = x;
        const bool first = !e.layout_output;
        e.layout_output = wlr_output_layout_add(outputs, e.output, static_cast<int>(std::lround(x)), 0);
        if (first) wlr_scene_output_layout_add_output(scene_layout, e.layout_output, e.scene_output);
        const auto lw = e.logical();
        wlr_scene_rect_set_size(e.background, std::max(1, static_cast<int>(std::lround(lw.width))), std::max(1, static_cast<int>(std::lround(lw.height))));
        wlr_scene_node_set_position(&e.background->node, static_cast<int>(std::lround(x)), 0);
        x += lw.width;
        if (e.link) e.link->whole = true;
    }
    tell_roles();
}

Extend* Server::extend_at(double x, double y) {
    for (auto& e : extends) {
        const auto lw = e.logical();
        if (x >= e.x && x < e.x + lw.width && y >= 0 && y < lw.height) return &e;
    }
    return nullptr;
}

// In the workspace view each extending output shows its workspace beside the
// primary's: those windows placed there. A workspace the primary went to is
// the primary's: the output that showed it takes the one the primary left.
void Server::with_extends(std::vector<layout::Placement>& ps) {
    const int current = lay.current();
    for (auto& e : extends) {
        if (e.workspace == current && arranged_current && arranged_current != current) e.workspace = arranged_current;
        if (!lay.workspace_info(e.workspace) || e.workspace == current) {
            e.workspace = 0;
            for (const int id : lay.workspaces())
                if (!e.workspace && id != current && std::ranges::none_of(extends, [&](const Extend& o) { return &o != &e && o.workspace == id; }))
                    e.workspace = id;
        }
    }
    arranged_current = current;
    if (wuxian()) return;
    const auto* f = focused();
    for (auto& e : extends) {
        if (!e.workspace) continue;
        for (auto p : lay.arrange_on(e.workspace, e.logical())) {
            p.rect.x += e.x;
            p.focused = f && f->id == p.id;
            ps.push_back(p);
        }
    }
}

}  // namespace xdesk::compositor
