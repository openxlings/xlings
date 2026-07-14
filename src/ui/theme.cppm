module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:theme;

import std;
import xlings.platform;

export namespace xlings::ui::theme {

using ftxui::Color;
using ftxui::Decorator;

// ─── Background detection ──────────────────────────────────
//
// xlings used near-white text by default which became invisible on a
// light terminal background. The theme now picks one of two palettes
// based on the actual terminal background, with explicit override:
//
//   1. XLINGS_THEME=dark|light  — explicit override, no querying.
//   2. XLINGS_THEME=auto / unset — try (in order):
//      a. platform::query_terminal_is_light() — POSIX OSC-11 query
//         on Linux/macOS; std::nullopt on Windows.
//      b. COLORFGBG env (rxvt-style "fg;bg" — bg 7 or 9-15 = light).
//      c. Fall back to Dark — the safe default for most modern terms.
//
// Detection runs at most once per process (cached). Cost is one
// terminal round-trip on first color access, bounded by a monotonic
// deadline (default 500 ms, XLINGS_TERM_QUERY_TIMEOUT_MS) but normally
// returning in a few ms via a DSR/CPR fence; the result is reused for
// the lifetime of the program.

enum class Background { Dark, Light };

namespace detail {

inline auto from_env_override_() -> std::optional<Background> {
    if (auto* v = std::getenv("XLINGS_THEME")) {
        std::string_view s{v};
        if (s == "dark")  return Background::Dark;
        if (s == "light") return Background::Light;
        // "auto" / unknown → fall through to detection
    }
    return std::nullopt;
}

inline auto from_colorfgbg_() -> std::optional<Background> {
    auto* v = std::getenv("COLORFGBG");
    if (!v) return std::nullopt;
    std::string_view s{v};
    auto sep = s.rfind(';');
    if (sep == std::string_view::npos) return std::nullopt;
    int bg = 0;
    for (char c : s.substr(sep + 1)) {
        if (c < '0' || c > '9') return std::nullopt;
        bg = bg * 10 + (c - '0');
        if (bg > 99) return std::nullopt;
    }
    // rxvt convention: 0..6 + 8 are dark; 7 and 9..15 are light.
    return (bg == 7 || bg >= 9) ? Background::Light : Background::Dark;
}

inline auto detect_() -> Background {
    if (auto e = from_env_override_()) return *e;
    if (auto t = platform::query_terminal_is_light()) {
        return *t ? Background::Light : Background::Dark;
    }
    if (auto c = from_colorfgbg_())    return *c;
    return Background::Dark;
}

inline auto cached_() -> Background& {
    static Background bg = detect_();
    return bg;
}

}  // namespace detail

inline auto current_background() -> Background { return detail::cached_(); }
inline void set_background(Background bg)      { detail::cached_() = bg; }

// ─── Color palette ─────────────────────────────────────────
//
// Two palettes, chosen at color-access time by the cached background.
// All public color accessors below are one-liners that pick from
// `dark_` or `light_` — no macros, no template tricks; the duplication
// is small enough that the obvious code reads better.

namespace dark_ {
    inline auto cyan()         -> Color { return Color::RGB( 34, 211, 238); }  // cyan-400
    inline auto green()        -> Color { return Color::RGB( 34, 197,  94); }  // green-500
    inline auto amber()        -> Color { return Color::RGB(245, 158,  11); }  // amber-500
    inline auto red()          -> Color { return Color::RGB(239,  68,  68); }  // red-500
    inline auto magenta()      -> Color { return Color::RGB(168,  85, 247); }  // purple-500
    inline auto dim_color()    -> Color { return Color::RGB(148, 163, 184); }  // slate-400
    inline auto text_color()   -> Color { return Color::RGB(248, 250, 252); }  // slate-50
    inline auto surface()      -> Color { return Color::RGB( 30,  41,  59); }  // slate-800
    inline auto border_color() -> Color { return Color::RGB( 51,  65,  85); }  // slate-700
}

namespace light_ {
    inline auto cyan()         -> Color { return Color::RGB(  8, 145, 178); }  // cyan-700
    inline auto green()        -> Color { return Color::RGB( 21, 128,  61); }  // green-700
    inline auto amber()        -> Color { return Color::RGB(180,  83,   9); }  // amber-700
    inline auto red()          -> Color { return Color::RGB(185,  28,  28); }  // red-700
    inline auto magenta()      -> Color { return Color::RGB(126,  34, 206); }  // purple-700
    inline auto dim_color()    -> Color { return Color::RGB(100, 116, 139); }  // slate-500
    inline auto text_color()   -> Color { return Color::RGB( 15,  23,  42); }  // slate-900
    inline auto surface()      -> Color { return Color::RGB(241, 245, 249); }  // slate-100
    inline auto border_color() -> Color { return Color::RGB(203, 213, 225); }  // slate-300
}

inline auto cyan() -> Color {
    return current_background() == Background::Light ? light_::cyan() : dark_::cyan();
}
inline auto green() -> Color {
    return current_background() == Background::Light ? light_::green() : dark_::green();
}
inline auto amber() -> Color {
    return current_background() == Background::Light ? light_::amber() : dark_::amber();
}
inline auto red() -> Color {
    return current_background() == Background::Light ? light_::red() : dark_::red();
}
inline auto magenta() -> Color {
    return current_background() == Background::Light ? light_::magenta() : dark_::magenta();
}
inline auto dim_color() -> Color {
    return current_background() == Background::Light ? light_::dim_color() : dark_::dim_color();
}
inline auto text_color() -> Color {
    return current_background() == Background::Light ? light_::text_color() : dark_::text_color();
}
inline auto surface() -> Color {
    return current_background() == Background::Light ? light_::surface() : dark_::surface();
}
inline auto border_color() -> Color {
    return current_background() == Background::Light ? light_::border_color() : dark_::border_color();
}

// ─── Decorator helpers ─────────────────────────────────────
auto title()     -> Decorator { return ftxui::bold | ftxui::color(cyan()); }
auto success()   -> Decorator { return ftxui::bold | ftxui::color(green()); }
auto warning()   -> Decorator { return ftxui::bold | ftxui::color(amber()); }
auto error()     -> Decorator { return ftxui::bold | ftxui::color(red()); }
auto hint()      -> Decorator { return ftxui::dim | ftxui::color(dim_color()); }
auto highlight() -> Decorator { return ftxui::bold | ftxui::color(magenta()); }
auto label()     -> Decorator { return ftxui::color(dim_color()); }
auto body()      -> Decorator { return ftxui::color(text_color()); }

// ─── Icons ─────────────────────────────────────────────────
//
// All glyphs are BMP-region characters that ship in the default monospace
// fonts of every mainstream terminal we care about: Cascadia Code/Mono
// (Windows Terminal default), Consolas (conhost), Lucida Console (legacy
// conhost), DejaVu Sans Mono / Liberation Mono / Noto Mono (Linux), and
// SF Mono / Menlo (macOS). No platform-conditional ASCII fallback —
// keeping a single glyph set is what makes Linux / macOS / Windows look
// the same, which is the whole point.
//
// Avoid: U+27D0 ⟐, U+2699 ⚙, U+27F0 ⟰ — these are spotty in older
// console fonts and were the main offenders behind Windows mojibake.
namespace icon {
    inline constexpr auto pending     = "\xe2\x97\x8b";   // ○ U+25CB
    inline constexpr auto downloading = "\xe2\x86\x93";   // ↓ U+2193
    inline constexpr auto extracting  = "\xe2\x96\xbe";   // ▾ U+25BE
    inline constexpr auto installing  = "\xe2\x8a\x95";   // ⊕ U+2295
    inline constexpr auto configuring = "\xe2\x8a\x95";   // ⊕ U+2295
    inline constexpr auto done        = "\xe2\x9c\x93";   // ✓ U+2713
    inline constexpr auto failed      = "\xe2\x9c\x97";   // ✗ U+2717
    inline constexpr auto info        = "\xe2\x80\xba";   // › U+203A
    inline constexpr auto arrow       = "\xe2\x96\xb8";   // ▸ U+25B8
    inline constexpr auto package     = "\xe2\x97\x86";   // ◆ U+25C6
} // namespace icon

} // namespace xlings::ui::theme
