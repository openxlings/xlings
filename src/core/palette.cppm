export module xlings.core.palette;

import std;
import xlings.platform;
import xlings.theme;

// The color table and the single answer to "may we emit color at all".
//
// Both used to live in `ui::theme`, which meant they only governed the ftxui
// renderers. Everything else that writes to the terminal — `log::` with its
// `[xlings]` prefix, the sub-OS status lines, the y/n prompt — carried its
// own copy of the dark palette spelled out as SGR literals. So the
// background detection below, which exists because near-white text is
// invisible on a light terminal, did not reach the one line the user has to
// read before answering it.
//
// `ui::theme` is now an adapter over this table.
export namespace xlings::palette {

// ─── Output mode ───────────────────────────────────────────

namespace detail {
inline bool& plain_override_() {
    static bool v = false;
    return v;
}
}  // namespace detail

// Forced plain by the caller (`--agent`).
inline void set_plain(bool on) { detail::plain_override_() = on; }
inline auto plain_forced() -> bool { return detail::plain_override_(); }

inline auto stdout_is_terminal() -> bool {
    // A console on Windows, isatty on POSIX — the same probe the download
    // progress renderer already trusts before it moves the cursor.
    return platform::supports_rewrite_output();
}

// The de-facto opt-outs. xlings already honors both in the shell profiles it
// generates for the user (xself/profile_resources.cppm) — it just never
// honored them in its own output, so `NO_COLOR=1 xlings use gcc` still came
// out in true color and a redirect to a file captured every escape.
inline auto opted_out_() -> bool {
    if (detail::plain_override_()) return true;
    // Present AND non-empty, per the NO_COLOR convention. `NO_COLOR=` is how a
    // wrapper script clears an inherited value; treating the empty string as
    // an opt-out means the user cannot turn colour back on.
    if (const auto* value = std::getenv("NO_COLOR");
        value != nullptr && *value != '\0') {
        return true;
    }
    if (auto* term = std::getenv("TERM"); term != nullptr && std::string_view{term} == "dumb")
        return true;
    return false;
}

// Whether the destination can be redrawn in place. Three independent
// questions decide the shape of xlings's output and each one only looks at
// what it is about:
//
//   colours        a terminal, and the user has not opted out of colour
//   cursor rewrite a terminal, no TUI owning the screen, not forced plain
//   live progress  always -- rewritten when it can be, appended when it cannot
//
// NO_COLOR deliberately does NOT appear here. It asks for no colour, not for
// no cursor control; folding it in left `NO_COLOR=1 xlings install llvm` with
// no feedback at all for the length of the download. `--agent` (plain forced)
// does appear, because that output is parsed by a machine.
inline auto cursor_rewrite_allowed(bool stdoutTerminal, bool tuiMode,
                                   bool plainOutput) -> bool {
    return stdoutTerminal && !tuiMode && !plainOutput;
}

inline auto cursor_rewrite_allowed() -> bool {
    return cursor_rewrite_allowed(stdout_is_terminal(),
                                  platform::is_tui_mode(), plain_forced());
}

inline auto colors_enabled() -> bool {
    return !opted_out_() && stdout_is_terminal();
}

// Warnings and errors go to stderr, which is a different stream with a
// different destination: `xlings install > log` still has an interactive
// stderr, and `2>&1 | cat` has neither.
inline auto colors_enabled_err() -> bool {
    return !opted_out_() && platform::stderr_is_terminal();
}

// ─── Background detection ──────────────────────────────────
//
//   1. XLINGS_THEME=dark|light  — explicit override, no querying.
//   2. XLINGS_THEME=auto / unset — try (in order):
//      a. platform::query_terminal_is_light() — POSIX OSC-11 query
//         on Linux/macOS; std::nullopt on Windows.
//      b. COLORFGBG env (rxvt-style "fg;bg" — bg 7 or 9-15 = light).
//      c. Fall back to Dark — the safe default for most modern terms.
//
// Detection runs at most once per process (cached). Cost is one terminal
// round-trip on first color access, bounded by a monotonic deadline (default
// 500 ms, XLINGS_TERM_QUERY_TIMEOUT_MS) but normally returning in a few ms
// via a DSR/CPR fence; the result is reused for the lifetime of the program.

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

// ─── Color table ───────────────────────────────────────────

// The colour TABLE now lives in the `theme` package; palette keeps the
// question it is actually about -- can this terminal take colour, and is the
// background dark or light. Aliased rather than re-declared so the ~40
// existing `palette::Rgb` uses keep compiling and cannot drift into a second
// incompatible type.
using Rgb = theme::Rgb;

namespace dark_ {
    inline constexpr Rgb cyan    {  34, 211, 238 };  // cyan-400
    inline constexpr Rgb green   {  34, 197,  94 };  // green-500
    inline constexpr Rgb amber   { 245, 158,  11 };  // amber-500
    inline constexpr Rgb red     { 239,  68,  68 };  // red-500
    inline constexpr Rgb magenta { 168,  85, 247 };  // purple-500
    inline constexpr Rgb dim     { 148, 163, 184 };  // slate-400
    inline constexpr Rgb text    { 248, 250, 252 };  // slate-50
    inline constexpr Rgb surface {  30,  41,  59 };  // slate-800
    inline constexpr Rgb border  {  51,  65,  85 };  // slate-700
}

namespace light_ {
    inline constexpr Rgb cyan    {   8, 145, 178 };  // cyan-700
    inline constexpr Rgb green   {  21, 128,  61 };  // green-700
    inline constexpr Rgb amber   { 180,  83,   9 };  // amber-700
    inline constexpr Rgb red     { 185,  28,  28 };  // red-700
    inline constexpr Rgb magenta { 126,  34, 206 };  // purple-700
    inline constexpr Rgb dim     { 100, 116, 139 };  // slate-500
    inline constexpr Rgb text    {  15,  23,  42 };  // slate-900
    inline constexpr Rgb surface { 241, 245, 249 };  // slate-100
    inline constexpr Rgb border  { 203, 213, 225 };  // slate-300
}

inline auto pick_(const Rgb& d, const Rgb& l) -> Rgb {
    return current_background() == Background::Light ? l : d;
}

// Resolved through the active theme, not from the table above.
//
// The `dark_`/`light_` constants are kept as the reference values the built-in
// default is asserted against (test_theme), but nothing reads them at runtime
// any more -- otherwise `xlings config --theme mono` would change the ftxui
// panels and leave `log::`'s own prefixes on the old colours, which is exactly
// the kind of half-applied setting this round is about.
inline auto slot_(theme::Slot s) -> Rgb {
    return theme::color(s, current_background() == Background::Light
                              ? theme::Background::Light
                              : theme::Background::Dark);
}

inline auto cyan()    -> Rgb { return slot_(theme::Slot::Accent);  }
inline auto green()   -> Rgb { return slot_(theme::Slot::Success); }
inline auto amber()   -> Rgb { return slot_(theme::Slot::Warn);    }
inline auto red()     -> Rgb { return slot_(theme::Slot::Error);   }
inline auto magenta() -> Rgb { return slot_(theme::Slot::Alt);     }
inline auto dim()     -> Rgb { return slot_(theme::Slot::Muted);   }
inline auto text()    -> Rgb { return slot_(theme::Slot::Text);    }
inline auto surface() -> Rgb { return slot_(theme::Slot::Surface); }
inline auto border()  -> Rgb { return slot_(theme::Slot::Border);  }

// ─── SGR, for the writers that do not go through ftxui ─────

inline constexpr auto reset = "\033[0m";
inline constexpr auto bold  = "\033[1m";

// Foreground escape, or "" when color is off. Callers concatenate freely;
// paired `reset` is likewise empty, so a plain run emits no escapes at all
// rather than bare resets.
inline auto sgr_fg(Rgb c) -> std::string {
    return std::format("\033[38;2;{};{};{}m", int(c.r), int(c.g), int(c.b));
}

inline auto fg(Rgb c) -> std::string {
    if (!colors_enabled()) return {};
    return sgr_fg(c);
}

inline auto off() -> std::string {
    return colors_enabled() ? std::string{reset} : std::string{};
}

inline auto strong() -> std::string {
    return colors_enabled() ? std::string{bold} : std::string{};
}

} // namespace xlings::palette
