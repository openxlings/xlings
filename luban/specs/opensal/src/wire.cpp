module opensal.wire;

import std;
import opensal;

namespace opensal::wire {

namespace {

void put32_(std::string& out, std::uint32_t v) {
    for (int shift : {24, 16, 8, 0}) out.push_back(static_cast<char>((v >> shift) & 0xff));
}

std::uint32_t get32_(std::string_view s, std::size_t at) {
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v = (v << 8) | static_cast<unsigned char>(s[at + k]);
    return v;
}

std::vector<std::string_view> words_(std::string_view s) {
    std::vector<std::string_view> out;
    while (!s.empty()) {
        const auto sp = s.find(' ');
        out.push_back(s.substr(0, sp));
        if (sp == std::string_view::npos) break;
        s.remove_prefix(sp + 1);
    }
    return out;
}

template <typename T>
bool num_(std::string_view w, T& v) {
    if constexpr (std::is_floating_point_v<T>) {
        auto r = std::from_chars(w.data(), w.data() + w.size(), v);
        return r.ec == std::errc{};
    } else {
        auto r = std::from_chars(w.data(), w.data() + w.size(), v);
        return r.ec == std::errc{} && r.ptr == w.data() + w.size();
    }
}

}  // namespace

std::string encode(char kind, std::string_view payload) {
    std::string out;
    out.reserve(5 + payload.size());
    out.push_back(kind);
    put32_(out, static_cast<std::uint32_t>(payload.size()));
    out.append(payload);
    return out;
}

std::string encode_frame(const Pixels& s, Rect d) {
    // Clipped to the surface.
    const auto x0 = std::clamp<std::int64_t>(d.x, 0, s.size.width), y0 = std::clamp<std::int64_t>(d.y, 0, s.size.height);
    const auto x1 = std::clamp<std::int64_t>(std::int64_t(d.x) + d.width, 0, s.size.width);
    const auto y1 = std::clamp<std::int64_t>(std::int64_t(d.y) + d.height, 0, s.size.height);
    const auto w = static_cast<std::uint32_t>(std::max<std::int64_t>(0, x1 - x0));
    const auto h = static_cast<std::uint32_t>(std::max<std::int64_t>(0, y1 - y0));
    std::string payload = std::format("{} {} {} {} {}\n", x0, y0, w, h, w * 4);
    payload.reserve(payload.size() + std::size_t(w) * h * 4);
    for (std::uint32_t row = 0; row < h; ++row) {
        const auto at = std::size_t(y0 + row) * s.stride + std::size_t(x0) * 4;
        if (at + std::size_t(w) * 4 > s.bytes.size()) break;
        payload.append(reinterpret_cast<const char*>(s.bytes.data() + at), std::size_t(w) * 4);
    }
    return encode('f', payload);
}

std::optional<std::pair<FrameHeader, std::string_view>> decode_frame(std::string_view payload) {
    const auto nl = payload.find('\n');
    if (nl == std::string_view::npos) return std::nullopt;
    const auto w = words_(payload.substr(0, nl));
    if (w.size() != 5) return std::nullopt;
    FrameHeader h;
    if (!num_(w[0], h.rect.x) || !num_(w[1], h.rect.y) || !num_(w[2], h.rect.width) || !num_(w[3], h.rect.height)
        || !num_(w[4], h.stride))
        return std::nullopt;
    const auto rows = payload.substr(nl + 1);
    if (h.stride != h.rect.width * 4 || rows.size() != std::size_t(h.stride) * h.rect.height) return std::nullopt;
    return std::pair{h, rows};
}

std::string encode_cursor_image(const CursorImage& c) {
    std::string payload = std::format("{} {} {} {} {}\n", c.size.width, c.size.height, c.hotspot_x, c.hotspot_y,
                                      c.size.width * 4);
    payload.append(c.bytes);
    return encode('C', payload);
}

std::optional<CursorImage> decode_cursor_image(std::string_view payload) {
    const auto nl = payload.find('\n');
    if (nl == std::string_view::npos) return std::nullopt;
    const auto w = words_(payload.substr(0, nl));
    if (w.size() != 5) return std::nullopt;
    CursorImage c;
    std::uint32_t stride = 0;
    if (!num_(w[0], c.size.width) || !num_(w[1], c.size.height) || !num_(w[2], c.hotspot_x) || !num_(w[3], c.hotspot_y)
        || !num_(w[4], stride))
        return std::nullopt;
    if (c.size.width == 0 || c.size.height == 0 || c.size.width > kMaxCursorSide || c.size.height > kMaxCursorSide
        || c.hotspot_x >= c.size.width || c.hotspot_y >= c.size.height || stride != c.size.width * 4)
        return std::nullopt;
    const auto rows = payload.substr(nl + 1);
    if (rows.size() != std::size_t(stride) * c.size.height) return std::nullopt;
    c.bytes = std::string(rows);
    return c;
}

std::string encode_event(const Event& e) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Resized>) return std::format("resize {} {}", v.size.width, v.size.height);
        else if constexpr (std::is_same_v<T, DensityChanged>) return std::format("density {}", v.scale);
        else if constexpr (std::is_same_v<T, Key>) return std::format("key {} {} {} {}", v.down ? 1 : 0, v.keysym, v.mods, v.repeat ? 1 : 0);
        else if constexpr (std::is_same_v<T, TextCommit>) return "text " + v.text;
        else if constexpr (std::is_same_v<T, Preedit>) return std::format("preedit {} {}", v.cursor, v.text);
        else if constexpr (std::is_same_v<T, PointerMotion>) return std::format("motion {} {}", v.x, v.y);
        else if constexpr (std::is_same_v<T, PointerButton>) return std::format("button {} {} {}", static_cast<std::uint32_t>(v.button), v.down ? 1 : 0, v.mods);
        else if constexpr (std::is_same_v<T, Wheel>) return std::format("wheel {} {}", v.dx, v.dy);
        else if constexpr (std::is_same_v<T, Focus>) return std::format("focus {}", v.in ? 1 : 0);
        else if constexpr (std::is_same_v<T, CloseRequested>) return "close";
        else if constexpr (std::is_same_v<T, ClipboardText>) return "clipboard " + v.text;
        else if constexpr (std::is_same_v<T, Launch>) return "launch " + v.app;
        else if constexpr (std::is_same_v<T, Activate>) return std::format("activate {}", v.window);
        else if constexpr (std::is_same_v<T, PointerRelative>) return std::format("relative {} {}", v.dx, v.dy);
        else if constexpr (std::is_same_v<T, Shown>) return std::format("shown {}", v.shown ? 1 : 0);
        else return "presented";
    }, e);
}

std::optional<Event> decode_event(std::string_view p) {
    const auto sp = p.find(' ');
    const auto name = p.substr(0, sp);
    const auto rest = sp == std::string_view::npos ? std::string_view{} : p.substr(sp + 1);
    const auto w = words_(rest);
    if (name == "resize" && w.size() == 2) {
        Resized r;
        if (num_(w[0], r.size.width) && num_(w[1], r.size.height)) return r;
    } else if (name == "density" && w.size() == 1) {
        DensityChanged d;
        if (num_(w[0], d.scale)) return d;
    } else if (name == "key" && w.size() == 4) {
        Key k;
        unsigned down = 0, rep = 0;
        if (num_(w[0], down) && num_(w[1], k.keysym) && num_(w[2], k.mods) && num_(w[3], rep)) {
            k.down = down != 0;
            k.repeat = rep != 0;
            return k;
        }
    } else if (name == "text") {
        return TextCommit{std::string(rest)};
    } else if (name == "preedit") {
        const auto s2 = rest.find(' ');
        Preedit e;
        if (num_(rest.substr(0, s2), e.cursor)) {
            e.text = s2 == std::string_view::npos ? std::string{} : std::string(rest.substr(s2 + 1));
            return e;
        }
    } else if (name == "motion" && w.size() == 2) {
        PointerMotion m;
        if (num_(w[0], m.x) && num_(w[1], m.y)) return m;
    } else if (name == "button" && w.size() == 3) {
        PointerButton b;
        std::uint32_t button = 0, down = 0;
        if (num_(w[0], button) && num_(w[1], down) && num_(w[2], b.mods) && button >= 1 && button <= 3) {
            b.button = static_cast<Button>(button);
            b.down = down != 0;
            return b;
        }
    } else if (name == "wheel" && w.size() == 2) {
        Wheel v;
        if (num_(w[0], v.dx) && num_(w[1], v.dy)) return v;
    } else if (name == "focus" && w.size() == 1) {
        return Focus{w[0] == "1"};
    } else if (name == "close") {
        return CloseRequested{};
    } else if (name == "clipboard") {
        return ClipboardText{std::string(rest)};
    } else if (name == "presented") {
        return FramePresented{};
    } else if (name == "launch" && !rest.empty()) {
        return Launch{std::string(rest)};
    } else if (name == "activate" && w.size() == 1) {
        Activate a;
        if (num_(w[0], a.window)) return a;
    } else if (name == "relative" && w.size() == 2) {
        PointerRelative r;
        if (num_(w[0], r.dx) && num_(w[1], r.dy)) return r;
    } else if (name == "shown" && w.size() == 1) {
        return Shown{w[0] == "1"};
    }
    return std::nullopt;
}

namespace {

std::string clean_(std::string s) {
    for (auto& c : s)
        if (c == '\t' || c == '\n') c = ' ';
    return s;
}

// Each line's N tab-separated fields (the last takes the rest); nullopt when one is short.
template <std::size_t N>
std::optional<std::vector<std::array<std::string_view, N>>> lines_(std::string_view p) {
    std::vector<std::array<std::string_view, N>> out;
    while (!p.empty()) {
        const auto nl = p.find('\n');
        auto line = p.substr(0, nl);
        p = nl == std::string_view::npos ? std::string_view{} : p.substr(nl + 1);
        std::array<std::string_view, N> f;
        for (std::size_t k = 0; k + 1 < N; ++k) {
            const auto tab = line.find('\t');
            if (tab == std::string_view::npos) return std::nullopt;
            f[k] = line.substr(0, tab);
            line.remove_prefix(tab + 1);
        }
        f[N - 1] = line;
        out.push_back(f);
    }
    return out;
}

}  // namespace

std::string encode_applications(std::span<const Application> apps) {
    std::string out;
    for (const auto& a : apps) out += std::format("{}\t{}\n", clean_(a.id), clean_(a.name));
    return out;
}

std::optional<std::vector<Application>> decode_applications(std::string_view p) {
    auto lines = lines_<2>(p);
    if (!lines) return std::nullopt;
    std::vector<Application> out;
    for (const auto& f : *lines) {
        if (f[0].empty()) return std::nullopt;
        out.push_back({std::string(f[0]), std::string(f[1])});
    }
    return out;
}

std::string encode_windows(std::span<const Window> windows) {
    std::string out;
    for (const auto& w : windows)
        out += std::format("{}\t{}\t{}\t{}\t{}\n", w.id, w.focused ? 1 : 0, w.minimized ? 1 : 0, clean_(w.app_id), clean_(w.title));
    return out;
}

std::optional<std::vector<Window>> decode_windows(std::string_view p) {
    auto lines = lines_<5>(p);
    if (!lines) return std::nullopt;
    std::vector<Window> out;
    for (const auto& f : *lines) {
        Window w;
        if (!num_(f[0], w.id) || (f[1] != "0" && f[1] != "1") || (f[2] != "0" && f[2] != "1")) return std::nullopt;
        w.focused = f[1] == "1";
        w.minimized = f[2] == "1";
        w.app_id = f[3];
        w.title = f[4];
        out.push_back(std::move(w));
    }
    return out;
}

std::expected<std::optional<Message>, std::string> Decoder::next() {
    if (buffer_.size() < 5) return std::optional<Message>{};
    const auto length = get32_(buffer_, 1);
    if (length > kMaxMessage) return std::unexpected(std::format("a message of {} bytes: not opensal.wire", length));
    if (buffer_.size() < 5 + length) return std::optional<Message>{};
    Message m{buffer_[0], buffer_.substr(5, length)};
    buffer_.erase(0, 5 + length);
    return std::optional<Message>{std::move(m)};
}

std::string encode_display(const DisplayRole& r) {
    if (r.kind == DisplayRole::Kind::mirror) return std::format("{} mirror {} {} {}", r.number, r.of, r.size.width, r.size.height);
    return std::format("{} primary", r.number);
}

std::optional<DisplayRole> decode_display(std::string_view payload) {
    std::istringstream in{std::string(payload)};
    DisplayRole r;
    std::string kind;
    if (!(in >> r.number >> kind) || r.number == 0) return std::nullopt;
    if (kind == "primary") return r;
    if (kind != "mirror") return std::nullopt;
    r.kind = DisplayRole::Kind::mirror;
    if (!(in >> r.of >> r.size.width >> r.size.height) || r.of == 0 || r.size.width == 0 || r.size.height == 0) return std::nullopt;
    return r;
}

}  // namespace opensal::wire
