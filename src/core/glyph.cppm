export module xlings.core.glyph;

import std;

// The glyph table. This is the ONLY place a glyph is chosen.
//
// Every character here is a BMP-region character that ships in the default
// monospace fonts of every mainstream terminal we care about: Cascadia
// Code/Mono (Windows Terminal default), Consolas (conhost), Lucida Console
// (legacy conhost), DejaVu Sans Mono / Liberation Mono / Noto Mono (Linux),
// and SF Mono / Menlo (macOS). There is no platform-conditional ASCII
// fallback — a single glyph set is what makes Linux / macOS / Windows look
// the same, which is the whole point.
//
// Avoid: U+27D0 ⟐, U+2699 ⚙, U+27F0 ⟰ — spotty in older console fonts and
// the main offenders behind Windows mojibake. U+24D8 ⓘ is the same bet
// (Enclosed Alphanumerics); `note` uses U+2022 • instead.
//
// This table lives in core rather than in the UI layer for one reason:
// `self doctor` composes its finding labels in core and used to spell ⚠ / ⓘ
// / · / → inline. A curated set with no single home is not a curated set —
// the policy above had no force over the module that produced most of the
// glyphs on screen. `ui::theme::icon` is defined in terms of this table.
export namespace xlings::glyph {

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
inline constexpr auto warn        = "\xe2\x9a\xa0";   // ⚠ U+26A0
inline constexpr auto note        = "\xe2\x80\xa2";   // • U+2022
inline constexpr auto bullet      = "\xc2\xb7";       // · U+00B7
inline constexpr auto remedy      = "\xe2\x86\x92";   // → U+2192
inline constexpr auto ellipsis    = "\xe2\x80\xa6";   // … U+2026
inline constexpr auto divider     = "\xe2\x94\x80";   // ─ U+2500 (one unit)

// The marker that means "this is the active one". It exists so that state is
// never carried by color alone: `xlings use <t>` used to paint the active
// version green and nothing else, so NO_COLOR, a pipe, or a colorblind
// reader lost the single fact the command is asked for.
inline constexpr auto active      = "\xe2\x96\xb8";   // ▸ U+25B8

// `<glyph> <text>` — how a marked label is built. Callers never concatenate
// a glyph literal themselves.
inline auto mark(std::string_view g, std::string_view text) -> std::string {
    return std::string(g) + " " + std::string(text);
}

} // namespace xlings::glyph
