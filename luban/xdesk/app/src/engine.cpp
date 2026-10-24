// `xdesk --wallpaper-engine` (design 2026-10-24 §9.7): the desktop's own
// wallpaper engine -- a client on the background layer (wlr-layer-shell),
// anchored on the screen, drawing one of xdesk.scenes: computed at a quarter
// of the screen's size, shown at all of it, at most 30 frames a second
// (`[wallpaper] speed`), paused while the desktop says so (a window covers
// the screen, nobody has done anything for five minutes: topic/xdesk/wallpaper)
// and following its camera (topic/xdesk/camera). The pointer over the
// desktop's background reaches the scene. Which scene: `[wallpaper] engine`.
export module xdesk.engine;

import std;
import xdesk.os;
import xdesk.waywire;
import xdesk.scenes;
import xdesk.config;
import xbus.node;

namespace os = xdesk::os;
namespace ww = xdesk::waywire;

namespace {

// The small picture over the whole buffer, smoothly (bilinear).
void upscale_(std::span<const std::uint32_t> src, int sw, int sh, std::uint32_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const double fy = std::max(0.0, (y + 0.5) * sh / dh - 0.5);
        const int y0 = std::min(sh - 1, static_cast<int>(fy)), y1 = std::min(sh - 1, y0 + 1);
        const double ty = fy - y0;
        for (int x = 0; x < dw; ++x) {
            const double fx = std::max(0.0, (x + 0.5) * sw / dw - 0.5);
            const int x0 = std::min(sw - 1, static_cast<int>(fx)), x1 = std::min(sw - 1, x0 + 1);
            const double tx = fx - x0;
            const auto p = [&](int xx, int yy) { return src[static_cast<std::size_t>(yy) * sw + xx]; };
            std::uint32_t out = 0xff000000u;
            for (int sh_ = 0; sh_ <= 16; sh_ += 8) {
                const auto c = [&](std::uint32_t v) { return double((v >> sh_) & 0xff); };
                const double top = c(p(x0, y0)) * (1 - tx) + c(p(x1, y0)) * tx, bot = c(p(x0, y1)) * (1 - tx) + c(p(x1, y1)) * tx;
                out |= static_cast<std::uint32_t>(top * (1 - ty) + bot * ty + 0.5) << sh_;
            }
            dst[static_cast<std::size_t>(y) * dw + x] = out;
        }
    }
}

struct Buffer {
    std::optional<os::SharedMemory> memory;
    std::uint32_t pool { 0 }, id { 0 };
    bool busy { false };
};

}  // namespace

export namespace xdesk::engine {

int run() {
    auto wire = ww::Wire::connect();
    if (!wire) { std::println(std::cerr, "xdesk --wallpaper-engine: {}", wire.error()); return 2; }
    auto& w = *wire;
    auto g = w.globals();
    if (!g) { std::println(std::cerr, "xdesk --wallpaper-engine: {}", g.error()); return 1; }
    const auto compositor = w.bind(*g, "wl_compositor", 4), shm = w.bind(*g, "wl_shm", 1);
    const auto layers = w.bind(*g, "zwlr_layer_shell_v1", 4), seat = w.bind(*g, "wl_seat", 5);
    if (!compositor || !shm || !layers) { std::println(std::cerr, "xdesk --wallpaper-engine: the desktop has no background layer"); return 1; }
    // Its settings: which scene, how fast.
    const auto settings = xdesk::config::load("xdesk", os::environment());
    auto scene = xdesk::scenes::make(settings.get("wallpaper", "engine", "reaction-diffusion"),
                                     static_cast<std::uint32_t>(settings.get_int("wallpaper", "seed", 1)));
    const auto speed_name = settings.get("wallpaper", "speed", "normal");
    const double speed = speed_name == "slow" ? 0.5 : speed_name == "fast" ? 2.0 : 1.0;
    const auto frame_gap = std::chrono::milliseconds(speed >= 2 ? 33 : 50);
    // What the desktop says: pause or run, where its camera is.
    std::atomic<bool> paused{false};
    std::mutex cam_m;
    std::optional<std::array<double, 3>> camera;
    std::shared_ptr<xbus::Node> bus;
    if (auto n = xbus::Node::connect({.name = "xdesk-wallpaper", .actor = "program:xdesk-wallpaper", .telemetry = false, .patient = true})) {
        bus = *n;
        bus->subscribe("topic/xdesk/wallpaper", [&](const xbus::Message& m) { paused = m.body.find("\"pause\"") != std::string::npos; });
        bus->subscribe("topic/xdesk/camera", [&](const xbus::Message& m) {
            double x = 0, y = 0, z = 1;
            if (std::sscanf(m.body.c_str(), "%*[^[][%lf,%lf],\"zoom\":%lf", &x, &y, &z) >= 2) {
                std::lock_guard l(cam_m);
                camera = {x, y, z};
            }
        });
    }
    // On the background layer, anchored on every edge: the whole screen.
    const auto surface = w.id();
    w.send(compositor, 0, ww::u32(surface));
    const auto layer = w.id();
    w.send(layers, 0, ww::u32(layer) + ww::u32(surface) + ww::u32(0) + ww::u32(0) + ww::str("xdesk-wallpaper"));
    w.send(layer, 1, ww::u32(15));                 // set_anchor: top, bottom, left, right
    w.send(layer, 2, ww::i32(-1));                 // set_exclusive_zone: under everything, moved by nothing
    w.send(layer, 0, ww::u32(0) + ww::u32(0));     // set_size: what the anchors give
    w.send(surface, 6);
    std::int32_t width = 0, height = 0;
    std::array<Buffer, 2> buffers;
    std::vector<std::uint32_t> small;
    int sw = 0, sh = 0;
    std::uint32_t frame = 0, pointer = 0;
    bool waiting_frame = false;
    double px = -1, py = -1;
    bool held = false;
    auto last = std::chrono::steady_clock::now();
    const auto make_buffers = [&] {
        for (auto& b : buffers) {
            if (b.id) w.send(b.id, 0);
            if (b.pool) w.send(b.pool, 1);
            if (b.memory) os::release(*b.memory);
            const auto size = static_cast<std::size_t>(width) * height * 4;
            b.memory = os::shared_memory(size);
            if (!b.memory) return false;
            b.pool = w.id();
            const int fds[1] = {b.memory->fd};
            w.send(shm, 0, ww::u32(b.pool) + ww::i32(static_cast<std::int32_t>(size)), fds);
            b.id = w.id();
            w.send(b.pool, 0, ww::u32(b.id) + ww::i32(0) + ww::i32(width) + ww::i32(height) + ww::i32(width * 4) + ww::u32(1));
            b.busy = false;
        }
        sw = std::max(16, width / 4);
        sh = std::max(16, height / 4);
        small.assign(static_cast<std::size_t>(sw) * sh, 0xff000000u);
        scene->resize(sw, sh);
        return true;
    };
    const auto draw_frame = [&] {
        auto* b = std::ranges::find_if(buffers, [](const Buffer& x) { return !x.busy && x.id; });
        if (b == buffers.end()) return;
        const auto now = std::chrono::steady_clock::now();
        scene->step(std::min(0.1, std::chrono::duration<double>(now - last).count()) * speed);
        last = now;
        scene->draw(small);
        upscale_(small, sw, sh, reinterpret_cast<std::uint32_t*>(b->memory->data), width, height);
        b->busy = true;
        w.send(surface, 1, ww::u32(b->id) + ww::i32(0) + ww::i32(0));
        w.send(surface, 2, ww::i32(0) + ww::i32(0) + ww::i32(width) + ww::i32(height));
        frame = w.id();
        w.send(surface, 3, ww::u32(frame));       // a frame callback: the next when this one is shown
        waiting_frame = true;
        w.send(surface, 6);
    };
    for (;;) {
        auto m = w.next(waiting_frame || paused ? 250 : static_cast<int>(frame_gap.count()));
        if (!m) return m.error() == "the desktop went" ? 0 : (std::println(std::cerr, "xdesk --wallpaper-engine: {}", m.error()), 1);
        {
            std::lock_guard l(cam_m);
            if (camera) { scene->camera((*camera)[0], (*camera)[1], (*camera)[2]); camera.reset(); }
        }
        if (!*m) {
            // Nothing came: the next frame when it is time and nothing holds it back.
            if (!waiting_frame && !paused && width > 0) draw_frame();
            continue;
        }
        const auto& msg = **m;
        if (msg.object == layer && msg.op == 0) {                         // configure: serial, width, height
            w.send(layer, 6, ww::u32(msg.u32_at(0)));
            const auto cw = msg.i32_at(4), ch = msg.i32_at(8);
            if (cw > 0 && ch > 0 && (cw != width || ch != height)) {
                width = cw;
                height = ch;
                if (!make_buffers()) return 1;
                waiting_frame = false;
            }
            draw_frame();
        } else if (msg.object == layer && msg.op == 1) {                  // closed
            return 0;
        } else if (msg.object == frame && msg.op == 0) {                  // done: the next may be drawn
            waiting_frame = false;
            const auto due = last + frame_gap;
            if (!paused && std::chrono::steady_clock::now() >= due) draw_frame();
        } else if (std::ranges::any_of(buffers, [&](const Buffer& b) { return b.id == msg.object; }) && msg.op == 0) {
            for (auto& b : buffers) if (b.id == msg.object) b.busy = false;   // released
        } else if (msg.object == seat && msg.op == 0) {                   // capabilities: a pointer
            if ((msg.u32_at(0) & 1) && !pointer) {
                pointer = w.id();
                w.send(seat, 0, ww::u32(pointer));
            }
        } else if (msg.object == pointer && (msg.op == 0 || msg.op == 2)) {    // enter, motion
            const double x = msg.fixed_at(msg.op == 0 ? 8 : 4), y = msg.fixed_at(msg.op == 0 ? 12 : 8);
            if (width > 0) { px = x * sw / width; py = y * sh / height; scene->pointer(px, py, held); }
        } else if (msg.object == pointer && msg.op == 3) {               // button: pressed or let go, where it is
            held = msg.u32_at(12) == 1;
            if (px >= 0) scene->pointer(px, py, held);
        } else if (msg.object == pointer && msg.op == 1) {               // leave
            held = false;
        }
    }
}

}  // namespace xdesk::engine
