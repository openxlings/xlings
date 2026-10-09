export module xlings.theme;

import std;

// The colour scheme, named by ROLE rather than by colour.
//
// WHY THIS IS A RENAME AND NOT A NEW ABSTRACTION
//
// A semantic layer already existed -- `ui/theme.cppm` exposed
// title/success/warning/error/hint/highlight/label/body. It was used SEVEN
// times. The colour-named accessors underneath it (`theme::cyan()`,
// `theme::dim_color()`, ...) were used ONE HUNDRED AND NINETEEN times.
//
// So 94% of the colour decisions in the tree named a colour instead of a role,
// and a theme file that remapped roles would have changed almost nothing. The
// expensive part of "support colour themes" was never the file format; it was
// that the call sites had bypassed the abstraction that makes theming possible.
//
// Classifying those 119 uses by what they were FOR produced a one-to-one match
// with the nine colours the palette already had:
//
//     dim_color    31   secondary text, paths, explanations   -> muted
//     text_color   20   body                                  -> text
//     green        16   success, installed                    -> success
//     magenta      12   command names, identifiers            -> alt
//     cyan         11   emphasis, current item, progress      -> accent
//     border_color 11   rules, panel edges                    -> border
//     red          10   errors                                -> error
//     amber         8   warnings                              -> warn
//     surface       -   backgrounds                           -> surface
//
// The palette's CARDINALITY was right all along; its VOCABULARY was wrong.
export namespace xlings::theme {

// Owned here, not borrowed from `core/palette`.
//
// palette's remaining job is deciding whether the terminal can take colour at
// all -- a different question from which colour to use, and one that needs
// `xlings.platform` to answer. Keeping them apart is what lets this package
// stay a leaf that everything (including core) can depend on.
struct Rgb { unsigned char r, g, b; };

enum class Background { Dark, Light };

// The complete, CLOSED set. A theme file naming anything else is reporting a
// typo, not extending the schema -- and opening it up (say, per-screen
// overrides) would couple every theme file to the list of screens, so adding a
// screen would mean editing every theme.
enum class Slot {
    Accent,   // emphasis, the current item, progress
    Alt,      // command names and other identifiers
    Success,
    Warn,
    Error,
    Text,     // body
    Muted,    // secondary: paths, explanations, "still on the old release"
    Border,   // rules and panel edges
    Surface,  // backgrounds
    Count_
};

inline constexpr int kSlotCount = static_cast<int>(Slot::Count_);

[[nodiscard]] std::string_view slot_name(Slot s);

// nullopt for an unknown name, so the caller can say "you probably meant ..."
// instead of silently dropping the line.
[[nodiscard]] std::optional<Slot> slot_from_name(std::string_view name);

// One theme: a full set of colours for each background. Both are always
// populated -- a theme file supplies only what it wants to change and the rest
// falls back to the built-in default, so by the time a Theme exists it is
// complete.
// How the inline picker draws itself.
//
// A theme is not only colours: a palette built for a monochrome terminal and
// one built for a projector disagree about DECORATION too, and forcing both
// through one widget means one of them is wrong. So the shape is part of the
// theme, with the same overlay rule as the slots -- unstated means inherit.
//
//   Inline  `◆` title, `▸` on the selected row, muted key hints.
//           The default: it is the visual language the rest of xlings already
//           speaks (`xlings subos`, `xlings list`, every diagnostic).
//   Plain   No glyphs, no accent colour, `>` for the selection, bold to
//           distinguish it. For `mono`, whose entire reason to exist is that
//           it does not decorate -- on a monochrome terminal an accent colour
//           and a `▸` are two things that do not survive.
//   Framed  A border drawn in `accent`, selected row inverted. For
//           `high-contrast`: in sunlight or through a projector a strong
//           boundary is the point, and the noise it adds elsewhere is what
//           that theme is willing to pay.
enum class SelectorStyle { Inline, Plain, Framed };

[[nodiscard]] constexpr std::string_view selector_style_name(SelectorStyle s) {
    switch (s) {
        case SelectorStyle::Plain:  return "plain";
        case SelectorStyle::Framed: return "framed";
        case SelectorStyle::Inline: break;
    }
    return "inline";
}

[[nodiscard]] constexpr std::optional<SelectorStyle>
selector_style_from_name(std::string_view name) {
    if (name == "inline") return SelectorStyle::Inline;
    if (name == "plain")  return SelectorStyle::Plain;
    if (name == "framed") return SelectorStyle::Framed;
    return std::nullopt;
}

struct Theme {
    std::string name { "default" };
    std::array<Rgb, kSlotCount> dark {};
    std::array<Rgb, kSlotCount> light {};
    SelectorStyle selector { SelectorStyle::Inline };
};

// Compiled into the binary. Works with no files on disk at all, which is what
// makes every theme file an OPTIONAL overlay rather than a required input.
[[nodiscard]] const Theme& builtin_default();

// Everything a theme file can go wrong with, as data. Reported, never
// swallowed: a mis-typed slot silently ignored is indistinguishable from a
// theme that simply does not do much -- this project's recurring bug shape.
struct LoadIssue {
    // Only what PARSING can go wrong with. "the file is not there" and "the
    // file could not be read" are the caller's to detect and report -- listing
    // them here would be two enumerators nothing ever produces.
    enum class Kind { BadJson, UnknownSlot, BadColor };
    Kind kind {};
    std::string detail;      // the offending key/value, when there is one
    std::string suggestion;  // nearest known slot name, for UnknownSlot
};

struct LoadResult {
    Theme theme;                      // always usable: base merged with what parsed
    std::vector<LoadIssue> issues;
};

// Parse an overlay on top of `base`. Missing slots keep the base's colour --
// which is why there is no `extends` field: "unspecified" already means
// "inherit", and a chain would only add ordering questions.
[[nodiscard]] LoadResult load_from_json(std::string_view json, const Theme& base);

// The resolved colour for a slot.
//
// The background is a PARAMETER, not something this package detects: probing
// the terminal needs `xlings.platform`, and taking that dependency would cost
// this package its leaf status for one boolean. `core/palette` owns the probe
// and passes the answer in.
[[nodiscard]] Rgb color(Slot s, Background bg);

// Install the process-wide theme.
void set_current(Theme theme);

[[nodiscard]] const Theme& current();

}  // namespace xlings::theme
