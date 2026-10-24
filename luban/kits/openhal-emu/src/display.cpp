module openhal.emu;

import std;
import opensal;
import opensal.wire;
import openhal.display;

namespace openhal::emu {

struct VirtualDisplay::State {
    mutable std::mutex m;
    display::Mode mode;
    bool wants_primary { false };
    std::vector<std::byte> pixels;      // mode.width x mode.height, B G R A
    opensal::wire::Decoder in;
    std::uint64_t frames { 0 };
    std::optional<std::chrono::steady_clock::time_point> last;
    std::optional<opensal::DisplayRole> role;
    std::string clipboard;
    opensal::Size consumer;
    std::string error;

    void clear() {
        pixels.assign(std::size_t{mode.width} * mode.height * 4, std::byte{0});
        for (std::size_t k = 3; k < pixels.size(); k += 4) pixels[k] = std::byte{0xff};
    }
    // A damaged rectangle into the picture, clipped to it.
    void blit(const opensal::wire::FrameHeader& h, std::string_view rows) {
        const auto& r = h.rect;
        for (std::uint32_t y = 0; y < r.height; ++y) {
            const std::int64_t dy = r.y + static_cast<std::int64_t>(y);
            if (dy < 0 || dy >= mode.height) continue;
            const std::size_t from = std::size_t{y} * h.stride;
            if (from >= rows.size()) break;
            std::int64_t x0 = r.x, x1 = r.x + static_cast<std::int64_t>(r.width);
            const std::int64_t skip = x0 < 0 ? -x0 : 0;
            x0 = std::max<std::int64_t>(x0, 0);
            x1 = std::min<std::int64_t>(x1, mode.width);
            if (x1 <= x0) continue;
            const std::size_t bytes = static_cast<std::size_t>(x1 - x0) * 4;
            const std::size_t src = from + static_cast<std::size_t>(skip) * 4;
            if (src + bytes > rows.size()) continue;
            std::memcpy(pixels.data() + (static_cast<std::size_t>(dy) * mode.width + static_cast<std::size_t>(x0)) * 4,
                        rows.data() + src, bytes);
        }
    }
};

VirtualDisplay::VirtualDisplay(display::Mode mode, bool wants_primary) : s_(std::make_shared<State>()) {
    s_->mode = mode;
    s_->wants_primary = wants_primary;
    s_->clear();
}

namespace {

// "<bits> <w> <h> <scale> <paced> [primary]": its size in device pixels and
// its density; it paces (FramePresented); it asks to be the primary when it is
// to show the desktop at its size (opensal 0.7: a display that comes to a
// desktop already running).
std::string hello_(const display::Mode& mode, bool primary) {
    using I = opensal::Interface;
    const auto bits = I::Surface | I::Pointer | I::Keyboard | I::Mode | I::Density | I::Text | I::Clipboard | I::Displays;
    return opensal::wire::encode('H', std::format("{} {} {} {} 1{}", bits, mode.width, mode.height, mode.scale, primary ? " primary" : ""));
}

}  // namespace

std::string VirtualDisplay::hello() const {
    std::lock_guard l(s_->m);
    return hello_(s_->mode, s_->wants_primary);
}

std::string VirtualDisplay::feed(std::string_view bytes) {
    std::lock_guard l(s_->m);
    s_->in.feed(bytes);
    std::string out;
    for (;;) {
        auto m = s_->in.next();
        if (!m) {
            s_->error = m.error();
            return out;
        }
        if (!*m) return out;
        const auto& msg = **m;
        switch (msg.kind) {
        case 'f':
        case 'z': {
            std::optional<std::string> unpacked;
            std::string_view payload = msg.payload;
            if (msg.kind == 'z') {
                unpacked = opensal::wire::decode_compressed(payload);
                if (!unpacked) continue;
                payload = *unpacked;
            }
            if (const auto f = opensal::wire::decode_frame(payload)) {
                s_->blit(f->first, f->second);
                ++s_->frames;
                s_->last = std::chrono::steady_clock::now();
                out += opensal::wire::encode('E', opensal::wire::encode_event(opensal::FramePresented{}));
            }
            break;
        }
        case 'h':
            out += hello_(s_->mode, s_->wants_primary);
            break;
        case 's': {
            unsigned w = 0, h = 0;
            if (std::sscanf(msg.payload.c_str(), "%u %u", &w, &h) == 2) s_->consumer = {w, h};
            break;
        }
        case 'd':
            if (const auto r = opensal::wire::decode_display(msg.payload)) s_->role = *r;
            break;
        case 'b':
            s_->clipboard = msg.payload;
            break;
        default:
            break;   // a cursor, a hello, the shell's lists: nothing it keeps
        }
    }
}

std::string VirtualDisplay::capture() const {
    std::lock_guard l(s_->m);
    return png(s_->pixels, s_->mode.width, s_->mode.height, s_->mode.width * 4);
}

std::string VirtualDisplay::set_mode(display::Mode mode) {
    std::lock_guard l(s_->m);
    s_->mode = mode;
    s_->clear();
    std::vector<opensal::Event> e{opensal::Resized{{mode.width, mode.height}}, opensal::DensityChanged{mode.scale}};
    std::string out;
    for (const auto& ev : e) out += opensal::wire::encode('E', opensal::wire::encode_event(ev));
    return out;
}

std::string VirtualDisplay::encode(std::span<const opensal::Event> events) {
    std::string out;
    for (const auto& e : events) out += opensal::wire::encode('E', opensal::wire::encode_event(e));
    return out;
}

display::Mode VirtualDisplay::mode() const { std::lock_guard l(s_->m); return s_->mode; }
std::uint64_t VirtualDisplay::frames() const { std::lock_guard l(s_->m); return s_->frames; }
std::optional<std::chrono::steady_clock::time_point> VirtualDisplay::last_frame() const { std::lock_guard l(s_->m); return s_->last; }
std::optional<opensal::DisplayRole> VirtualDisplay::role() const { std::lock_guard l(s_->m); return s_->role; }
std::string VirtualDisplay::clipboard() const { std::lock_guard l(s_->m); return s_->clipboard; }
opensal::Size VirtualDisplay::consumer_size() const { std::lock_guard l(s_->m); return s_->consumer; }
std::string VirtualDisplay::error() const { std::lock_guard l(s_->m); return s_->error; }

}  // namespace openhal::emu
