module xlings.theme;

import std;
import xlings.libs.json;

namespace xlings::theme {

namespace {

constexpr std::string_view kSlotNames[kSlotCount] = {
    "accent", "alt", "success", "warn", "error",
    "text", "muted", "border", "surface",
};

// The built-in default carries the values `core/palette` shipped, slot for
// slot.
//
// Deliberate and load-bearing: this change renames HOW colours are addressed,
// so the pixels must not move. Any visual difference after it lands is a bug in
// the rename -- and test_theme asserts these numbers against palette's own
// table, so the two cannot drift apart silently.
Theme make_default_() {
    Theme t;
    t.name = "default";
    using S = Slot;
    const auto set = [](auto& arr, S s, Rgb c) { arr[static_cast<int>(s)] = c; };

    set(t.dark, S::Accent,  Rgb{  34, 211, 238 });  // cyan-400
    set(t.dark, S::Alt,     Rgb{ 168,  85, 247 });  // purple-500
    set(t.dark, S::Success, Rgb{  34, 197,  94 });  // green-500
    set(t.dark, S::Warn,    Rgb{ 245, 158,  11 });  // amber-500
    set(t.dark, S::Error,   Rgb{ 239,  68,  68 });  // red-500
    set(t.dark, S::Text,    Rgb{ 248, 250, 252 });  // slate-50
    set(t.dark, S::Muted,   Rgb{ 148, 163, 184 });  // slate-400
    set(t.dark, S::Border,  Rgb{  51,  65,  85 });  // slate-700
    set(t.dark, S::Surface, Rgb{  30,  41,  59 });  // slate-800

    set(t.light, S::Accent,  Rgb{   8, 145, 178 });  // cyan-700
    set(t.light, S::Alt,     Rgb{ 126,  34, 206 });  // purple-700
    set(t.light, S::Success, Rgb{  21, 128,  61 });  // green-700
    set(t.light, S::Warn,    Rgb{ 180,  83,   9 });  // amber-700
    set(t.light, S::Error,   Rgb{ 185,  28,  28 });  // red-700
    set(t.light, S::Text,    Rgb{  15,  23,  42 });  // slate-900
    set(t.light, S::Muted,   Rgb{ 100, 116, 139 });  // slate-500
    set(t.light, S::Border,  Rgb{ 203, 213, 225 });  // slate-300
    set(t.light, S::Surface, Rgb{ 241, 245, 249 });  // slate-100
    return t;
}

Theme& current_() {
    static Theme t = make_default_();
    return t;
}

// Levenshtein, small and local. The point is only to turn "you wrote `acent`"
// into a usable message; `subos.cpp` has a fuller one for subos names, and
// pulling that in would cost this leaf package its independence for 15 lines.
std::size_t distance_(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1), next(b.size() + 1);
    std::iota(prev.begin(), prev.end(), std::size_t{0});
    for (std::size_t i = 0; i < a.size(); ++i) {
        next[0] = i + 1;
        for (std::size_t j = 0; j < b.size(); ++j) {
            next[j + 1] = std::min({ prev[j + 1] + 1, next[j] + 1,
                                     prev[j] + (a[i] == b[j] ? 0u : 1u) });
        }
        std::swap(prev, next);
    }
    return prev.back();
}

std::string nearest_slot_(std::string_view name) {
    std::string best;
    auto bestDist = std::numeric_limits<std::size_t>::max();
    for (auto known : kSlotNames) {
        if (auto d = distance_(name, known); d < bestDist) {
            bestDist = d;
            best = std::string(known);
        }
    }
    // A threshold, because a suggestion that is obviously unrelated teaches the
    // reader to stop reading suggestions. The subos "did you mean" list has no
    // threshold and therefore offers names six edits away.
    const auto limit = std::max<std::size_t>(2, name.size() / 3);
    return bestDist <= limit ? best : std::string{};
}

// "#RRGGBB" or "#RGB".
std::optional<Rgb> parse_color_(std::string_view s) {
    if (s.empty() || s.front() != '#') return std::nullopt;
    s.remove_prefix(1);
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const std::size_t w = (s.size() == 3) ? 1 : (s.size() == 6 ? 2 : 0);
    if (w == 0) return std::nullopt;
    const auto comp = [&](std::size_t i) -> int {
        if (w == 1) {
            const int v = hex(s[i]);
            return v < 0 ? -1 : v * 17;      // #abc -> #aabbcc
        }
        const int hi = hex(s[i]), lo = hex(s[i + 1]);
        return (hi < 0 || lo < 0) ? -1 : hi * 16 + lo;
    };
    const int r = comp(0), g = comp(w), b = comp(2 * w);
    if (r < 0 || g < 0 || b < 0) return std::nullopt;
    return Rgb{ static_cast<unsigned char>(r),
                static_cast<unsigned char>(g),
                static_cast<unsigned char>(b) };
}

void apply_section_(const nlohmann::json& section,
                    std::array<Rgb, kSlotCount>& into,
                    std::string_view which,
                    std::vector<LoadIssue>& issues) {
    if (!section.is_object()) return;
    for (auto it = section.begin(); it != section.end(); ++it) {
        auto slot = slot_from_name(it.key());
        if (!slot) {
            issues.push_back({ LoadIssue::Kind::UnknownSlot,
                               std::format("{}.{}", which, it.key()),
                               nearest_slot_(it.key()) });
            continue;
        }
        if (!it.value().is_string()) {
            issues.push_back({ LoadIssue::Kind::BadColor,
                               std::format("{}.{}", which, it.key()), {} });
            continue;
        }
        auto rgb = parse_color_(it.value().get<std::string>());
        if (!rgb) {
            issues.push_back({ LoadIssue::Kind::BadColor,
                               std::format("{}.{} = {}", which, it.key(),
                                           it.value().get<std::string>()),
                               {} });
            continue;
        }
        into[static_cast<int>(*slot)] = *rgb;
    }
}

}  // namespace

[[nodiscard]] std::string_view slot_name(Slot s) {
    const auto i = static_cast<int>(s);
    return (i >= 0 && i < kSlotCount) ? kSlotNames[i] : std::string_view{};
}

[[nodiscard]] std::optional<Slot> slot_from_name(std::string_view name) {
    for (int i = 0; i < kSlotCount; ++i) {
        if (kSlotNames[i] == name) return static_cast<Slot>(i);
    }
    return std::nullopt;
}

[[nodiscard]] const Theme& builtin_default() {
    static const Theme t = make_default_();
    return t;
}

[[nodiscard]] LoadResult load_from_json(std::string_view json, const Theme& base) {
    // Start from the base and overlay. This is why themes need no `extends`:
    // "not mentioned" already means "keep what was there".
    LoadResult out { .theme = base };

    auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        out.issues.push_back({ LoadIssue::Kind::BadJson, {}, {} });
        return out;
    }
    if (doc.contains("name") && doc["name"].is_string()) {
        out.theme.name = doc["name"].get<std::string>();
    }
    // dark and light fall back independently: a theme that only states `dark`
    // is saying nothing about light terminals, and inventing an answer there
    // would produce a scheme its author never looked at.
    if (doc.contains("dark"))  apply_section_(doc["dark"],  out.theme.dark,  "dark",  out.issues);
    if (doc.contains("light")) apply_section_(doc["light"], out.theme.light, "light", out.issues);
    return out;
}

[[nodiscard]] Rgb color(Slot s, Background bg) {
    const auto& t = current_();
    const auto i = static_cast<int>(s);
    return bg == Background::Light ? t.light[i] : t.dark[i];
}

void set_current(Theme theme) { current_() = std::move(theme); }

[[nodiscard]] const Theme& current() { return current_(); }

}  // namespace xlings::theme
