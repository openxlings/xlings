module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:info_panel;

import std;
import xlings.core.palette;
import :theme;
import :layout;

export namespace xlings::ui {

// One row of `xlings search` / `xlings list`.
//
// `status` is a short qualifier the row cannot be read correctly without —
// today only `inactive`. It gets a column of its own rather than a place in
// the description because the description is the column that gets dropped
// when the terminal is narrow (plan_two_column sets descW to 0 below
// kMinDescW), and a qualifier that disappears at 80 columns is a qualifier
// that lies. Empty on every row means no column is reserved at all, so
// `search` renders exactly as it did before.
struct ListRow {
    std::string name;
    std::string desc;
    std::string status;
};

// A key-value row for info panels
//
// `is_alert` exists because the panel could previously only say
// "notable-good". `xlings subos info` now reports the graphics wiring, whose
// whole point is naming a vendor that fails to load, and rendering that in the
// green+◆ styling reserved for "this is the active one" would make a failure
// read as a success — the exact confusion this repo keeps producing. A row is
// at most one of the two; alert wins, since a row that is both notable and
// broken is a broken row.
struct InfoField {
    std::string label;
    std::string value;
    bool is_highlight { false }; // Use green+bold for value
    bool is_alert { false };     // Use amber+bold for value, warn glyph marker
};

// How a panel lays its rows out at the width it actually has.
//
// The old renderer had one shape — label padded to 14, value on the same
// line — and one response to not fitting, which was to widen the canvas past
// the terminal (`max(term, minWidth)`) and let the terminal hard-wrap every
// line. It also mis-measured that width: `render_fields_` padded labels up to
// 14 but never truncated them, while the width formula assumed 14 flat, so a
// 19-column label under-reserved by 5 and the value lost its tail with no
// marker at all. `xlings use gcc` dropped the `bin` off a toolchain path
// that way.
//
// Now the panel measures itself honestly, never exceeds the terminal, and
// when a value does not fit it *wraps* rather than eliding: for a payload
// path or a `self doctor` remedy the whole string is the point.
struct PanelLayout {
    int width { 0 };       // canvas width, never wider than the terminal
    int labelW { 0 };      // label column, in display columns
    int markerW { 0 };     // 2 when any row is highlighted, else 0
    int lead { 0 };        // columns before the value on a shared line
    int valueW { 0 };      // columns available to the value
    bool stacked { false };// label on its own line, value indented below
};

// Below this a value column carries too little to be worth the label column
// sitting next to it; the panel switches to stacked rows instead.
inline constexpr int kMinValueW = 20;
inline constexpr int kStackIndent = 6;

inline int field_value_width_(const InfoField& f) {
    return layout::max_line_width(f.value);
}

PanelLayout measure_panel_(std::string_view title,
                           std::span<const InfoField> fields,
                           std::span<const InfoField> extra);

// Render info fields into rows, wrapping values that do not fit.
void render_fields_(ftxui::Elements& rows, std::span<const InfoField> fields,
                    const PanelLayout& L);

// Print a styled info panel with a title and key-value fields,
// plus an optional second section separated by a divider.
void print_info_panel(std::string_view title,
                      std::span<const InfoField> fields,
                      std::span<const InfoField> extra = {});

// Print a styled list of items with a title.
//
// `xlings search` and `xlings list` both land here. The package name is the
// column the user acts on — it goes into the next `xlings install` — so it is
// never elided; the description is.
void print_styled_list(std::string_view title,
                       std::span<const ListRow> items,
                       bool show_marker);

// Print install plan display.
// Saves cursor position before package lines so download progress can replace them.
void print_install_plan(std::span<const std::pair<std::string, std::string>> packages);

// Print subos list
void print_subos_list(
    std::span<const std::tuple<std::string, std::string, int, bool>> entries);

void print_subos_resolved(std::string_view query, std::string_view selected);

// Subos status messages are single-line with a possibly-long path, and they
// stay plain `std::print` rather than going through a rendered document: the
// content has to survive CI / pipes / non-TTY contexts unchanged.
//
// The colors come from the shared palette, so these lines follow the
// terminal background and fall silent under NO_COLOR / a pipe like every
// other writer. They used to be dark-palette SGR literals spelled out here,
// which left them low-contrast on a light terminal and leaking escapes into
// a redirect.
namespace subos_ansi_ {
    inline std::string reset()   { return palette::off(); }
    inline std::string bold()    { return palette::strong(); }
    inline std::string cyan()    { return palette::fg(palette::cyan()); }     // switched (--global)
    inline std::string green()   { return palette::fg(palette::green()); }    // created / removed
    inline std::string gray()    { return palette::fg(palette::dim()); }      // dim / "already in"
    inline std::string magenta() { return palette::fg(palette::magenta()); }  // entering (spawn)
    inline std::string amber()   { return palette::fg(palette::amber()); }    // nesting (caution-ish)
}

void print_subos_created(const std::string& name, const std::string& dir);

// `subos new --from <base>` returns through a different function than plain
// `subos new`, and that branch emitted a DataEvent nobody rendered -- so the
// whole success path was silent. One flag apart, one branch printed a panel
// and the other said nothing at all.
void print_subos_forked(const std::string& name, const std::string& from,
                        const std::string& dir);

void print_subos_switched(const std::string& name, const std::string& dir);

void print_subos_removed(const std::string& name);

// `xlings subos use <name>` (default spawn mode): user is entering a fresh
// sub-shell where XLINGS_ACTIVE_SUBOS=<name> is set. Reuses the same
// `▸ <verb> to subos <name>  (<modifier>)` template as `switched`; the
// absence of a `[global]` tag is what tells the user this is a per-shell
// action, not a persistent one.
//
// The subos NAME is rendered in bold green (matching the prompt marker
// styling) so the user's eye lands on it; the rest of the line stays in
// the magenta accent that distinguishes "spawn" from "switched" (cyan).
void print_subos_entering(const std::string& name);

// `xlings subos use <same>` while already in <same>: nothing happens, but
// we tell the user so they know the command was received and acknowledged.
void print_subos_already_in(const std::string& name);

// `xlings subos use <other>` while in <current>: the spawn will create a
// nested sub-shell. Single-line summary; the entering line that follows
// gives the destination, no need to repeat the layer count separately.
void print_subos_nesting(const std::string& from, const std::string& to);

// Width for a short message block: the widest line it will draw, clamped to
// the terminal. These blocks are a couple of lines of prose, so there is no
// column to plan — they just must not exceed the screen.
inline int message_width_(std::span<const std::string> lines) {
    int w = 0;
    for (auto& l : lines) w = std::max(w, layout::display_width(l) + 4);
    return layout::fit_width(w);
}

// Print install summary with success/fail counts
void print_install_summary(int success, int failed);

// Format "<name>[@<version>]" for display. Centralized so the plan and
// summary lines stay aligned when version is absent.
inline std::string remove_target_label_(const std::string& name,
                                        const std::string& version) {
    if (version.empty()) return name;
    return name + "@" + version;
}

// Print remove plan (shown before the confirmation prompt)
void print_remove_plan(const std::string& subos,
                       const std::string& name,
                       const std::string& version);

// Print uninstall summary
// `detached` distinguishes the two outcomes `remove` can have. They used to
// render identically, so a run that kept the registration and the payload in
// full looked exactly like one that deleted both (#443). `pinnedBy` names the
// other subos still holding this version — that list is the remedy, not
// decoration: removing it from each in turn lets the last one delete for real.
void print_remove_summary(const std::string& subos,
                          const std::string& name,
                          const std::string& version,
                          bool detached,
                          const std::vector<std::string>& pinnedBy);

} // namespace xlings::ui
