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
                           std::span<const InfoField> extra) {
    PanelLayout L;

    auto scan = [&](std::span<const InfoField> fs) {
        for (auto& f : fs) {
            L.labelW = std::max(L.labelW, layout::display_width(f.label));
            if (f.is_highlight || f.is_alert) L.markerW = 2;
        }
    };
    scan(fields);
    scan(extra);
    L.labelW = std::max(L.labelW, 12);  // keeps short panels looking like a column

    int valueMax = 0;
    auto widest = [&](std::span<const InfoField> fs) {
        for (auto& f : fs) valueMax = std::max(valueMax, field_value_width_(f));
    };
    widest(fields);
    widest(extra);

    L.lead = 2 + L.markerW + L.labelW + 2;
    int titleW = 2 + 2 + layout::display_width(title);
    int natural = std::max({ L.lead + valueMax, titleW, 42 });

    L.width = layout::fit_width(natural);
    L.valueW = L.width - L.lead;
    L.stacked = (L.valueW < kMinValueW) || (L.labelW > L.width / 2);
    if (L.stacked) L.valueW = std::max(1, L.width - kStackIndent);
    return L;
}

// Render info fields into rows, wrapping values that do not fit.
void render_fields_(ftxui::Elements& rows, std::span<const InfoField> fields,
                    const PanelLayout& L) {
    using namespace ftxui;

    for (auto& f : fields) {
        // The active/notable marker is a glyph, not a color. `xlings use`
        // used to say "this version is the active one" in green and nothing
        // else, so NO_COLOR, a pipe, or a colorblind reader lost the one
        // fact the command exists to report.
        auto marker = L.markerW == 0
            ? text("")
            : (f.is_alert
                   ? (text(std::string(theme::icon::warn) + " ") | color(theme::amber()))
                   : f.is_highlight
                       ? (text(std::string(theme::icon::active) + " ") | color(theme::green()))
                       : text("  "));

        auto value_style = [&](Element e) {
            if (f.is_alert)     return e | color(theme::amber()) | bold;
            if (f.is_highlight) return e | color(theme::green()) | bold;
            return e | color(theme::text_color());
        };

        auto lines = layout::wrap_to_width(f.value, L.valueW);

        if (L.stacked) {
            rows.push_back(hbox({
                text("  "),
                marker,
                text(f.label) | color(theme::dim_color()),
            }));
            for (auto& line : lines) {
                if (line.empty()) continue;
                rows.push_back(hbox({
                    text(std::string(kStackIndent, ' ')),
                    value_style(text(line)),
                }));
            }
            continue;
        }

        rows.push_back(hbox({
            text("  "),
            marker,
            text(layout::pad_to_width(f.label, L.labelW)) | color(theme::dim_color()),
            text("  "),
            value_style(text(lines.front())),
        }));
        // Continuation lines sit under the value column, so the label column
        // still reads as a column.
        for (std::size_t i = 1; i < lines.size(); ++i) {
            rows.push_back(hbox({
                text(std::string(static_cast<std::size_t>(L.lead), ' ')),
                value_style(text(lines[i])),
            }));
        }
    }
}

// Print a styled info panel with a title and key-value fields,
// plus an optional second section separated by a divider.
void print_info_panel(std::string_view title,
                      std::span<const InfoField> fields,
                      std::span<const InfoField> extra = {}) {
    using namespace ftxui;

    auto L = measure_panel_(title, fields, extra);

    // The divider spans the panel. It used to be forty hardcoded box glyphs
    // whatever the panel measured, so a 230-column `self doctor` header sat
    // on a forty-column rule and read as a frame that failed to draw.
    auto divider = [&] {
        return text("  " + layout::repeat(theme::icon::divider, L.width - 4))
               | color(theme::border_color());
    };

    Elements rows;
    rows.push_back(hbox({
        text("  " + std::string(theme::icon::package) + " ") | color(theme::magenta()),
        text(std::string(title)) | theme::highlight(),
    }));
    rows.push_back(divider());

    render_fields_(rows, fields, L);

    if (!extra.empty()) {
        rows.push_back(divider());
        render_fields_(rows, extra, L);
    }

    rows.push_back(text(""));

    layout::print_rows(std::move(rows), L.width);
}

// Print a styled list of items with a title.
//
// `xlings search` and `xlings list` both land here. The package name is the
// column the user acts on — it goes into the next `xlings install` — so it is
// never elided; the description is.
void print_styled_list(std::string_view title,
                       std::span<const ListRow> items,
                       bool show_marker) {
    using namespace ftxui;

    constexpr int kMarkerW = 4;   // "  ◆ " or four spaces
    constexpr int kGap = 2;

    int nameMax = 0, descMax = 0, statusMax = 0;
    for (auto& row : items) {
        nameMax = std::max(nameMax, layout::display_width(row.name));
        descMax = std::max(descMax, layout::display_width(row.desc));
        statusMax = std::max(statusMax, layout::display_width(row.status));
    }
    // The status column is planned as part of the name block, not of the
    // description, so it is never the thing that gets dropped or truncated.
    const int statusBlock = statusMax > 0 ? kGap + statusMax : 0;
    auto P = layout::plan_two_column(kMarkerW, kGap, nameMax + statusBlock,
                                     descMax,
                                     2 + layout::display_width(title));

    Elements rows;
    if (!title.empty()) {
        rows.push_back(text("  " + std::string(title)) | bold | color(theme::text_color()));
        rows.push_back(text(""));
    }

    for (auto& row : items) {
        const auto& name = row.name;
        const auto& desc = row.desc;
        auto marker = show_marker
            ? (text("  " + std::string(theme::icon::package) + " ") | color(theme::magenta()))
            : text("    ");

        if (P.stacked) {
            // The name is wider than the terminal. It still does not get cut:
            // it wraps. A package name that does not round-trip into the next
            // `xlings install` is worse than a taller list.
            auto nameLines = layout::wrap_to_width(
                name, std::max(1, P.width - kMarkerW));
            for (std::size_t i = 0; i < nameLines.size(); ++i) {
                rows.push_back(hbox({
                    i == 0 ? marker : text(std::string(kMarkerW, ' ')),
                    text(nameLines[i]) | bold | color(theme::magenta()),
                }));
            }
            // Its own line, never elided. The stacked branch is where the
            // terminal is already too narrow for the name, which is exactly
            // when a dropped qualifier would do the most damage.
            if (!row.status.empty()) {
                rows.push_back(hbox({
                    text(std::string(kMarkerW + kGap, ' ')),
                    text(row.status) | color(theme::amber()),
                }));
            }
            if (P.descW > 0 && !desc.empty()) {
                rows.push_back(hbox({
                    text(std::string(kMarkerW + kGap, ' ')),
                    text(layout::truncate_to_width(desc, P.descW)) | color(theme::dim_color()),
                }));
            }
            continue;
        }

        Elements cells;
        cells.push_back(marker);
        cells.push_back(text(layout::pad_to_width(name, nameMax))
                        | bold | color(theme::magenta()));
        if (statusMax > 0) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(text(layout::pad_to_width(row.status, statusMax))
                            | color(theme::amber()));
        }
        if (P.descW > 0 && !desc.empty()) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(text(layout::truncate_to_width(desc, P.descW))
                            | color(theme::dim_color()));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), P.width);
}

// Print install plan display.
// Saves cursor position before package lines so download progress can replace them.
void print_install_plan(std::span<const std::pair<std::string, std::string>> packages) {
    using namespace ftxui;

    constexpr int kMarkerW = 6;   // "    ◆ "
    constexpr int kGap = 2;

    int nameMax = 0, descMax = 0;
    for (auto& [nameVer, desc] : packages) {
        nameMax = std::max(nameMax, layout::display_width(nameVer));
        descMax = std::max(descMax, layout::display_width(desc));
    }
    auto P = layout::plan_two_column(kMarkerW, kGap, nameMax, descMax);

    // Print header
    Elements header;
    header.push_back(hbox({
        text("  Packages to install (") | color(theme::text_color()),
        text(std::to_string(packages.size())) | bold | color(theme::text_color()),
        text("):") | color(theme::text_color()),
    }));
    header.push_back(text(""));
    // The blank row above is the spacing. print_rows terminates every row,
    // including that one, so a further "\n" here would double it.
    layout::print_rows(std::move(header), P.width);

    // Print package lines (will be replaced by download progress via
    // cursor-up). One row per package, always — the progress renderer counts
    // the rows it has to move back over, so this list truncates rather than
    // wraps whatever the width.
    Elements pkgRows;
    for (auto& [nameVer, desc] : packages) {
        Elements cells;
        cells.push_back(text("    " + std::string(theme::icon::package) + " ")
                        | color(theme::magenta()));
        // The one place a package name may be elided: this list is
        // overwritten row-for-row and cannot grow a second line. It is
        // elided visibly, with an ellipsis, rather than clipped at the
        // screen edge.
        auto shown = P.stacked
            ? layout::truncate_to_width(nameVer, std::max(1, P.width - kMarkerW))
            : layout::pad_to_width(nameVer, P.nameW);
        cells.push_back(text(shown) | bold | color(theme::magenta()));
        if (!P.stacked && P.descW > 0 && !desc.empty()) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(text(layout::truncate_to_width(desc, P.descW))
                            | color(theme::dim_color()));
        }
        pkgRows.push_back(hbox(std::move(cells)));
    }
    pkgRows.push_back(text(""));

    layout::print_rows(std::move(pkgRows), P.width);
}

// Print subos list
void print_subos_list(
    std::span<const std::tuple<std::string, std::string, int, bool>> entries) {
    using namespace ftxui;

    constexpr int kMarkerW = 4;
    constexpr int kGap = 2;

    auto detail_of = [](const std::string& dir, int tools) {
        return "(" + dir + "  tools: " + std::to_string(tools) + ")";
    };

    int nameMax = 0, detailMax = 0;
    for (auto& [name, dir, tools, active] : entries) {
        nameMax = std::max(nameMax, layout::display_width(name));
        detailMax = std::max(detailMax, layout::display_width(detail_of(dir, tools)));
    }
    auto P = layout::plan_two_column(kMarkerW, kGap, nameMax, detailMax);

    Elements rows;
    rows.push_back(text("  Sub-OS environments:") | bold | color(theme::text_color()));
    rows.push_back(text(""));

    for (auto& [name, dir, tools, active] : entries) {
        auto marker = active
            ? (text("  " + std::string(theme::icon::active) + " ") | color(theme::cyan()))
            : text("    ");
        auto nameEl = active
            ? (text(layout::pad_to_width(name, P.nameW)) | bold | color(theme::cyan()))
            : (text(layout::pad_to_width(name, P.nameW)) | color(theme::text_color()));

        Elements cells { marker, nameEl };
        if (P.descW > 0) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(
                text(layout::truncate_to_width(detail_of(dir, tools), P.descW))
                | color(theme::dim_color()));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), P.width);
}

void print_subos_resolved(std::string_view query, std::string_view selected) {
    std::println("{}  {} resolved subos '{}' -> {}{}{}{}",
                 palette::fg(palette::cyan()), theme::icon::arrow, query,
                 palette::strong(), selected, palette::off(), palette::off());
}

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

void print_subos_created(const std::string& name, const std::string& dir) {
    using namespace subos_ansi_;
    std::println("{}  {} subos created: {}{}{}{}", green(), theme::icon::done, bold(), name, reset(), reset());
    if (!dir.empty()) {
        std::println("{}    dir:{} {}", gray(), reset(), dir);
    }
}

void print_subos_switched(const std::string& name, const std::string& dir) {
    using namespace subos_ansi_;
    // [global] tag distinguishes the persistent "--global" action from the
    // per-shell spawn (which is the default `xlings subos use NAME`).
    if (dir.empty()) {
        std::println("{}  {} switched to subos {}{}{}{}  [global]{}",
                     cyan(), theme::icon::arrow, bold(), name, reset(), cyan(), reset());
    } else {
        std::println("{}  {} switched to subos {}{}{}{}  [global]  ({}){}",
                     cyan(), theme::icon::arrow, bold(), name, reset(), cyan(), dir, reset());
    }
}

void print_subos_removed(const std::string& name) {
    using namespace subos_ansi_;
    std::println("{}  {} subos removed: {}{}{}", green(), theme::icon::done, bold(), name, reset());
}

// `xlings subos use <name>` (default spawn mode): user is entering a fresh
// sub-shell where XLINGS_ACTIVE_SUBOS=<name> is set. Reuses the same
// `▸ <verb> to subos <name>  (<modifier>)` template as `switched`; the
// absence of a `[global]` tag is what tells the user this is a per-shell
// action, not a persistent one.
//
// The subos NAME is rendered in bold green (matching the prompt marker
// styling) so the user's eye lands on it; the rest of the line stays in
// the magenta accent that distinguishes "spawn" from "switched" (cyan).
void print_subos_entering(const std::string& name) {
    using namespace subos_ansi_;
    std::println("{}  {} entering subos {}{}{}{}{}  (exit to leave){}",
                 magenta(), theme::icon::arrow, green(), bold(), name, reset(), magenta(), reset());
}

// `xlings subos use <same>` while already in <same>: nothing happens, but
// we tell the user so they know the command was received and acknowledged.
void print_subos_already_in(const std::string& name) {
    using namespace subos_ansi_;
    std::println("{}  {} already in subos {}{}{}{}",
                 gray(), theme::icon::info, bold(), name, reset(), gray());
    std::print("{}", reset());
}

// `xlings subos use <other>` while in <current>: the spawn will create a
// nested sub-shell. Single-line summary; the entering line that follows
// gives the destination, no need to repeat the layer count separately.
void print_subos_nesting(const std::string& from, const std::string& to) {
    using namespace subos_ansi_;
    std::println("{}  {} nesting subos {}{}{}{} -> {}{}{}{}  ('exit' returns to {}{}{}{}){}",
                 amber(), theme::icon::extracting,
                 bold(), from, reset(), amber(),
                 bold(), to, reset(), amber(),
                 bold(), from, reset(), amber(), reset());
}

// Width for a short message block: the widest line it will draw, clamped to
// the terminal. These blocks are a couple of lines of prose, so there is no
// column to plan — they just must not exceed the screen.
inline int message_width_(std::span<const std::string> lines) {
    int w = 0;
    for (auto& l : lines) w = std::max(w, layout::display_width(l) + 4);
    return layout::fit_width(w);
}

// Print install summary with success/fail counts
void print_install_summary(int success, int failed) {
    using namespace ftxui;

    Elements rows;
    std::vector<std::string> measured;
    rows.push_back(text(""));

    if (success > 0) {
        auto msg = std::to_string(success) + " package(s) installed";
        measured.push_back(msg);
        rows.push_back(hbox({
            text("  " + std::string(theme::icon::done) + " ") | color(theme::green()),
            text(msg) | color(theme::green()) | bold,
        }));
    }
    if (failed > 0) {
        auto msg = std::to_string(failed) + " package(s) failed";
        measured.push_back(msg);
        rows.push_back(hbox({
            text("  " + std::string(theme::icon::failed) + " ") | color(theme::red()),
            text(msg) | color(theme::red()) | bold,
        }));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), message_width_(measured));
}

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
                       const std::string& version) {
    using namespace ftxui;

    auto label = remove_target_label_(name, version);

    Elements rows;
    rows.push_back(text(""));
    rows.push_back(hbox({
        text("  Package to remove:") | color(theme::text_color()),
    }));
    rows.push_back(text(""));
    auto suffix = "  (subos: " + subos + ")";
    rows.push_back(hbox({
        text("    " + std::string(theme::icon::package) + " ") | color(theme::magenta()),
        text(label) | bold | color(theme::magenta()),
        text(suffix) | color(theme::dim_color()),
    }));
    rows.push_back(text(""));

    std::vector<std::string> measured { "Package to remove:", label + suffix + "  " };
    layout::print_rows(std::move(rows), message_width_(measured));
}

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
                          const std::vector<std::string>& pinnedBy) {
    using namespace ftxui;

    auto label = remove_target_label_(name, version);

    auto suffix = subos.empty() ? std::string{} : "  (subos: " + subos + ")";

    Elements rows;
    std::vector<std::string> measured;
    rows.push_back(text(""));
    if (detached) {
        measured.push_back(label + " detached" + suffix);
        rows.push_back(hbox({
            text("  " + std::string(theme::icon::done) + " ") | color(theme::green()),
            text(label + " detached") | color(theme::green()) | bold,
            suffix.empty() ? text("")
                           : (text(suffix) | color(theme::dim_color())),
        }));
        // Say how many, and name them only while the line still fits.
        //
        // ftxui clips at the screen edge, and on a pipe that is 80 columns:
        // a home with 20 referencing subos rendered four names and a
        // half-word, which reads as a complete list and is not. That is the
        // same "looks finished, isn't" failure this whole change removes, so
        // the cap is applied here where the count is known rather than left
        // to the terminal. The COUNT is the load-bearing fact — it tells the
        // user this is not one more `remove` away — and it always fits.
        constexpr std::size_t kMaxNamed = 2;
        if (pinnedBy.empty()) {
            // Pinned by something the subos scan cannot name (a project-local
            // subos, say). Still say the payload stayed.
            measured.emplace_back("payload kept — another subos still uses it");
            rows.push_back(text("    payload kept — another subos still uses it")
                           | color(theme::dim_color()));
        } else {
            std::string line = std::format("    payload kept — {} other subos still use it",
                                           pinnedBy.size());
            if (pinnedBy.size() <= kMaxNamed) {
                std::string names;
                for (const auto& n : pinnedBy) {
                    if (!names.empty()) names += ", ";
                    names += n;
                }
                line = std::format("    payload kept — still used by {}", names);
            }
            measured.push_back(line);
            rows.push_back(text(line) | color(theme::dim_color()));
            rows.push_back(text("    remove it there too to delete it for good")
                           | color(theme::dim_color()));
        }
    } else {
        measured.push_back(label + " removed" + suffix);
        rows.push_back(hbox({
            text("  " + std::string(theme::icon::done) + " ") | color(theme::green()),
            text(label + " removed") | color(theme::green()) | bold,
            suffix.empty() ? text("")
                           : (text(suffix) | color(theme::dim_color())),
        }));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), message_width_(measured));
}

} // namespace xlings::ui
