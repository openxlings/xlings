module openhal.emu;

import std;
import opensal;
import nlohmann.json;

namespace openhal::emu {

namespace {

using Json = nlohmann::json;

std::string lower_(std::string_view s) {
    std::string out(s);
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// X11 keysyms of the keys a chord names.
std::optional<std::uint32_t> keysym_(std::string_view name) {
    if (name.size() == 1) {
        const auto ch = static_cast<unsigned char>(name[0]);
        if (ch >= 0x20 && ch < 0x7f) return std::uint32_t{ch};
    }
    static const std::map<std::string, std::uint32_t, std::less<>> named{
        {"return", 0xff0d}, {"enter", 0xff0d}, {"escape", 0xff1b}, {"esc", 0xff1b}, {"tab", 0xff09}, {"backspace", 0xff08},
        {"delete", 0xffff}, {"space", 0x20}, {"left", 0xff51}, {"up", 0xff52}, {"right", 0xff53}, {"down", 0xff54},
        {"home", 0xff50}, {"end", 0xff57}, {"pageup", 0xff55}, {"page_up", 0xff55}, {"pagedown", 0xff56}, {"page_down", 0xff56},
        {"insert", 0xff63}, {"minus", 0x2d}, {"equal", 0x3d}, {"plus", 0x2b}, {"comma", 0x2c}, {"period", 0x2e},
        {"slash", 0x2f}, {"bracketleft", 0x5b}, {"bracketright", 0x5d},
        {"shift", 0xffe1}, {"ctrl", 0xffe3}, {"control", 0xffe3}, {"alt", 0xffe9}, {"super", 0xffeb}, {"logo", 0xffeb}};
    const auto l = lower_(name);
    if (const auto it = named.find(l); it != named.end()) return it->second;
    if (l.size() >= 2 && l.size() <= 3 && l[0] == 'f') {
        int n = 0;
        if (std::from_chars(l.data() + 1, l.data() + l.size(), n).ec == std::errc{} && n >= 1 && n <= 12)
            return static_cast<std::uint32_t>(0xffbe + n - 1);
    }
    return std::nullopt;
}

std::uint32_t mod_(std::string_view name) {
    const auto l = lower_(name);
    if (l == "shift") return opensal::Shift;
    if (l == "ctrl" || l == "control") return opensal::Ctrl;
    if (l == "alt") return opensal::Alt;
    if (l == "super" || l == "logo") return opensal::Super;
    return 0;
}

std::uint32_t mod_of_keysym_(std::uint32_t k) {
    switch (k) {
    case 0xffe1: return opensal::Shift;
    case 0xffe3: return opensal::Ctrl;
    case 0xffe9: return opensal::Alt;
    case 0xffeb: return opensal::Super;
    default: return 0;
    }
}

std::optional<opensal::Button> button_(std::string_view b) {
    if (b == "left") return opensal::Button::Left;
    if (b == "middle") return opensal::Button::Middle;
    if (b == "right") return opensal::Button::Right;
    return std::nullopt;
}

}  // namespace

std::optional<Chord> parse_chord(std::string_view text) {
    std::vector<std::string_view> parts;
    for (auto p : std::views::split(text, '+')) parts.emplace_back(p.begin(), p.end());
    if (parts.empty() || parts.back().empty()) return std::nullopt;
    Chord c;
    for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
        const auto m = mod_(parts[k]);
        if (!m) return std::nullopt;
        c.mods |= m;
    }
    const auto key = keysym_(parts.back());
    if (!key) return std::nullopt;
    c.keysym = *key;
    return c;
}

// A character as a US keyboard types it: its key's keysym, and whether Shift is
// held for it; nothing for one no key types (an input method's to give).
std::optional<std::pair<std::uint32_t, bool>> typed_(char32_t c) {
    static constexpr std::string_view kShifted = "!@#$%^&*()_+{}|:\"<>?~", kBase = "1234567890-=[]\\;',./`";
    if (c == '\n') return std::pair{0xff0du, false};
    if (c == '\t') return std::pair{0xff09u, false};
    if (c >= 'a' && c <= 'z') return std::pair{std::uint32_t(c), false};
    if (c >= 'A' && c <= 'Z') return std::pair{std::uint32_t(c - 'A' + 'a'), true};
    if (c < 0x80) {
        if (const auto k = kShifted.find(static_cast<char>(c)); k != std::string_view::npos) return std::pair{std::uint32_t(kBase[k]), true};
        if (c >= 0x20 && c < 0x7f) return std::pair{std::uint32_t(c), false};
    }
    return std::nullopt;
}

std::expected<std::vector<opensal::Event>, std::string> input_events(std::string_view steps_json, double scale) {
    auto steps = Json::parse(steps_json, nullptr, false);
    if (steps.is_object() && steps.contains("events")) steps = steps["events"];
    if (!steps.is_array()) return std::unexpected(std::string("input is an array of steps (openhal 0.3 §6.6)"));
    std::vector<opensal::Event> out;
    std::uint32_t held = 0;   // the modifiers a keydown holds, for the keys after it
    for (const auto& s : steps) {
        if (!s.is_object()) return std::unexpected("a step is an object: " + s.dump());
        if (s.contains("move")) {
            const auto& p = s["move"];
            if (!p.is_array() || p.size() != 2) return std::unexpected("move takes [x, y]");
            out.push_back(opensal::PointerMotion{p[0].get<double>() * scale, p[1].get<double>() * scale});
        } else if (s.contains("click")) {
            const auto& p = s["click"];
            if (!p.is_array() || p.size() != 2) return std::unexpected("click takes [x, y]");
            const auto b = button_(s.value("button", std::string("left")));
            if (!b) return std::unexpected("a button is left, middle or right");
            out.push_back(opensal::PointerMotion{p[0].get<double>() * scale, p[1].get<double>() * scale});
            for (int k = 0; k < std::clamp(s.value("count", 1), 1, 3); ++k) {
                out.push_back(opensal::PointerButton{*b, true, held});
                out.push_back(opensal::PointerButton{*b, false, held});
            }
        } else if (s.contains("button")) {
            const auto b = button_(s["button"].get<std::string>());
            if (!b) return std::unexpected("a button is left, middle or right");
            out.push_back(opensal::PointerButton{*b, s.value("down", true), held});
        } else if (s.contains("wheel")) {
            const auto& p = s["wheel"];
            if (!p.is_array() || p.size() != 2) return std::unexpected("wheel takes [dx, dy]");
            out.push_back(opensal::Wheel{p[0].get<double>(), p[1].get<double>()});
        } else if (s.contains("key")) {
            // A chord as a person types it: its modifiers pressed, the key
            // pressed and let go, the modifiers let go -- a desktop follows
            // the modifiers' own keys, not only the bits.
            const auto text = s["key"].get<std::string>();
            const auto c = parse_chord(text);
            if (!c) return std::unexpected("not a key: " + text);
            std::vector<std::string_view> parts;
            for (auto part : std::views::split(std::string_view(text), '+')) parts.emplace_back(part.begin(), part.end());
            std::vector<std::uint32_t> mods;
            for (std::size_t k = 0; k + 1 < parts.size(); ++k)
                if (const auto sym = keysym_(parts[k])) mods.push_back(*sym);
            std::uint32_t now = held;
            for (std::size_t k = 0; k < mods.size(); ++k) {
                out.push_back(opensal::Key{.down = true, .keysym = mods[k], .mods = now});
                now |= mod_of_keysym_(mods[k]);
            }
            out.push_back(opensal::Key{.down = true, .keysym = c->keysym, .mods = now});
            out.push_back(opensal::Key{.down = false, .keysym = c->keysym, .mods = now});
            for (std::size_t k = mods.size(); k-- > 0;) {
                now &= ~mod_of_keysym_(mods[k]);
                out.push_back(opensal::Key{.down = false, .keysym = mods[k], .mods = now});
            }
        } else if (s.contains("keydown") || s.contains("keyup")) {
            const bool down = s.contains("keydown");
            const auto name = s[down ? "keydown" : "keyup"].get<std::string>();
            const auto c = parse_chord(name);
            if (!c) return std::unexpected("not a key: " + name);
            if (const auto m = mod_(name)) held = down ? held | m : held & ~m;
            out.push_back(opensal::Key{.down = down, .keysym = c->keysym, .mods = held});
        } else if (s.contains("text")) {
            // Typed as a window types it (openhal 0.3 §6.6): what a keyboard
            // has as its keys (Shift around a shifted one), the rest as an
            // input method's text.
            const auto text = s["text"].get<std::string>();
            std::string composed;
            const auto flush = [&] { if (!composed.empty()) out.push_back(opensal::TextCommit{std::exchange(composed, {})}); };
            for (std::size_t k = 0; k < text.size();) {
                const auto lead = static_cast<unsigned char>(text[k]);
                const std::size_t n = lead < 0x80 ? 1 : lead >> 5 == 6 ? 2 : lead >> 4 == 14 ? 3 : 4;
                const auto typed = n == 1 ? typed_(static_cast<char32_t>(lead)) : std::nullopt;
                if (!typed) { composed += text.substr(k, n); k += n; continue; }
                flush();
                const std::uint32_t mods = held | (typed->second ? opensal::Shift : 0u);
                if (typed->second) out.push_back(opensal::Key{.down = true, .keysym = 0xffe1, .mods = held});
                out.push_back(opensal::Key{.down = true, .keysym = typed->first, .mods = mods});
                out.push_back(opensal::Key{.down = false, .keysym = typed->first, .mods = mods});
                if (typed->second) out.push_back(opensal::Key{.down = false, .keysym = 0xffe1, .mods = held});
                k += 1;
            }
            flush();
        } else {
            return std::unexpected("not a step: " + s.dump());
        }
    }
    return out;
}

}  // namespace openhal::emu
