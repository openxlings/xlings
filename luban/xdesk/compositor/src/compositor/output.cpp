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
import telemetry;
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

// A frame's cost by its parts, as telemetry (xdesk msg telemetry, the bus's
// node/<id>/telemetry): what the HUD shows of the last ten seconds, kept whole.
struct FrameMeters {
    telemetry::Histogram& frame = telemetry::metrics().histogram("xdesk.frame_ns");
    telemetry::Histogram& compose = telemetry::metrics().histogram("xdesk.frame.compose_ns");
    telemetry::Histogram& readback = telemetry::metrics().histogram("xdesk.frame.readback_ns");
    telemetry::Histogram& send = telemetry::metrics().histogram("xdesk.frame.send_ns");
    telemetry::Counter& frames = telemetry::metrics().counter("xdesk.frames");
    telemetry::Counter& whole = telemetry::metrics().counter("xdesk.frames.whole");
    telemetry::Counter& bytes = telemetry::metrics().counter("xdesk.frame.bytes");
    telemetry::Counter& rects = telemetry::metrics().counter("xdesk.frame.rects");
    telemetry::Histogram& compress = telemetry::metrics().histogram("xdesk.frame.compress_ns");
    telemetry::Counter& compressed_bytes = telemetry::metrics().counter("xdesk.frame.bytes_saved");
};

FrameMeters& meters_() {
    static FrameMeters m;
    return m;
}

}  // namespace

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
        if (hint_node->node.enabled) redraw_hint();
        if (tour_at >= 0) draw_tour();
        tell_roles();   // the mirrors: the picture's new size
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
        icons.clear();
        for (auto& v : views) v.drawn.clear();
        panel_drawn.clear();
        dots_drawn.clear();
        wlr_scene_rect_set_size(background, logical_w(), logical_h());
        draw_launcher();
        arrange();
        if (hint_node->node.enabled) redraw_hint();
        if (tour_at >= 0) draw_tour();
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

// The frame wlroots rendered: its damaged rectangles, as opensal frames, to
// every window showing it (design 2026-10-20 §11) -- rendered once. A window
// that paces is sent the next frame once it has shown the last (a second at
// most: a frame it lost does not stop the desktop); a mirror that has not is
// skipped and later sent all of it.
void Server::render() {
    auto* first = primary();
    // Not seen anywhere: no frame, and no frame done -- the clients' animations wait too.
    if (!first || std::ranges::all_of(links, &Link::hidden)) return;
    const auto now = std::chrono::steady_clock::now();
    auto waiting = [&](const Link& l) { return l.paced && l.in_flight && now - *l.in_flight < std::chrono::seconds(1); };
    if (!first->hidden && waiting(*first)) {
        wl_event_source_timer_update(pace_timer, 1000);   // held: sent anyway if no word comes
        return;
    }
    using ms = std::chrono::duration<double, std::milli>;
    FrameStat stat{.at = now};
    telemetry::Span frame_span("frame", &meters_().frame, "xdesk");
    wlr_output_state st;
    wlr_output_state_init(&st);
    zoom_surfaces();
    const auto composing = std::chrono::steady_clock::now();
    bool built = false;
    {
        telemetry::Span s("compose", nullptr, "xdesk");
        built = wlr_scene_output_build_state(scene_output, &st, nullptr);
    }
    stat.compose_ms = ms(std::chrono::steady_clock::now() - composing).count();
    if (!built || !st.buffer) {
        frame_span.discard();
        wlr_output_state_finish(&st);
        return;
    }
    const int bw = st.buffer->width, bh = st.buffer->height;
    const opensal::Size s{static_cast<std::uint32_t>(bw), static_cast<std::uint32_t>(bh)};
    std::vector<opensal::Rect> damage;
    bool whole = !(st.committed & static_cast<std::uint32_t>(wlr_output_state_field::WLR_OUTPUT_STATE_DAMAGE));
    if (!whole) {
        int n = 0;
        const pixman_box32_t* boxes = pixman_region32_rectangles(&st.damage, &n);
        for (int k = 0; k < n; ++k)
            damage.push_back({boxes[k].x1, boxes[k].y1, static_cast<std::uint32_t>(boxes[k].x2 - boxes[k].x1),
                              static_cast<std::uint32_t>(boxes[k].y2 - boxes[k].y1)});
    }
    // Many rectangles: the nearest merged until eight are left -- not the whole frame (§5.6a).
    if (whole || damage.size() > 64) damage = {opensal::Rect{0, 0, s.width, s.height}};
    else if (damage.size() > 8) {
        std::vector<layout::Rect> rs;
        for (const auto& d : damage) rs.push_back({double(d.x), double(d.y), double(d.width), double(d.height)});
        damage.clear();
        for (const auto& r : layout::merge_damage(std::move(rs), 8))
            damage.push_back({static_cast<std::int32_t>(r.x), static_cast<std::int32_t>(r.y), static_cast<std::uint32_t>(r.width),
                              static_cast<std::uint32_t>(r.height)});
    }
    const std::vector<opensal::Rect> all{opensal::Rect{0, 0, s.width, s.height}};
    // Who is sent what: all of it to a window that missed frames.
    const bool any_whole = std::ranges::any_of(links, [&](const Link& l) { return !l.gone && l.hello && !l.hidden && l.whole; });
    // The pixels: the buffer itself (software), or read back from the GPU --
    // only where they changed, and all of them for a window that needs it.
    const void* data = nullptr;
    std::uint32_t format = 0;
    std::size_t stride = 0;
    void* mapped = nullptr;
    bool ok = false;
    if (wlr_buffer_begin_data_ptr_access(st.buffer, wlr_buffer_data_ptr_access_flag::WLR_BUFFER_DATA_PTR_ACCESS_READ, &mapped, &format, &stride)) {
        data = mapped;
        ok = true;
    } else if (gpu) {
        const auto reading = std::chrono::steady_clock::now();
        telemetry::Span reading_span("readback", nullptr, "xdesk");
        struct Timed { FrameStat& s; std::chrono::steady_clock::time_point t; ~Timed() {
            s.readback_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); } } timed{stat, reading};
        if (wlr_texture* tex = wlr_texture_from_buffer(renderer, st.buffer)) {
            stride = static_cast<std::size_t>(bw) * 4;
            readback.resize(stride * static_cast<std::size_t>(bh));
            format = kARGB8888;
            ok = true;
            for (const auto& r : any_whole ? all : damage) {
                const wlr_texture_read_pixels_options opts{.data = readback.data(), .format = kARGB8888,
                                                           .stride = static_cast<std::uint32_t>(stride),
                                                           .dst_x = static_cast<std::uint32_t>(r.x), .dst_y = static_cast<std::uint32_t>(r.y),
                                                           .src_box = {r.x, r.y, static_cast<int>(r.width), static_cast<int>(r.height)}};
                if (!wlr_texture_read_pixels(tex, &opts)) { ok = false; break; }
            }
            wlr_texture_destroy(tex);
            data = readback.data();
        }
        if (!ok) {
            wlr_output_state_finish(&st);
            fall_back("a frame could not be read back from the GPU");
            return;
        }
    }
    if (ok) {
        const auto* px = static_cast<const std::uint8_t*>(data);
        // ARGB with an opaque background (start_): the rows are sent as they
        // are, in one message built in a buffer kept from frame to frame.
        const bool opaque = format == kARGB8888;
        auto send_rects = [&](Link& l, const std::vector<opensal::Rect>& rects) {
            for (const auto& r : rects) {
                if (r.width == 0 || r.height == 0) continue;
                const auto head = std::format("{} {} {} {} {}\n", r.x, r.y, r.width, r.height, r.width * 4);
                const std::size_t row = std::size_t(r.width) * 4, size = head.size() + row * r.height;
                frame_buf.resize(5 + size);
                frame_buf[0] = 'f';
                for (int k = 0; k < 4; ++k) frame_buf[1 + k] = static_cast<char>((size >> (8 * (3 - k))) & 0xff);
                std::memcpy(frame_buf.data() + 5, head.data(), head.size());
                auto* out = frame_buf.data() + 5 + head.size();
                for (std::uint32_t y = 0; y < r.height; ++y) {
                    auto* dst = out + std::size_t(y) * row;
                    std::memcpy(dst, px + (std::size_t(r.y) + y) * stride + std::size_t(r.x) * 4, row);
                    // An XRGB buffer's fourth byte is undefined; opensal's pixels are opaque there.
                    if (!opaque)
                        for (std::uint32_t x = 0; x < r.width; ++x) dst[x * 4 + 3] = static_cast<char>(0xff);
                }
                const auto sending = std::chrono::steady_clock::now();
                telemetry::Span sending_span("send", nullptr, "xdesk");
                // Across a narrow channel (the provider takes them): compressed where shorter (opensal 0.6).
                std::optional<std::string> packed;
                if (opensal::has(l.provided, opensal::Interface::Compressed)) {
                    telemetry::Span s("compress", &meters_().compress, "xdesk");
                    if (auto z = opensal::wire::encode_compressed(std::string_view(frame_buf).substr(5))) packed = opensal::wire::encode('z', *z);
                }
                const bool sent = os::write_all(l.out, packed ? std::string_view(*packed) : std::string_view(frame_buf));
                if (packed) meters_().compressed_bytes.add(frame_buf.size() - packed->size());
                stat.send_ms += ms(std::chrono::steady_clock::now() - sending).count();
                stat.bytes += packed ? packed->size() : frame_buf.size();
                ++stat.rects;
                if (!sent) { l.gone = true; return; }
                l.in_flight = now;
            }
        };
        for (auto& l : links) {
            if (l.gone || !l.hello) continue;
            // Not seen, or not done with the last: it gets all of it later.
            if (l.hidden || (&l != first && waiting(l))) { l.whole = true; continue; }
            if (s != l.told) {
                if (!os::write_all(l.out, opensal::wire::encode('s', std::format("{} {}", s.width, s.height)))) { l.gone = true; continue; }
                l.told = s;
            }
            const bool all_of_it = std::exchange(l.whole, false);
            stat.whole = stat.whole || all_of_it || (!damage.empty() && damage.front().width == s.width && damage.front().height == s.height);
            send_rects(l, all_of_it ? all : damage);
        }
        if (mapped) wlr_buffer_end_data_ptr_access(st.buffer);
    }
    keep_frame_stat(stat);
    wlr_output_commit_state(output, &st);
    wlr_output_state_finish(&st);
    if (std::ranges::any_of(links, &Link::gone)) wl_event_loop_add_idle(loop, [](void* d) { static_cast<Server*>(d)->drop_gone(); }, this);
    // Frame callbacks are timed on the monotonic clock.
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    std::timespec ts{};
    ts.tv_sec = static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(t).count());
    ts.tv_nsec = static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count() % 1'000'000'000);
    wlr_scene_output_send_frame_done(scene_output, &ts);
}

void Server::keep_frame_stat(FrameStat stat) {
    auto& m = meters_();
    const auto ns = [](double ms) { return static_cast<std::uint64_t>(ms * 1e6); };
    m.frames.add();
    m.compose.record(ns(stat.compose_ms));
    if (stat.readback_ms > 0) m.readback.record(ns(stat.readback_ms));
    if (stat.rects > 0) m.send.record(ns(stat.send_ms));
    m.bytes.add(stat.bytes);
    m.rects.add(static_cast<std::uint64_t>(stat.rects));
    if (stat.whole) m.whole.add();
    frame_stats.push_back(stat);
    while (!frame_stats.empty() && stat.at - frame_stats.front().at > std::chrono::seconds(10)) frame_stats.pop_front();
    if (hud && stat.at - hud_drawn > std::chrono::milliseconds(500)) draw_hud();
}

}  // namespace xdesk::compositor
