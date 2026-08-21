export module xlings.core.xself.theme_resources;

import std;

// The colour themes xlings ships, written to
// $XLINGS_HOME/config/themes/ on init -- the same treatment
// `profile_resources` gives the shell profiles, and the same directory
// convention (`config/shell/` already lives next to it).
//
// WHAT IS AND IS NOT ON DISK
//
// `default` is NOT here: it is compiled into the binary
// (`theme::builtin_default()`). That is what makes every file below optional
// -- a wiped, read-only or not-yet-initialised config directory still has
// colours, and a theme file is an OVERLAY rather than a required input.
//
// OWNERSHIP
//
// This directory belongs to xlings and is overwritten when `kVersion` moves,
// exactly like `config/shell/`. A user who wants their own colours points
// `theme` at their own path -- which is why the config key holds a PATH
// REFERENCE and not a name: ownership follows where you point, not a
// directory convention, so there is no `official/` subdirectory to explain.
//
// The `_comment` field says so in the file itself, because "copy me, do not
// edit me" is not discoverable from a directory listing.
//
// THREE THEMES, NOT MORE
//
//   default (built in)  looks good
//   mono                accessible: colour-vision deficiency, monochrome
//                       terminals, and people who want it to stop shouting
//   high-contrast       hostile environments: sunlight, projectors, terminals
//                       with a bad palette
//
// Each one has to be looked at by a human across nine slots and two
// backgrounds; a fourth would be a maintenance cost, not a choice.
export namespace xlings::theme_resources {

// Bump when a shipped theme changes meaningfully. init checks the
// `"_version"` field and rewrites when it differs.
inline constexpr std::string_view kVersion = "1";

struct ShippedTheme {
    std::string_view filename;
    std::string_view content;
};

// Slots left out inherit the built-in default, so each file states only what
// it actually changes -- which is also what makes them readable as examples.
inline constexpr std::string_view kMono = R"XTHEME({
  "_comment": "shipped by xlings; copy this file and point `xlings config --theme <path>` at the copy. Edits here are overwritten on upgrade.",
  "_version": "1",
  "name": "mono",

  "dark": {
    "accent":  "#E6EDF3",
    "alt":     "#E6EDF3",
    "success": "#C9D1D9",
    "warn":    "#C9D1D9",
    "error":   "#F0F6FC",
    "text":    "#F0F6FC",
    "muted":   "#8B949E",
    "border":  "#30363D",
    "surface": "#0D1117"
  },

  "light": {
    "accent":  "#1F2328",
    "alt":     "#1F2328",
    "success": "#424A53",
    "warn":    "#424A53",
    "error":   "#0E1116",
    "text":    "#0E1116",
    "muted":   "#656D76",
    "border":  "#D0D7DE",
    "surface": "#FFFFFF"
  }
}
)XTHEME";

inline constexpr std::string_view kHighContrast = R"XTHEME({
  "_comment": "shipped by xlings; copy this file and point `xlings config --theme <path>` at the copy. Edits here are overwritten on upgrade.",
  "_version": "1",
  "name": "high-contrast",

  "dark": {
    "accent":  "#00FFFF",
    "alt":     "#FF00FF",
    "success": "#00FF00",
    "warn":    "#FFFF00",
    "error":   "#FF4444",
    "text":    "#FFFFFF",
    "muted":   "#C0C0C0",
    "border":  "#808080",
    "surface": "#000000"
  },

  "light": {
    "accent":  "#005F87",
    "alt":     "#8700AF",
    "success": "#005F00",
    "warn":    "#875F00",
    "error":   "#870000",
    "text":    "#000000",
    "muted":   "#3A3A3A",
    "border":  "#767676",
    "surface": "#FFFFFF"
  }
}
)XTHEME";

inline constexpr ShippedTheme kAll[] = {
    { "mono.json",          kMono },
    { "high-contrast.json", kHighContrast },
};

}  // namespace xlings::theme_resources
