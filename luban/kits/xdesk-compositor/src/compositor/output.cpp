// The output: its size and density, the fonts at it, and each frame sent (server.cppm).
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

// ── output ───────────────────────────────────────────────────────
void Server::resize(opensal::Size want) {
    if (want.width == 0 || want.height == 0 || want == size) return;
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_custom_mode(&st, static_cast<std::int32_t>(want.width), static_cast<std::int32_t>(want.height), 0);
    if (wlr_output_commit_state(output, &st)) {
        size = want;
        wlr_scene_rect_set_size(background, logical_w(), logical_h());
        draw_launcher();
        arrange();
    } else {
        log(std::format("the output did not take {}x{}", want.width, want.height));
    }
    wlr_output_state_finish(&st);
}

// The window's density: the output's scale (clients are told it and draw
// for it), the shell drawn again at it, the layout in logical pixels.
void Server::set_scale(double want) {
    want = std::clamp(want, 0.5, 4.0);
    if (std::abs(want - scale) < 0.01) return;
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_scale(&st, static_cast<float>(want));
    if (wlr_output_commit_state(output, &st)) {
        scale = want;
        open_fonts();
        for (auto& v : views) v.drawn.clear();
        panel_drawn.clear();
        dots_drawn.clear();
        wlr_scene_rect_set_size(background, logical_w(), logical_h());
        draw_launcher();
        arrange();
    } else {
        log(std::format("the output did not take scale {}", want));
    }
    wlr_output_state_finish(&st);
}

// Text: the system's faces at this density (no fonts, no text -- the shapes still are).
void Server::open_fonts() {
    // The system's faces first, then the ones xdesk carries (share/xdesk/fonts).
    std::vector<fs::path> own;
    if (const auto exe = os::executable(); !exe.empty()) own.push_back(exe.parent_path().parent_path() / "share" / "xdesk" / "fonts");
    const auto faces = draw::system_fonts(os::environment(), own);
    ui_font.reset();
    big_font.reset();
    if (auto f = draw::Fonts::open(faces, 13, scale); f) ui_font = std::move(*f);
    else log("no fonts for text: " + f.error());
    if (auto f = draw::Fonts::open(faces, 19, scale); f) big_font = std::move(*f);
}

// The frame wlroots rendered: its damaged rectangles, as opensal frames.
// A window that paces is sent the next frame once it has shown the last
// (a second at most: a frame it lost does not stop the desktop).
void Server::render() {
    if (paced && in_flight && std::chrono::steady_clock::now() - *in_flight < std::chrono::seconds(1)) {
        wl_event_source_timer_update(pace_timer, 1000);   // held: sent anyway if no word comes
        return;
    }
    wlr_output_state st;
    wlr_output_state_init(&st);
    if (zoom_shown != 1) zoom_surfaces(zoom_shown);
    if (!wlr_scene_output_build_state(scene_output, &st, nullptr) || !st.buffer) {
        wlr_output_state_finish(&st);
        return;
    }
    void* data = nullptr;
    std::uint32_t format = 0;
    std::size_t stride = 0;
    const int bw = st.buffer->width, bh = st.buffer->height;
    if (wlr_buffer_begin_data_ptr_access(st.buffer, wlr_buffer_data_ptr_access_flag::WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                         &data, &format, &stride)) {
        const opensal::Size s{static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh)};
        if (s != told) {
            send(opensal::wire::encode('s', std::format("{} {}", s.width, s.height)));
            told = s;
        }
        std::vector<opensal::Rect> rects;
        const bool whole = !(st.committed & static_cast<std::uint32_t>(wlr_output_state_field::WLR_OUTPUT_STATE_DAMAGE));
        if (!whole) {
            int n = 0;
            const pixman_box32_t* boxes = pixman_region32_rectangles(&st.damage, &n);
            for (int k = 0; k < n; ++k)
                rects.push_back({boxes[k].x1, boxes[k].y1, static_cast<std::uint32_t>(boxes[k].x2 - boxes[k].x1),
                                 static_cast<std::uint32_t>(boxes[k].y2 - boxes[k].y1)});
        }
        if (whole || rects.size() > 8) rects = {opensal::Rect{0, 0, s.width, s.height}};
        const auto* px = static_cast<const std::uint8_t*>(data);
        for (const auto& r : rects) {
            if (r.width == 0 || r.height == 0) continue;
            std::string payload = std::format("{} {} {} {} {}\n", r.x, r.y, r.width, r.height, r.width * 4);
            const auto at = payload.size();
            payload.resize(at + std::size_t(r.width) * r.height * 4);
            for (std::uint32_t y = 0; y < r.height; ++y) {
                auto* dst = payload.data() + at + std::size_t(y) * r.width * 4;
                std::memcpy(dst, px + (std::size_t(r.y) + y) * stride + std::size_t(r.x) * 4, std::size_t(r.width) * 4);
                // XRGB's fourth byte is undefined; opensal's pixels are opaque there.
                for (std::uint32_t x = 0; x < r.width; ++x) dst[x * 4 + 3] = static_cast<char>(0xff);
            }
            if (!send(opensal::wire::encode('f', payload))) break;
            in_flight = std::chrono::steady_clock::now();
        }
        wlr_buffer_end_data_ptr_access(st.buffer);
    }
    wlr_output_commit_state(output, &st);
    wlr_output_state_finish(&st);
    // Frame callbacks are timed on the monotonic clock.
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    std::timespec now{};
    now.tv_sec = static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(t).count());
    now.tv_nsec = static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count() % 1'000'000'000);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

}  // namespace xdesk::compositor
