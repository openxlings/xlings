module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:banner;

import std;
import :theme;
import :layout;

export namespace xlings::ui {

// --- Option / Arg descriptors for subcommand help ---
struct HelpOpt {
    std::string flag;
    std::string desc;
};

struct HelpArg {
    std::string name;
    std::string desc;
    bool required { false };
};

// Help pages are the `[indent][name]  [description]` list too, and they were
// the most visible casualty of ftxui's proportional shrink: rows with a long
// description had their name column squeezed harder than rows with a short
// one, so `xlings --help` in a narrow terminal came out with a name column
// that changed width line by line and read as a rendering fault.
//
// One plan for the whole page, so every section shares a column.
constexpr int kHelpIndent = 4;
constexpr int kHelpGap = 2;

// Build a help section: heading plus name/description rows sharing `P`.
void help_section_(ftxui::Elements& rows, std::string_view heading,
                   std::span<const std::pair<std::string, std::string>> entries,
                   const layout::ColumnPlan& P, ftxui::Decorator nameStyle) {
    using namespace ftxui;
    if (entries.empty()) return;

    rows.push_back(text("  " + std::string(heading)) | bold | color(theme::text_color()));
    for (auto& [name, desc] : entries) {
        if (P.stacked) {
            for (auto& part : layout::wrap_to_width(
                     name, std::max(1, P.width - kHelpIndent))) {
                rows.push_back(text(std::string(kHelpIndent, ' ') + part) | nameStyle);
            }
            if (P.descW > 0 && !desc.empty()) {
                rows.push_back(text(std::string(kHelpIndent + kHelpGap, ' ')
                                    + layout::truncate_to_width(desc, P.descW))
                               | color(theme::text_color()));
            }
            continue;
        }
        Elements cells;
        cells.push_back(text(std::string(kHelpIndent, ' ')));
        cells.push_back(text(layout::pad_to_width(name, P.nameW)) | nameStyle);
        if (P.descW > 0 && !desc.empty()) {
            cells.push_back(text(std::string(kHelpGap, ' ')));
            cells.push_back(text(layout::truncate_to_width(desc, P.descW))
                            | color(theme::text_color()));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    rows.push_back(text(""));
}

layout::ColumnPlan plan_help_(
        std::span<const std::span<const std::pair<std::string, std::string>>> sections,
        int titleW) {
    int nameMax = 0, descMax = 0;
    for (auto& sec : sections) {
        for (auto& [name, desc] : sec) {
            nameMax = std::max(nameMax, layout::display_width(name));
            descMax = std::max(descMax, layout::display_width(desc));
        }
    }
    return layout::plan_two_column(kHelpIndent, kHelpGap, nameMax, descMax, titleW);
}

// Print TUI-styled subcommand help
void print_subcommand_help(std::string_view name,
                           std::string_view description,
                           std::span<const HelpArg> args,
                           std::span<const HelpOpt> opts,
                           std::span<const HelpOpt> subcmds = {}) {
    using namespace ftxui;

    using Pair = std::pair<std::string, std::string>;

    std::vector<Pair> subEntries;
    for (auto& s : subcmds) subEntries.emplace_back(s.flag, s.desc);

    std::vector<Pair> argEntries;
    for (auto& a : args) {
        if (a.desc.empty()) continue;
        argEntries.emplace_back("<" + a.name + ">", a.desc);
    }

    std::vector<Pair> optEntries;
    for (auto& o : opts) optEntries.emplace_back(o.flag, o.desc);

    std::string usage = "    xlings " + std::string(name);
    if (!subcmds.empty()) usage += " [SUBCOMMAND]";
    if (!opts.empty()) usage += " [OPTIONS]";
    for (auto& a : args) {
        usage += a.required ? " <" + a.name + ">" : " [" + a.name + "]";
    }

    std::span<const Pair> sections[] = { subEntries, argEntries, optEntries };
    auto P = plan_help_(sections,
                        std::max({ layout::display_width(usage) + 2,
                                   layout::display_width(description) + 4,
                                   40 }));

    Elements rows;
    rows.push_back(text(""));

    // Title
    rows.push_back(hbox({
        text("  xlings ") | color(theme::dim_color()),
        text(std::string(name)) | theme::title(),
    }));
    rows.push_back(text(""));

    // Description
    if (!description.empty()) {
        for (auto& line : layout::wrap_to_width(description, std::max(1, P.width - 2))) {
            rows.push_back(text("  " + line) | color(theme::text_color()));
        }
        rows.push_back(text(""));
    }

    // USAGE
    rows.push_back(text("  USAGE") | bold | color(theme::text_color()));
    rows.push_back(text(usage) | color(theme::dim_color()));
    rows.push_back(text(""));

    help_section_(rows, "SUBCOMMANDS", subEntries, P,
                  bold | color(theme::magenta()));
    help_section_(rows, "ARGS", argEntries, P,
                  bold | color(theme::magenta()));
    help_section_(rows, "OPTIONS", optEntries, P,
                  color(theme::dim_color()));

    layout::print_rows(std::move(rows), P.width);
}

// Print styled top-level help text
void print_help(std::string_view version) {
    using namespace ftxui;

    using Pair = std::pair<std::string, std::string>;

    struct CmdEntry { std::string name; std::string desc; };
    CmdEntry cmds[] = {
        {"install",  "Install packages (e.g. xlings install gcc@15 node)"},
        {"remove",   "Remove a package"},
        {"update",   "Update package index or a specific package"},
        {"search",   "Search for packages"},
        {"list",     "List installed packages"},
        {"info",     "Show package information"},
        {"use",      "Switch tool version"},
        {"config",   "Show or modify configuration"},
        {"subos",    "Manage sub-OS environments"},
        {"self",     "Manage xlings itself (install, update, clean)"},
        {"script",   "Run xlings scripts"},
        {"agent",    "Built-in skills and plain-text mode for LLM agents"},
    };
    struct OptEntry { std::string flag; std::string desc; };
    OptEntry opts[] = {
        {"-y, --yes",       "Skip confirmation prompts"},
        {"-v, --verbose",   "Enable verbose output"},
        {"-q, --quiet",     "Suppress non-essential output"},
        {"    --agent",     "Plain-text output for LLM agents (no TUI/ANSI)"},
    };

    std::vector<Pair> cmdEntries;
    for (auto& c : cmds) cmdEntries.emplace_back(c.name, c.desc);
    std::vector<Pair> optEntries;
    for (auto& o : opts) optEntries.emplace_back(o.flag, o.desc);

    constexpr std::string_view kTagline =
        "A modern package manager and development environment tool";
    constexpr std::string_view kAgentLine1 =
        "If you are an LLM/AI agent, run `xlings agent` FIRST.";
    constexpr std::string_view kAgentLine2 =
        "It contains usage instructions designed specifically for you.";

    std::span<const Pair> sections[] = { cmdEntries, optEntries };
    auto P = plan_help_(sections,
                        std::max({ layout::display_width(kTagline) + 4,
                                   layout::display_width(kAgentLine2) + 6,
                                   40 }));

    Elements rows;
    rows.push_back(text(""));
    rows.push_back(hbox({
        text("  xlings") | theme::title(),
        text(" " + std::string(version)) | color(theme::dim_color()),
    }));
    rows.push_back(text(""));
    for (auto& line : layout::wrap_to_width(kTagline, std::max(1, P.width - 2))) {
        rows.push_back(text("  " + line) | color(theme::text_color()));
    }
    rows.push_back(text(""));

    // USAGE
    rows.push_back(text("  USAGE") | bold | color(theme::text_color()));
    rows.push_back(
        text("    xlings [OPTIONS] [SUBCOMMAND]") | color(theme::dim_color()));
    rows.push_back(text(""));

    help_section_(rows, "SUBCOMMANDS", cmdEntries, P, bold | color(theme::magenta()));
    help_section_(rows, "OPTIONS", optEntries, P, color(theme::dim_color()));

    // AGENT hint — strong signal for LLM agents
    rows.push_back(text("  FOR AI AGENTS") | bold | color(theme::text_color()));
    for (auto& line : layout::wrap_to_width(kAgentLine1, std::max(1, P.width - 4))) {
        rows.push_back(text("    " + line) | color(theme::text_color()));
    }
    for (auto& line : layout::wrap_to_width(kAgentLine2, std::max(1, P.width - 4))) {
        rows.push_back(text("    " + line) | color(theme::dim_color()));
    }
    rows.push_back(text(""));

    layout::print_rows(std::move(rows), P.width);
}

// Print a styled tip message
void print_tip(std::string_view message) {
    using namespace ftxui;
    int width = layout::fit_width(layout::display_width(message) + 6);
    Elements rows;
    auto lines = layout::wrap_to_width(message, std::max(1, width - 4));
    for (std::size_t i = 0; i < lines.size(); ++i) {
        rows.push_back(hbox({
            i == 0 ? (text("  " + std::string(theme::icon::info) + " ")
                      | color(theme::cyan()))
                   : text("    "),
            text(lines[i]) | color(theme::dim_color()),
        }));
    }
    layout::print_rows(std::move(rows), width);
}

// Print a styled usage message
void print_usage(std::string_view usage) {
    using namespace ftxui;
    constexpr std::string_view kPrefix = "  Usage: ";
    int lead = static_cast<int>(kPrefix.size());
    int width = layout::fit_width(lead + layout::display_width(usage));
    Elements rows;
    auto lines = layout::wrap_to_width(usage, std::max(1, width - lead));
    for (std::size_t i = 0; i < lines.size(); ++i) {
        rows.push_back(hbox({
            i == 0 ? (text(std::string(kPrefix)) | color(theme::dim_color()))
                   : text(std::string(static_cast<std::size_t>(lead), ' ')),
            text(lines[i]) | color(theme::text_color()),
        }));
    }
    layout::print_rows(std::move(rows), width);
}

} // namespace xlings::ui
