module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:banner;

import std;
import :theme;

export namespace xlings::ui {

// Pad string to desired width (avoids std::format width specifiers - GCC 15 crash)
std::string pad_to(std::string s, std::size_t width) {
    while (s.size() < width) s += ' ';
    return s;
}

// Render FTXUI rows and print to stdout
void render_rows_(ftxui::Elements rows) {
    using namespace ftxui;
    auto doc = vbox(std::move(rows));
    auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(doc));
    Render(screen, doc);
    screen.Print();
    std::println("");
}

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

// Print TUI-styled subcommand help
void print_subcommand_help(std::string_view name,
                           std::string_view description,
                           std::span<const HelpArg> args,
                           std::span<const HelpOpt> opts,
                           std::span<const HelpOpt> subcmds = {}) {
    using namespace ftxui;

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
        rows.push_back(
            text("  " + std::string(description)) | color(theme::text_color()));
        rows.push_back(text(""));
    }

    // USAGE
    rows.push_back(text("  USAGE") | bold | color(theme::text_color()));
    {
        std::string usage = "    xlings " + std::string(name);
        if (!subcmds.empty()) usage += " [SUBCOMMAND]";
        if (!opts.empty()) usage += " [OPTIONS]";
        for (auto& a : args) {
            if (a.required)
                usage += " <" + a.name + ">";
            else
                usage += " [" + a.name + "]";
        }
        rows.push_back(text(usage) | color(theme::dim_color()));
    }
    rows.push_back(text(""));

    // SUBCOMMANDS
    if (!subcmds.empty()) {
        rows.push_back(text("  SUBCOMMANDS") | bold | color(theme::text_color()));
        for (auto& s : subcmds) {
            rows.push_back(hbox({
                text("    "),
                text(pad_to(s.flag, 24)) | bold | color(theme::magenta()),
                text("  " + s.desc) | color(theme::text_color()),
            }));
        }
        rows.push_back(text(""));
    }

    // ARGS
    bool hasArgs = false;
    for (auto& a : args) {
        if (!a.desc.empty()) { hasArgs = true; break; }
    }
    if (hasArgs) {
        rows.push_back(text("  ARGS") | bold | color(theme::text_color()));
        for (auto& a : args) {
            if (a.desc.empty()) continue;
            rows.push_back(hbox({
                text("    "),
                text(pad_to("<" + a.name + ">", 24)) | bold | color(theme::magenta()),
                text("  " + a.desc) | color(theme::text_color()),
            }));
        }
        rows.push_back(text(""));
    }

    // OPTIONS
    if (!opts.empty()) {
        rows.push_back(text("  OPTIONS") | bold | color(theme::text_color()));
        for (auto& o : opts) {
            rows.push_back(hbox({
                text("    "),
                text(pad_to(o.flag, 24)) | color(theme::dim_color()),
                text("  " + o.desc) | color(theme::text_color()),
            }));
        }
        rows.push_back(text(""));
    }

    render_rows_(std::move(rows));
}

// Print styled top-level help text
void print_help(std::string_view version) {
    using namespace ftxui;

    Elements rows;

    rows.push_back(text(""));
    rows.push_back(hbox({
        text("  xlings") | theme::title(),
        text(" " + std::string(version)) | color(theme::dim_color()),
    }));
    rows.push_back(text(""));
    rows.push_back(
        text("  A modern package manager and development environment tool")
            | color(theme::text_color()));
    rows.push_back(text(""));

    // USAGE
    rows.push_back(text("  USAGE") | bold | color(theme::text_color()));
    rows.push_back(
        text("    xlings [OPTIONS] [SUBCOMMAND]") | color(theme::dim_color()));
    rows.push_back(text(""));

    // SUBCOMMANDS
    rows.push_back(text("  SUBCOMMANDS") | bold | color(theme::text_color()));

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
    for (auto& cmd : cmds) {
        rows.push_back(hbox({
            text("    "),
            text(pad_to(cmd.name, 12)) | bold | color(theme::magenta()),
            text("  " + cmd.desc) | color(theme::text_color()),
        }));
    }
    rows.push_back(text(""));

    // OPTIONS
    rows.push_back(text("  OPTIONS") | bold | color(theme::text_color()));

    struct OptEntry { std::string flag; std::string desc; };
    OptEntry opts[] = {
        {"-y, --yes",       "Skip confirmation prompts"},
        {"-v, --verbose",   "Enable verbose output"},
        {"-q, --quiet",     "Suppress non-essential output"},
        {"    --agent",     "Plain-text output for LLM agents (no TUI/ANSI)"},
    };
    for (auto& opt : opts) {
        rows.push_back(hbox({
            text("    "),
            text(pad_to(opt.flag, 24)) | color(theme::dim_color()),
            text("  " + opt.desc) | color(theme::text_color()),
        }));
    }
    rows.push_back(text(""));

    render_rows_(std::move(rows));
}

// Print a styled tip message
void print_tip(std::string_view message) {
    using namespace ftxui;
    auto doc = hbox({
        text("  " + std::string(theme::icon::info) + " ") | color(theme::cyan()),
        text(std::string(message)) | color(theme::dim_color()),
    });
    auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(doc));
    Render(screen, doc);
    screen.Print();
    std::println("");
}

// Print a styled usage message
void print_usage(std::string_view usage) {
    using namespace ftxui;
    auto doc = hbox({
        text("  Usage: ") | color(theme::dim_color()),
        text(std::string(usage)) | color(theme::text_color()),
    });
    auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(doc));
    Render(screen, doc);
    screen.Print();
    std::println("");
}

} // namespace xlings::ui
