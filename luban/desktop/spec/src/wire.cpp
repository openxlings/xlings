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
    }
    return std::nullopt;
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

}  // namespace opensal::wire
