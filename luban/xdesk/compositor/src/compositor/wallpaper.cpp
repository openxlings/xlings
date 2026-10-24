// The wallpaper (server.cppm; design 2026-10-24 §9.7): what any desktop
// has, done by xdesk -- a colour, a gradient or a picture (fill, fit, tile,
// centre), for every workspace or one of its own ([wallpaper] and
// [wallpaper.<n>] in xdesk.ini) -- and room for more:
//
//   - in the WuXian view the plane's wallpaper is anchored on the plane (its
//     world), a workspace's on its plate: panning and zooming move and scale
//     them at once (the scene does it; nothing is drawn again), and drawn
//     again at the new zoom once the camera is still;
//   - the background layer of wlr-layer-shell: any wallpaper program (swaybg,
//     xdesk's own engine) draws there, anchored on the screen;
//   - topic/xdesk/camera (latest): where the camera is and what is seen, for
//     a wallpaper program that follows it; topic/xdesk/wallpaper: run or pause
//     (a window maximized or full screen, nothing done for five minutes).
//
// `kind = engine` starts the desktop's own engine (xdesk --wallpaper-engine,
// a background-layer client) and ends it when another kind is chosen.
module xdesk.compositor;

import std;
import xdesk.os;
import xdesk.draw;
import xdesk.shell;
import xdesk.config;
import xdesk.layout;
import openwx;
import openbus;
import xbus.node;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace draw = xdesk::draw;
namespace fs = std::filesystem;

namespace {

constexpr auto kIdle = std::chrono::minutes(5);

draw::Color rgb_(std::uint32_t v) { return draw::Color::rgb(v); }

// The picture of a paper at `w` x `h` logical pixels, `scale` device pixels each.
draw::Image paint_(const Paper& p, int w, int h, double scale) {
    draw::Image img(std::max(1, static_cast<int>(std::lround(w * scale))), std::max(1, static_cast<int>(std::lround(h * scale))));
    const int W = img.width(), H = img.height();
    auto put = [&](int x, int y, draw::Color c) {
        img.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] =
            (0xffu << 24) | (std::uint32_t(c.r) << 16) | (std::uint32_t(c.g) << 8) | c.b;
    };
    const auto a = rgb_(p.colour), b = rgb_(p.colour2);
    if (p.kind == "gradient") {
        for (int y = 0; y < H; ++y) {
            const double t = H > 1 ? double(y) / (H - 1) : 0;
            const draw::Color c{static_cast<std::uint8_t>(a.r + (b.r - a.r) * t), static_cast<std::uint8_t>(a.g + (b.g - a.g) * t),
                                static_cast<std::uint8_t>(a.b + (b.b - a.b) * t), 255};
            for (int x = 0; x < W; ++x) put(x, y, c);
        }
        return img;
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) put(x, y, a);
    if (p.kind != "image" || p.image.empty()) return img;
    std::expected<draw::Image, std::string> pic = p.image.extension() == ".svg" ? draw::read_svg(p.image, std::min(1024, std::max(W, H)))
                                                                              : draw::read_png(p.image);
    if (!pic || pic->empty()) return img;
    const double iw = pic->width(), ih = pic->height();
    if (p.fit == "tile") {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) img.data()[static_cast<std::size_t>(y) * W + x] = pic->data()[(y % pic->height()) * pic->width() + (x % pic->width())] | 0xff000000u;
        return img;
    }
    // fill: covers it, cut; fit: inside it, the colour around; centre: as it is.
    double s = p.fit == "fit" ? std::min(W / iw, H / ih) : p.fit == "center" ? 1.0 : std::max(W / iw, H / ih);
    const auto scaled = draw::scaled(*pic, std::max(1, static_cast<int>(iw * s)), std::max(1, static_cast<int>(ih * s)));
    const int ox = (W - scaled.width()) / 2, oy = (H - scaled.height()) / 2;
    for (int y = 0; y < scaled.height(); ++y) {
        const int ty = oy + y;
        if (ty < 0 || ty >= H) continue;
        for (int x = 0; x < scaled.width(); ++x) {
            const int tx = ox + x;
            if (tx < 0 || tx >= W) continue;
            const auto px = scaled.data()[static_cast<std::size_t>(y) * scaled.width() + x];
            const std::uint32_t al = px >> 24;
            auto& dst = img.data()[static_cast<std::size_t>(ty) * W + tx];
            // Premultiplied over the colour.
            auto ch = [&](int sh) { return ((px >> sh) & 0xff) + (((dst >> sh) & 0xff) * (255 - al) + 127) / 255; };
            dst = (0xffu << 24) | (std::min(255u, ch(16)) << 16) | (std::min(255u, ch(8)) << 8) | std::min(255u, ch(0));
        }
    }
    return img;
}

}  // namespace

// A paper as the settings say it: [wallpaper.<n>] for workspace n, else [wallpaper].
Paper Server::paper_of(int workspace) const {
    const auto s = config::load(o.config_profile, os::environment());
    const std::string own = workspace > 0 ? std::format("wallpaper.{}", workspace) : std::string{};
    const auto get = [&](std::string_view key, std::string_view fallback) {
        if (!own.empty())
            if (auto v = s.get(own, key, ""); !v.empty()) return v;
        return s.get("wallpaper", key, fallback);
    };
    Paper p;
    p.kind = get("kind", "solid");
    p.colour = s.colour(own.empty() || s.get(own, "colour", "").empty() ? std::string("wallpaper") : own, "colour",
                        (std::uint32_t(theme.background.r) << 16) | (std::uint32_t(theme.background.g) << 8) | theme.background.b);
    p.colour2 = s.colour(own.empty() || s.get(own, "colour2", "").empty() ? std::string("wallpaper") : own, "colour2", 0x0b0d12);
    p.image = get("image", "");
    // `~/...`: in the home of whoever runs the desktop.
    if (const auto text = p.image.string(); text.starts_with("~/")) {
        const auto env = os::environment();
        if (const auto home = env.find("HOME"); home != env.end()) p.image = std::filesystem::path(home->second) / text.substr(2);
    }
    p.fit = get("fit", "fill");
    p.engine = s.get("wallpaper", "engine", "reaction-diffusion");
    p.own = !own.empty() && !s.get(own, "kind", "").empty();
    return p;
}

void Server::wallpaper_update() {
    if (!wallpaper_node) wallpaper_node = wlr_scene_buffer_create(background_layer, nullptr);
    wlr_scene_node_place_above(&wallpaper_node->node, &background->node);
    // The engine: running while it is chosen.
    const auto plane = paper_of(0);
    const bool engine = plane.kind == "engine";
    if (engine && (wallpaper_engine_pid <= 0 || os::exited(wallpaper_engine_pid))) {
        if (const auto self = os::executable(); !self.empty()) {
            launch({self.string(), "--wallpaper-engine"}, "xdesk-wallpaper", "Wallpaper");
            wallpaper_engine_pid = children.empty() ? -1 : children.back();
        }
    } else if (!engine && wallpaper_engine_pid > 0) {
        (void)os::terminate(wallpaper_engine_pid);
        wallpaper_engine_pid = -1;
    }
    // Where it is drawn: on the screen (the workspace view), on the plane (WuXian).
    const bool on_plane = wuxian();
    const auto cam = lay.camera();
    const int w = logical_w(), h = logical_h();
    const Paper p = on_plane ? plane : paper_of(lay.current());
    // The plane's paper covers the world around what there is, at most a few screens.
    layout::Rect world{};
    if (on_plane) {
        const auto m = lay.minimap();
        world = m.bounds.width > 0 ? m.bounds : layout::Rect{-double(w), -double(h), 3.0 * w, 3.0 * h};
        const double padx = std::max(world.width, double(w)), pady = std::max(world.height, double(h));
        world = {world.x - padx, world.y - pady, world.width + 2 * padx, world.height + 2 * pady};
    }
    // Drawn again only when what it is, or its size, changed; on the plane at
    // the camera's zoom once it is still.
    const bool still = !view_move.has_value() && drag != Drag::pan;
    const double zoom_drawn = on_plane && still ? cam.zoom : wallpaper_zoom;
    const int dw = on_plane ? static_cast<int>(std::min(4096.0, world.width * std::min(1.0, zoom_drawn))) : w;
    const int dh = on_plane ? static_cast<int>(std::min(4096.0, world.height * std::min(1.0, zoom_drawn))) : h;
    const auto key = std::format("{}|{}|{:06x}|{:06x}|{}|{}|{}x{}@{}|{}", p.kind, on_plane, p.colour, p.colour2, p.image.string(), p.fit, dw, dh, scale,
                                 on_plane ? std::format("{:.0f},{:.0f}", world.x, world.y) : std::string{});
    if (key != wallpaper_drawn && p.kind != "engine") {
        wallpaper_drawn = key;
        wallpaper_zoom = zoom_drawn;
        wallpaper_world = world;
        show(wallpaper_node, paint_(p, std::max(1, dw), std::max(1, dh), on_plane ? 1.0 : scale), on_plane ? 1.0 : scale);
    }
    wlr_scene_node_set_enabled(&wallpaper_node->node, p.kind != "engine" && !background_client);
    if (on_plane) {
        // Moved and scaled with the camera, at once.
        const auto at = openwx::to_screen(cam, openwx::Point{wallpaper_world.x, wallpaper_world.y}, openwx::Point{0, 0});
        wlr_scene_node_set_position(&wallpaper_node->node, static_cast<int>(std::lround(at.x)), static_cast<int>(std::lround(at.y)));
        wlr_scene_buffer_set_dest_size(wallpaper_node, std::max(1, static_cast<int>(std::lround(wallpaper_world.width * cam.zoom))),
                                       std::max(1, static_cast<int>(std::lround(wallpaper_world.height * cam.zoom))));
    } else {
        wlr_scene_node_set_position(&wallpaper_node->node, 0, 0);
        wlr_scene_buffer_set_dest_size(wallpaper_node, w, h);
    }
    // Where the camera is, for a wallpaper program that follows it (latest only).
    if (bus && bus->connected()) {
        const auto said = std::format(R"({{"view":"{}","at":[{:.1f},{:.1f}],"zoom":{:.4f},"workspace":{},"size":[{},{}]}})", on_plane ? "wuxian" : "workspace",
                                      cam.at.x, cam.at.y, cam.zoom, lay.current(), w, h);
        if (said != camera_said) {
            camera_said = said;
            bus->publish("topic/xdesk/camera", said, static_cast<std::uint16_t>(openbus::Codec::json), openbus::Reliability::latest);
        }
    }
    wallpaper_pause_check();
}

void Server::wallpaper_restart() {
    if (wallpaper_engine_pid > 0) (void)os::terminate(wallpaper_engine_pid);
    wallpaper_engine_pid = -1;
    wallpaper_update();
}

// Run or pause, said when it changes: a window maximized or full screen over
// the workspace shown, or nothing done for five minutes.
void Server::wallpaper_pause_check() {
    bool covered = false;
    if (!wuxian())
        for (const auto& p : placements)
            if (p.visible && (p.maximized || p.fullscreen) && p.workspace == lay.current()) covered = true;
    const bool idle = std::chrono::steady_clock::now() - last_input > kIdle;
    const bool pause = covered || idle;
    if (pause == wallpaper_paused) return;
    wallpaper_paused = pause;
    const auto why = idle ? "idle" : covered ? "covered" : "";
    publish("wallpaper", {{"state", pause ? "pause" : "run"}, {"why", why}});
    if (bus && bus->connected())
        bus->publish("topic/xdesk/wallpaper", std::format(R"({{"state":"{}","why":"{}"}})", pause ? "pause" : "run", why),
                     static_cast<std::uint16_t>(openbus::Codec::json), openbus::Reliability::latest);
}

// The background layer (wlr-layer-shell): a wallpaper program's surface,
// anchored on the screen, under everything; the desktop's own paper is not
// drawn while one is there.
void Server::layer_surface_new(wlr_layer_surface_v1* s) {
    if (s->pending.layer != 0) {                     // only the background is this desktop's to give
        wlr_layer_surface_v1_destroy(s);
        return;
    }
    if (!s->output) s->output = output;
    auto& b = background_clients.emplace_back();
    b.surface = s;
    b.scene = wlr_scene_layer_surface_v1_create(background_layer, s);
    auto* self = &b;
    b.commit.connect(&s->surface->events.commit, [this, self](void*) {
        if (!self->surface->initialized) return;
        const wlr_box full{0, 0, static_cast<int>(logical_w()), static_cast<int>(logical_h())};
        wlr_box usable = full;
        wlr_scene_layer_surface_v1_configure(self->scene, &full, &usable);
        background_client = true;
        wlr_scene_node_set_enabled(&wallpaper_node->node, false);
    });
    b.destroy.connect(&s->events.destroy, [this, self](void*) {
        background_clients.remove_if([self](const BackgroundClient& c) { return &c == self; });
        background_client = !background_clients.empty();
        wallpaper_drawn.clear();
        wallpaper_update();
    });
}

}  // namespace xdesk::compositor
