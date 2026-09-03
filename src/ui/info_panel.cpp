module;

// For `stdout`. Every `std::println` below names its stream: the stream-less
// overload makes clang instantiate `basic_format_string` with the format string
// itself as an argument, and whether it does depends on what else the
// translation unit imports -- xim/commands.cpp compiled under clang for months
// with five such calls until one unrelated `import` was added to it. Not
// something a reader of this file can predict, so it is removed rather than
// managed. `stdout` currently arrives via the ftxui headers; relying on that
// is the same class of accident.
#include <cstdio>

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;
import xlings.core.palette;
import xlings.i18n;

namespace xlings::ui {

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
                   ? (text(std::string(theme::icon::warn) + " ") | color(theme::warn()))
                   : f.is_highlight
                       ? (text(std::string(theme::icon::active) + " ") | color(theme::success()))
                       : text("  "));

        auto value_style = [&](Element e) {
            if (f.is_alert)     return e | color(theme::warn()) | bold;
            if (f.is_highlight) return e | color(theme::success()) | bold;
            return e | color(theme::text());
        };

        auto lines = layout::wrap_to_width(f.value, L.valueW);

        if (L.stacked) {
            rows.push_back(hbox({
                text("  "),
                marker,
                text(f.label) | color(theme::muted()),
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
            text(layout::pad_to_width(f.label, L.labelW)) | color(theme::muted()),
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

void print_info_panel(std::string_view title, std::span<const InfoField> fields, std::span<const InfoField> extra) {
    using namespace ftxui;

    auto L = measure_panel_(title, fields, extra);

    // The divider spans the panel. It used to be forty hardcoded box glyphs
    // whatever the panel measured, so a 230-column `self doctor` header sat
    // on a forty-column rule and read as a frame that failed to draw.
    auto divider = [&] {
        return text("  " + layout::repeat(theme::icon::divider, L.width - 4))
               | color(theme::border());
    };

    Elements rows;
    rows.push_back(hbox({
        text("  " + std::string(theme::icon::package) + " ") | color(theme::alt()),
        text(std::string(title)) | theme::style::highlight(),
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
        rows.push_back(text("  " + std::string(title)) | bold | color(theme::text()));
        rows.push_back(text(""));
    }

    for (auto& row : items) {
        const auto& name = row.name;
        const auto& desc = row.desc;
        auto marker = show_marker
            ? (text("  " + std::string(theme::icon::package) + " ") | color(theme::alt()))
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
                    text(nameLines[i]) | bold | color(theme::alt()),
                }));
            }
            // Its own line, never elided. The stacked branch is where the
            // terminal is already too narrow for the name, which is exactly
            // when a dropped qualifier would do the most damage.
            if (!row.status.empty()) {
                rows.push_back(hbox({
                    text(std::string(kMarkerW + kGap, ' ')),
                    text(row.status) | color(theme::warn()),
                }));
            }
            if (P.descW > 0 && !desc.empty()) {
                rows.push_back(hbox({
                    text(std::string(kMarkerW + kGap, ' ')),
                    text(layout::truncate_to_width(desc, P.descW)) | color(theme::muted()),
                }));
            }
            continue;
        }

        Elements cells;
        cells.push_back(marker);
        cells.push_back(text(layout::pad_to_width(name, nameMax))
                        | bold | color(theme::alt()));
        if (statusMax > 0) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(text(layout::pad_to_width(row.status, statusMax))
                            | color(theme::warn()));
        }
        if (P.descW > 0 && !desc.empty()) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(text(layout::truncate_to_width(desc, P.descW))
                            | color(theme::muted()));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), P.width);
}

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
        text("  " + std::string(i18n::tr("Packages to install")) + " (") | color(theme::text()),
        text(std::to_string(packages.size())) | bold | color(theme::text()),
        text("):") | color(theme::text()),
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
                        | color(theme::alt()));
        // The one place a package name may be elided: this list is
        // overwritten row-for-row and cannot grow a second line. It is
        // elided visibly, with an ellipsis, rather than clipped at the
        // screen edge.
        auto shown = P.stacked
            ? layout::truncate_to_width(nameVer, std::max(1, P.width - kMarkerW))
            : layout::pad_to_width(nameVer, P.nameW);
        cells.push_back(text(shown) | bold | color(theme::alt()));
        if (!P.stacked && P.descW > 0 && !desc.empty()) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(text(layout::truncate_to_width(desc, P.descW))
                            | color(theme::muted()));
        }
        pkgRows.push_back(hbox(std::move(cells)));
    }
    pkgRows.push_back(text(""));

    layout::print_rows(std::move(pkgRows), P.width);
}

void print_subos_list(
    std::span<const std::tuple<std::string, std::string, int, bool>> entries) {
    using namespace ftxui;

    constexpr int kMarkerW = 4;
    constexpr int kGap = 2;

    auto detail_of = [](const std::string& dir, int tools) {
        return "(" + dir + "  commands: " + std::to_string(tools) + ")";
    };

    int nameMax = 0, detailMax = 0;
    for (auto& [name, dir, tools, active] : entries) {
        nameMax = std::max(nameMax, layout::display_width(name));
        detailMax = std::max(detailMax, layout::display_width(detail_of(dir, tools)));
    }
    auto P = layout::plan_two_column(kMarkerW, kGap, nameMax, detailMax);

    Elements rows;
    rows.push_back(text("  " + std::string(i18n::tr("Sub-OS environments:"))) | bold | color(theme::text()));
    rows.push_back(text(""));

    for (auto& [name, dir, tools, active] : entries) {
        auto marker = active
            ? (text("  " + std::string(theme::icon::active) + " ") | color(theme::accent()))
            : text("    ");
        auto nameEl = active
            ? (text(layout::pad_to_width(name, P.nameW)) | bold | color(theme::accent()))
            : (text(layout::pad_to_width(name, P.nameW)) | color(theme::text()));

        Elements cells { marker, nameEl };
        if (P.descW > 0) {
            cells.push_back(text(std::string(kGap, ' ')));
            cells.push_back(
                text(layout::truncate_to_width(detail_of(dir, tools), P.descW))
                | color(theme::muted()));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), P.width);
}

void print_subos_resolved(std::string_view query, std::string_view selected) {
    std::println(stdout, "{}  {} resolved subos '{}' -> {}{}{}{}",
                 palette::fg(palette::cyan()), theme::icon::arrow, query,
                 palette::strong(), selected, palette::off(), palette::off());
}

void print_subos_created(const std::string& name, const std::string& dir) {
    using namespace subos_ansi_;
    std::println(stdout, "{}  {} subos created: {}{}{}{}", green(), theme::icon::done, bold(), name, reset(), reset());
    if (!dir.empty()) {
        std::println(stdout, "{}    dir:{} {}", gray(), reset(), dir);
    }
}

void print_subos_forked(const std::string& name, const std::string& from,
                        const std::string& dir) {
    using namespace subos_ansi_;
    // Says what plain `subos new` cannot: which base this one came from. That
    // is the whole reason the user passed `--from`, so leaving it out would
    // make the fixed output answer a question nobody asked.
    std::println(stdout, "{}  {} subos created: {}{}{}{}", green(), theme::icon::done,
                 bold(), name, reset(), reset());
    if (!from.empty()) std::println(stdout, "{}    from:{} {}", gray(), reset(), from);
    if (!dir.empty())  std::println(stdout, "{}    dir:{}  {}", gray(), reset(), dir);
}

void print_subos_switched(const std::string& name, const std::string& dir) {
    using namespace subos_ansi_;
    // [global] tag distinguishes the persistent "--global" action from the
    // per-shell spawn (which is the default `xlings subos use NAME`).
    if (dir.empty()) {
        std::println(stdout, "{}  {} switched to subos {}{}{}{}  [global]{}",
                     cyan(), theme::icon::arrow, bold(), name, reset(), cyan(), reset());
    } else {
        std::println(stdout, "{}  {} switched to subos {}{}{}{}  [global]  ({}){}",
                     cyan(), theme::icon::arrow, bold(), name, reset(), cyan(), dir, reset());
    }
}

void print_subos_removed(const std::string& name) {
    using namespace subos_ansi_;
    std::println(stdout, "{}  {} subos removed: {}{}{}", green(), theme::icon::done, bold(), name, reset());
}

void print_subos_entering(const std::string& name) {
    using namespace subos_ansi_;
    std::println(stdout, "{}  {} entering subos {}{}{}{}{}  (exit to leave){}",
                 magenta(), theme::icon::arrow, green(), bold(), name, reset(), magenta(), reset());
}

void print_subos_already_in(const std::string& name) {
    using namespace subos_ansi_;
    std::println(stdout, "{}  {} already in subos {}{}{}{}",
                 gray(), theme::icon::info, bold(), name, reset(), gray());
    std::print("{}", reset());
}

void print_subos_nesting(const std::string& from, const std::string& to) {
    using namespace subos_ansi_;
    std::println(stdout, "{}  {} nesting subos {}{}{}{} -> {}{}{}{}  ('exit' returns to {}{}{}{}){}",
                 amber(), theme::icon::extracting,
                 bold(), from, reset(), amber(),
                 bold(), to, reset(), amber(),
                 bold(), from, reset(), amber(), reset());
}

void print_install_summary(int success, int failed) {
    using namespace ftxui;

    Elements rows;
    std::vector<std::string> measured;
    rows.push_back(text(""));

    if (success > 0) {
        auto msg = std::to_string(success) + " package(s) installed";
        measured.push_back(msg);
        rows.push_back(hbox({
            text("  " + std::string(theme::icon::done) + " ") | color(theme::success()),
            text(msg) | color(theme::success()) | bold,
        }));
    }
    if (failed > 0) {
        auto msg = std::to_string(failed) + " package(s) failed";
        measured.push_back(msg);
        rows.push_back(hbox({
            text("  " + std::string(theme::icon::failed) + " ") | color(theme::error()),
            text(msg) | color(theme::error()) | bold,
        }));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), message_width_(measured));
}

void print_remove_plan(const std::string& subos,
                       const std::string& name,
                       const std::string& version) {
    using namespace ftxui;

    auto label = remove_target_label_(name, version);

    Elements rows;
    rows.push_back(text(""));
    rows.push_back(hbox({
        text("  " + std::string(i18n::tr("Package to remove:"))) | color(theme::text()),
    }));
    rows.push_back(text(""));
    auto suffix = "  (subos: " + subos + ")";
    rows.push_back(hbox({
        text("    " + std::string(theme::icon::package) + " ") | color(theme::alt()),
        text(label) | bold | color(theme::alt()),
        text(suffix) | color(theme::muted()),
    }));
    rows.push_back(text(""));

    std::vector<std::string> measured { "Package to remove:", label + suffix + "  " };
    layout::print_rows(std::move(rows), message_width_(measured));
}

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
            text("  " + std::string(theme::icon::done) + " ") | color(theme::success()),
            text(label + " detached") | color(theme::success()) | bold,
            suffix.empty() ? text("")
                           : (text(suffix) | color(theme::muted())),
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
                           | color(theme::muted()));
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
            rows.push_back(text(line) | color(theme::muted()));
            rows.push_back(text("    remove it there too to delete it for good")
                           | color(theme::muted()));
        }
    } else {
        measured.push_back(label + " removed" + suffix);
        rows.push_back(hbox({
            text("  " + std::string(theme::icon::done) + " ") | color(theme::success()),
            text(label + " removed") | color(theme::success()) | bold,
            suffix.empty() ? text("")
                           : (text(suffix) | color(theme::muted())),
        }));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), message_width_(measured));
}

}
