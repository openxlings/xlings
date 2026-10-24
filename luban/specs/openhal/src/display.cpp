module openhal.display;

import std;

namespace openhal::display {

namespace {

std::optional<std::uint32_t> number_(std::string_view t) {
    std::uint32_t v = 0;
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc{} || end != t.data() + t.size() || t.empty()) return std::nullopt;
    return v;
}

std::vector<std::string_view> words_(std::string_view t) {
    std::vector<std::string_view> out;
    for (auto w : std::views::split(t, ' '))
        if (!std::string_view(w.begin(), w.end()).empty()) out.emplace_back(w.begin(), w.end());
    return out;
}

}  // namespace

std::optional<Kind> kind_of(std::string_view name) {
    for (const auto k : {Kind::physical, Kind::window, Kind::virtual_, Kind::remote})
        if (name_of(k) == name) return k;
    return std::nullopt;
}

std::string format_mode(const Mode& m) {
    if (m.scale == 1.0) return std::format("{}x{}", m.width, m.height);
    auto scale = std::format("{:.3f}", m.scale);
    while (scale.ends_with('0')) scale.pop_back();
    if (scale.ends_with('.')) scale.pop_back();
    return std::format("{}x{}@{}", m.width, m.height, scale);
}

std::optional<Mode> parse_mode(std::string_view text) {
    Mode m;
    const auto at = text.find('@');
    const auto size = text.substr(0, at);
    const auto x = size.find('x');
    if (x == std::string_view::npos) return std::nullopt;
    const auto w = number_(size.substr(0, x)), h = number_(size.substr(x + 1));
    if (!w || !h || *w < 64 || *h < 64 || *w > 16384 || *h > 16384) return std::nullopt;
    m.width = *w;
    m.height = *h;
    if (at != std::string_view::npos) {
        const auto s = text.substr(at + 1);
        double v = 0;
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || end != s.data() + s.size() || v < 0.5 || v > 4) return std::nullopt;
        m.scale = v;
    }
    return m;
}

std::string format_role(const Role& r) {
    switch (r.kind) {
    case Role::Kind::primary: return "primary";
    case Role::Kind::mirror: return std::format("mirror {}", r.of);
    case Role::Kind::extend: return std::format("extend {} {}", r.of, r.side);
    }
    return "primary";
}

std::optional<Role> parse_role(std::string_view text) {
    const auto w = words_(text);
    if (w.size() == 1 && w[0] == "primary") return Role{};
    if (w.size() == 2 && w[0] == "mirror")
        if (const auto n = number_(w[1])) return Role{.kind = Role::Kind::mirror, .of = *n};
    if (w.size() == 3 && w[0] == "extend" && (w[2] == "left" || w[2] == "right" || w[2] == "above" || w[2] == "below"))
        if (const auto n = number_(w[1])) return Role{.kind = Role::Kind::extend, .of = *n, .side = std::string(w[2])};
    return std::nullopt;
}

}  // namespace openhal::display
