// `xdesk --panel <name>` (design 2026-10-24 §9.6): a window of the desktop's
// own -- its settings -- as an ordinary client: it is moved, tiled, collapsed
// and bookmarked like any other window, and a crash of it is a window gone.
// What it shows and what is done in it are the desktop's (it draws there and
// takes its input); this process only holds the window open: a surface, an
// xdg toplevel, one buffer (xdesk.waywire).
export module xdesk.panel;

import std;
import xdesk.os;
import xdesk.waywire;

namespace os = xdesk::os;
namespace ww = xdesk::waywire;

export namespace xdesk::panel {

// Holds the window `app_id` titled `title` until it is closed; 0 then.
int run(std::string_view app_id, std::string_view title) {
    auto wire = ww::Wire::connect();
    if (!wire) { std::println(std::cerr, "xdesk --panel: {}", wire.error()); return 2; }
    auto& w = *wire;
    auto g = w.globals();
    if (!g) { std::println(std::cerr, "xdesk --panel: {}", g.error()); return 1; }
    const auto compositor = w.bind(*g, "wl_compositor", 4), shm = w.bind(*g, "wl_shm", 1), wm = w.bind(*g, "xdg_wm_base", 1);
    if (!compositor || !shm || !wm) { std::println(std::cerr, "xdesk --panel: the desktop has no xdg-shell"); return 1; }
    const auto surface = w.id();
    w.send(compositor, 0, ww::u32(surface));
    const auto xsurface = w.id();
    w.send(wm, 2, ww::u32(xsurface) + ww::u32(surface));
    const auto toplevel = w.id();
    w.send(xsurface, 1, ww::u32(toplevel));
    w.send(toplevel, 2, ww::str(title));
    w.send(toplevel, 3, ww::str(app_id));
    w.send(toplevel, 8, ww::i32(560) + ww::i32(420));       // set_min_size
    w.send(surface, 6);                                      // commit: configure me
    std::int32_t width = 640, height = 480;
    std::optional<os::SharedMemory> memory;
    std::uint32_t pool = 0, buffer = 0;
    for (;;) {
        auto m = w.next(-1);
        if (!m) return m.error() == "the desktop went" ? 0 : (std::println(std::cerr, "xdesk --panel: {}", m.error()), 1);
        if (!*m) continue;
        const auto& msg = **m;
        if (msg.object == wm && msg.op == 0) {                         // ping
            w.send(wm, 3, ww::u32(msg.u32_at(0)));
        } else if (msg.object == toplevel && msg.op == 0) {            // configure: its size
            if (msg.i32_at(0) > 0 && msg.i32_at(4) > 0) { width = msg.i32_at(0); height = msg.i32_at(4); }
        } else if (msg.object == xsurface && msg.op == 0) {            // configure done: acked, a buffer
            w.send(xsurface, 4, ww::u32(msg.u32_at(0)));
            if (buffer) w.send(buffer, 0);
            if (pool) w.send(pool, 1);
            if (memory) os::release(*memory);
            const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
            memory = os::shared_memory(size);
            if (!memory) return 1;
            // The card's colour while the desktop has drawn nothing over it.
            auto* px = reinterpret_cast<std::uint32_t*>(memory->data);
            for (std::size_t k = 0; k < size / 4; ++k) px[k] = 0xff23262e;
            pool = w.id();
            const int fds[1] = {memory->fd};
            w.send(shm, 0, ww::u32(pool) + ww::i32(static_cast<std::int32_t>(size)), fds);
            buffer = w.id();
            w.send(pool, 0, ww::u32(buffer) + ww::i32(0) + ww::i32(width) + ww::i32(height) + ww::i32(width * 4) + ww::u32(1));
            w.send(surface, 1, ww::u32(buffer) + ww::i32(0) + ww::i32(0));
            w.send(surface, 2, ww::i32(0) + ww::i32(0) + ww::i32(width) + ww::i32(height));
            w.send(surface, 6);
        } else if (msg.object == toplevel && msg.op == 1) {            // close
            return 0;
        }
    }
}

}  // namespace xdesk::panel
