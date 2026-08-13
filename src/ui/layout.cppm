module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/string.hpp"
#include "ftxui/screen/terminal.hpp"

export module xlings.ui:layout;

import std;
import xlings.core.console;
import xlings.core.palette;
import :theme;

// The width and output contract for every rendered document.
//
// Before this module every renderer decided independently how wide to draw
// and what to do when the content did not fit, and no two answers agreed:
//
//   * `print_info_panel` widened the canvas past the terminal
//     (`max(term, minWidth)`), so a 60-column terminal received 92-column
//     lines and hard-wrapped every one of them. That is the "ragged output
//     in a small window" users report.
//   * Everything else drew at `Dimension::Full()` and let ftxui clip. ftxui
//     shrinks every hbox child proportionally, so it eats a package name as
//     readily as a description: `xim:binutils@2.42` rendered as
//     `xim:binutils@2`, which is not ugly — it is a wrong version number.
//   * Nothing asked whether stdout was a terminal, so a pipe received true
//     color, a CR per line, trailing padding, and a NUL byte
//     (`Screen::Print()` ends with `'\0'`).
//
// The rules here are the whole contract:
//
//   1. A canvas is never wider than the terminal. Content that does not fit
//      is elided by the *caller*, which knows which column carries the
//      identifier, not by ftxui, which does not.
//   2. Off a terminal there is no width limit: a pipe or a file gets the
//      full untruncated text. A report is not a viewport (:theme says the
//      same thing about height).
//   3. Width is measured in display columns, never in bytes.
//   4. Rendered output is plain text plus optional SGR: no NUL, no CR, no
//      trailing spaces, and no color at all when the destination is not an
//      interactive terminal or the user asked for none.
export namespace xlings::ui::layout {

// ─── Output mode ───────────────────────────────────────────

// One switch for every writer — ftxui here, raw SGR in `log::` and the
// sub-OS lines. See `xlings.core.palette`.
inline void set_plain(bool on)          { palette::set_plain(on); }
inline auto stdout_is_terminal() -> bool { return palette::stdout_is_terminal(); }
inline auto colors_enabled() -> bool     { return palette::colors_enabled(); }

// ─── Width ─────────────────────────────────────────────────

// Columns available, or nullopt when stdout is not a terminal — which means
// "do not clamp and do not pad", not "assume 80". ftxui's Terminal::Size()
// answers 80x24 off a tty, and that fallback silently truncated reports in
// CI logs and `| less`.
//
// XLINGS_TERM_WIDTH overrides the probe (an integer, or 0 for "unlimited").
// Tests use it to assert layout at a width without a pty; users get an
// escape hatch for terminals that report their size wrongly.
inline auto term_width() -> std::optional<int> {
    if (auto* v = std::getenv("XLINGS_TERM_WIDTH")) {
        int w = 0;
        auto sv = std::string_view{v};
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), w);
        if (ec == std::errc{} && ptr == sv.data() + sv.size() && w >= 0) {
            if (w == 0) return std::nullopt;
            return w;
        }
    }
    if (!stdout_is_terminal()) return std::nullopt;
    auto d = ftxui::Terminal::Size();
    if (d.dimx <= 0) return std::nullopt;
    return d.dimx;
}

// Narrowest canvas we will ever draw. Below this the output is unreadable
// whatever we do, and clamping further only costs information.
inline constexpr int kMinCanvas = 24;

// The canvas width for content whose natural width is `natural`.
inline auto fit_width(int natural) -> int {
    if (natural < 1) natural = 1;
    auto term = term_width();
    if (!term) return natural;
    return std::max(kMinCanvas, std::min(natural, *term));
}

// ─── Display width ─────────────────────────────────────────

inline auto display_width(std::string_view s) -> int {
    return ftxui::string_width(std::string{s});
}

// Widest line of a possibly multi-line string.
inline auto max_line_width(std::string_view s) -> int {
    int w = 0;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto nl = s.find('\n', start);
        auto end = (nl == std::string_view::npos) ? s.size() : nl;
        w = std::max(w, display_width(s.substr(start, end - start)));
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return w;
}

inline auto repeat(std::string_view unit, int times) -> std::string {
    std::string s;
    if (times <= 0 || unit.empty()) return s;
    s.reserve(unit.size() * static_cast<std::size_t>(times));
    for (int i = 0; i < times; ++i) s += unit;
    return s;
}

// Byte length of the UTF-8 sequence starting at `s[i]`. Tolerant of invalid
// input: an unexpected byte advances by one rather than running off the end.
namespace detail {
inline auto utf8_len_(std::string_view s, std::size_t i) -> std::size_t {
    auto c = static_cast<unsigned char>(s[i]);
    std::size_t n = 1;
    if ((c & 0x80U) == 0x00U)      n = 1;
    else if ((c & 0xE0U) == 0xC0U) n = 2;
    else if ((c & 0xF0U) == 0xE0U) n = 3;
    else if ((c & 0xF8U) == 0xF0U) n = 4;
    return std::min(n, s.size() - i);
}
}  // namespace detail

// Cut `s` to at most `max` display columns, marking the cut with an
// ellipsis. Never splits a UTF-8 sequence and never splits a double-width
// glyph in half.
//
// Callers apply this to the columns they are willing to lose (descriptions,
// paths) and never to the ones the user has to be able to copy (package
// names, versions).
inline auto truncate_to_width(std::string_view s, int max) -> std::string {
    if (max <= 0) return {};
    if (display_width(s) <= max) return std::string{s};

    constexpr std::string_view mark = theme::icon::ellipsis;
    const int mark_w = display_width(mark);
    if (max <= mark_w) {
        // No room for content plus a marker; a bare marker is still honest.
        return std::string{mark};
    }

    const int budget = max - mark_w;
    int used = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        auto n = detail::utf8_len_(s, i);
        auto g = s.substr(i, n);
        auto w = display_width(g);
        if (used + w > budget) break;
        used += w;
        i += n;
    }
    return std::string{s.substr(0, i)} + std::string{mark};
}

// Break `s` into lines of at most `width` display columns.
//
// The alternative to eliding a value is showing all of it on more than one
// line, and for the values that matter — a `self doctor` finding whose
// remedy sits at the end of a 230-column line, a payload path — that is the
// only option that keeps the output usable. Breaks on spaces; a single token
// wider than the line (a path) is split at a glyph boundary rather than
// allowed to overflow.
//
// Embedded newlines are honored: `self doctor` composes multi-line details.
inline auto wrap_to_width(std::string_view s, int width) -> std::vector<std::string> {
    if (width < 1) width = 1;
    std::vector<std::string> out;

    auto hard_split = [&](std::string_view tok) {
        std::size_t i = 0;
        while (i < tok.size()) {
            int used = 0;
            std::size_t j = i;
            while (j < tok.size()) {
                auto n = detail::utf8_len_(tok, j);
                auto w = display_width(tok.substr(j, n));
                if (used + w > width) break;
                used += w;
                j += n;
            }
            if (j == i) j = std::min(tok.size(), i + detail::utf8_len_(tok, i));
            // Prefer to break a path after a separator: a payload path split
            // mid-component reads as two unrelated strings, one split after
            // `/` still reads as a path.
            if (j < tok.size()) {
                auto slash = tok.rfind('/', j - 1);
                if (slash != std::string_view::npos && slash + 1 > i && slash + 1 < j) {
                    j = slash + 1;
                }
            }
            out.emplace_back(tok.substr(i, j - i));
            i = j;
        }
    };

    std::size_t para_start = 0;
    while (para_start <= s.size()) {
        auto nl = s.find('\n', para_start);
        auto para = s.substr(para_start,
                             (nl == std::string_view::npos ? s.size() : nl) - para_start);

        std::string line;
        std::size_t i = 0;
        while (i <= para.size()) {
            auto sp = para.find(' ', i);
            auto tok = para.substr(i, (sp == std::string_view::npos ? para.size() : sp) - i);

            if (!tok.empty()) {
                auto tw = display_width(tok);
                auto lw = display_width(line);
                if (line.empty() && tw > width) {
                    hard_split(tok);
                    if (!out.empty()) { line = out.back(); out.pop_back(); }
                } else if (line.empty()) {
                    line = std::string{tok};
                } else if (lw + 1 + tw <= width) {
                    line += ' ';
                    line += tok;
                } else {
                    out.push_back(std::move(line));
                    if (tw > width) {
                        hard_split(tok);
                        line = out.back();
                        out.pop_back();
                    } else {
                        line = std::string{tok};
                    }
                }
            }
            if (sp == std::string_view::npos) break;
            i = sp + 1;
        }
        out.push_back(std::move(line));

        if (nl == std::string_view::npos) break;
        para_start = nl + 1;
    }

    if (out.empty()) out.emplace_back();
    return out;
}

// Byte length of the UTF-8 glyph starting at `i`.
inline auto glyph_len(std::string_view s, std::size_t i) -> std::size_t {
    return i < s.size() ? detail::utf8_len_(s, i) : 0;
}

// Byte offset of the glyph boundary at or before `cols` display columns.
// Used by the progress renderer, which colors a name up to a fraction of its
// own width and used to compute that split in bytes — so a multi-byte name
// was cut mid-sequence.
inline auto split_at_width(std::string_view s, int cols) -> std::size_t {
    if (cols <= 0) return 0;
    int used = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        auto n = detail::utf8_len_(s, i);
        auto w = display_width(s.substr(i, n));
        if (used + w > cols) break;
        used += w;
        i += n;
    }
    return i;
}

// Right-pad to exactly `width` display columns. Replaces the `while
// (s.size() < N) s += ' '` idiom that was repeated in five renderers and
// mis-aligned every non-ASCII string it touched.
inline auto pad_to_width(std::string s, int width) -> std::string {
    auto w = display_width(s);
    if (w >= width) return s;
    s.append(static_cast<std::size_t>(width - w), ' ');
    return s;
}

// ─── Column planning ───────────────────────────────────────

// How a `[marker][identifier]  [description]` list lays out at the width it
// has. Every list xlings prints is this shape: search results, installed
// packages, sub-OS environments, the install plan, the `--help` command
// table.
//
// ftxui's hbox shrinks its children proportionally when they do not fit,
// which is the wrong rule for all of them: it charges the package name the
// same as the sentence describing it, so `xlings list` in a 60-column
// terminal rendered `xim:binutils@2` — a version number that looks valid,
// is wrong, and cannot be pasted back into an install command. Rows also
// shrank by different amounts depending on how long their description was,
// which is why `--help` came out with a ragged name column.
//
// The rule here: the identifier is never truncated. The description gets
// whatever is left, elided with an ellipsis, and is dropped entirely when
// what is left is too little to say anything.
struct ColumnPlan {
    int width { 0 };     // canvas width
    int nameW { 0 };     // identifier column
    int descW { 0 };     // description column; 0 means "no room, drop it"
    int lead { 0 };      // marker + name + gap
    bool stacked { false };  // identifier on its own line, description under it
};

// Under this a description says nothing an ellipsis would not.
inline constexpr int kMinDescW = 8;

inline auto plan_two_column(int markerW, int gap, int nameMax, int descMax,
                            int titleW = 0) -> ColumnPlan {
    ColumnPlan P;
    P.nameW = std::max(0, nameMax);
    P.lead = markerW + P.nameW + gap;

    int natural = std::max(P.lead + std::max(0, descMax), titleW);
    P.width = fit_width(natural);

    if (markerW + P.nameW > P.width) {
        // The identifiers alone are wider than the terminal. Give them their
        // own lines rather than cutting them: a name that does not round-trip
        // into the next command is worse than a taller list.
        P.stacked = true;
        P.descW = std::max(0, P.width - markerW - gap);
        if (P.descW < kMinDescW) P.descW = 0;
        return P;
    }

    P.descW = P.width - P.lead;
    if (P.descW < kMinDescW) P.descW = 0;
    return P;
}

// ─── Printing ──────────────────────────────────────────────

namespace detail {

// One rendered line, cleaned. ftxui pads every row out to the canvas width
// and leaves a style reset after the padding, so the trailing spaces cannot
// simply be trimmed off the end of the string — the escapes have to survive
// and the spaces have to go.
inline auto clean_line_(std::string_view line, bool keep_style) -> std::string {
    struct Seg { bool esc; std::string text; };
    std::string out;
    std::vector<Seg> pending;  // since the last visible glyph, in order

    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '\x1b') {
            std::size_t j = i + 1;
            if (j < line.size() && line[j] == '[') {
                ++j;
                while (j < line.size() && (line[j] < '@' || line[j] > '~')) ++j;
                if (j < line.size()) ++j;   // the final byte
            } else if (j < line.size()) {
                ++j;
            }
            if (keep_style) pending.push_back({true, std::string{line.substr(i, j - i)}});
            i = j;
            continue;
        }
        if (line[i] == ' ') {
            pending.push_back({false, " "});
            ++i;
            continue;
        }
        for (auto& seg : pending) out += seg.text;
        pending.clear();
        out += line[i];
        ++i;
    }
    // Trailing run: keep the styling, drop the padding.
    for (auto& seg : pending) {
        if (seg.esc) out += seg.text;
    }
    return out;
}

}  // namespace detail

// Render `doc` at exactly `width` columns into a printable string.
//
// Replaces `Screen::ToString()` + `Screen::Print()`, which pad every row to
// the canvas, join rows with "\r\n", and end with a NUL byte. All three are
// invisible on a terminal and all three corrupt a pipe.
//
// `erase_eol` is for the redraw-in-place renderers: trimming the padding
// would otherwise leave the tail of the previous, longer frame on screen, so
// those callers ask for an erase-to-end-of-line instead of the padding.
inline auto render_to_string(ftxui::Element doc, int width,
                             bool erase_eol = false) -> std::string {
    if (width < 1) width = 1;

    // Off a terminal, ftxui's own fallback (80x24) would lay the document
    // out at 80 columns even though we are about to paint it at `width`.
    if (!stdout_is_terminal()) {
        ftxui::Terminal::SetFallbackSize({width, 10000});
    }

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                        theme::fit_full_height(doc));
    ftxui::Render(screen, doc);

    const bool style = colors_enabled();
    // An erase-to-end-of-line is a cursor control, not decoration: it only
    // means anything on a terminal, and it is noise in a captured log.
    const bool eol = erase_eol && stdout_is_terminal();
    auto raw = screen.ToString();

    std::string out;
    out.reserve(raw.size());
    std::size_t start = 0;
    while (start <= raw.size()) {
        auto nl = raw.find('\n', start);
        auto end = (nl == std::string::npos) ? raw.size() : nl;
        auto line = std::string_view{raw}.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out += detail::clean_line_(line, style);
        if (eol) out += "\033[K";
        out += '\n';
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    return out;
}

// Render `doc` at exactly `width` columns and write it to stdout.
//
// One sink and one lock, like every other terminal writer -- see
// xlings.core.console. A panel printed with `std::cout` while a log line went
// out through `stdout` could arrive in the wrong order on Windows, where the
// two are separately buffered.
inline void print_doc(ftxui::Element doc, int width) {
    const auto text = render_to_string(std::move(doc), width);
    std::lock_guard guard(console::output_mutex());
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    console::note_foreign_output();
}

// Convenience for the common "build rows, print them" shape.
inline void print_rows(ftxui::Elements rows, int width) {
    print_doc(ftxui::vbox(std::move(rows)), width);
}

}  // namespace xlings::ui::layout
