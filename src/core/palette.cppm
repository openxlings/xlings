export module xlings.core.palette;

import std;
import xlings.platform;

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
    if (std::getenv("NO_COLOR") != nullptr) return true;
    if (auto* term = std::getenv("TERM"); term != nullptr && std::string_view{term} == "dumb")
        return true;
    return false;
}

inline auto cursor_rewrite_allowed(bool stdoutTerminal, bool tuiMode,
                                   bool plainOutput) -> bool {
    return stdoutTerminal && !tuiMode && !plainOutput;
}

inline auto cursor_rewrite_allowed() -> bool {
    return cursor_rewrite_allowed(stdout_is_terminal(),
                                  platform::is_tui_mode(), opted_out_());
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

struct Rgb { unsigned char r, g, b; };

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

inline auto cyan()    -> Rgb { return pick_(dark_::cyan,    light_::cyan); }
inline auto green()   -> Rgb { return pick_(dark_::green,   light_::green); }
inline auto amber()   -> Rgb { return pick_(dark_::amber,   light_::amber); }
inline auto red()     -> Rgb { return pick_(dark_::red,     light_::red); }
inline auto magenta() -> Rgb { return pick_(dark_::magenta, light_::magenta); }
inline auto dim()     -> Rgb { return pick_(dark_::dim,     light_::dim); }
inline auto text()    -> Rgb { return pick_(dark_::text,    light_::text); }
inline auto surface() -> Rgb { return pick_(dark_::surface, light_::surface); }
inline auto border()  -> Rgb { return pick_(dark_::border,  light_::border); }

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
