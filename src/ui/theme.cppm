module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:theme;

import std;
import xlings.core.glyph;
import xlings.core.palette;

export namespace xlings::ui::theme {

using ftxui::Color;
using ftxui::Decorator;

// The palette and the background detection live in `xlings.core.palette`,
// because `log::`, the sub-OS status lines and the y/n prompt write SGR
// directly and have to draw from the same table — see the rationale there.
// This is the ftxui-facing adapter over it.
using Background = palette::Background;

inline auto current_background() -> Background { return palette::current_background(); }
inline void set_background(Background bg)      { palette::set_background(bg); }

namespace detail {
inline auto rgb_(palette::Rgb c) -> Color { return Color::RGB(c.r, c.g, c.b); }
}

// ─── Colours, BY ROLE ──────────────────────────────────────
//
// These used to be named after the colour (`cyan()`, `dim_color()`, ...) and
// were called 119 times, against 7 calls to the semantic Decorators below. A
// theme file remapping roles would therefore have changed almost nothing --
// the abstraction existed and the call sites went around it.
//
// Renaming is the whole fix: a role is themeable, a colour is not. `accent`
// means "this is the current thing", and a theme deciding accent is orange
// moves the progress bar, the active-version marker and the panel title
// together, because they were always the same idea.
inline auto accent()  -> Color { return detail::rgb_(palette::cyan()); }
inline auto alt()     -> Color { return detail::rgb_(palette::magenta()); }
inline auto success() -> Color { return detail::rgb_(palette::green()); }
inline auto warn()    -> Color { return detail::rgb_(palette::amber()); }
inline auto error()   -> Color { return detail::rgb_(palette::red()); }
inline auto text()    -> Color { return detail::rgb_(palette::text()); }
inline auto muted()   -> Color { return detail::rgb_(palette::dim()); }
inline auto border()  -> Color { return detail::rgb_(palette::border()); }
inline auto surface() -> Color { return detail::rgb_(palette::surface()); }

// ─── Decorator helpers ─────────────────────────────────────
//
// Moved under `style::` so the colour accessors above could take the plain
// role names. Seven call sites moved; the alternative was 119 awkward ones.
namespace style {
auto title()     -> Decorator;
auto success()   -> Decorator;
auto warning()   -> Decorator;
auto error()     -> Decorator;
auto hint()      -> Decorator;
auto highlight() -> Decorator;
auto label()     -> Decorator;
auto body()      -> Decorator;
}  // namespace style

// ─── Icons ─────────────────────────────────────────────────
//
// The glyph table itself lives in `xlings.core.glyph`, because `self doctor`
// composes marked labels in core and must draw from the same table — see the
// rationale there. This is the UI-facing name for it.
namespace icon {
    inline constexpr auto pending     = glyph::pending;
    inline constexpr auto downloading = glyph::downloading;
    inline constexpr auto extracting  = glyph::extracting;
    inline constexpr auto installing  = glyph::installing;
    inline constexpr auto configuring = glyph::configuring;
    inline constexpr auto done        = glyph::done;
    inline constexpr auto failed      = glyph::failed;
    inline constexpr auto info        = glyph::info;
    inline constexpr auto arrow       = glyph::arrow;
    inline constexpr auto package     = glyph::package;
    inline constexpr auto warn        = glyph::warn;
    inline constexpr auto note        = glyph::note;
    inline constexpr auto bullet      = glyph::bullet;
    inline constexpr auto remedy      = glyph::remedy;
    inline constexpr auto ellipsis    = glyph::ellipsis;
    inline constexpr auto divider     = glyph::divider;
    inline constexpr auto active      = glyph::active;
} // namespace icon

// Height for a document we intend to print in full.
//
// ftxui's Dimension::Fit(doc) takes extend_beyond_screen = false by default,
// which clamps the fitted height to the terminal's. When stdout is not a tty
// ftxui reports a default 80x24, so every row past 24 was dropped -- silently,
// with no marker. `xlings list` on a home with 246 targets printed 25 lines;
// `self doctor` printed its findings and lost the summary underneath them,
// which is the one line saying how bad it is.
//
// A report is not a viewport. Print all of it and let the terminal scroll.
// The redraw-in-place progress bars in :progress deliberately do NOT use this
// -- they overwrite a known number of lines and must stay inside the screen.
inline ftxui::Dimensions fit_full_height(ftxui::Element& doc) {
    return ftxui::Dimension::Fit(doc, /*extend_beyond_screen=*/true);
}

} // namespace xlings::ui::theme
