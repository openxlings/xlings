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
                   const layout::ColumnPlan& P, ftxui::Decorator nameStyle);

layout::ColumnPlan plan_help_(
        std::span<const std::span<const std::pair<std::string, std::string>>> sections,
        int titleW);

// Print TUI-styled subcommand help
void print_subcommand_help(std::string_view name,
                           std::string_view description,
                           std::span<const HelpArg> args,
                           std::span<const HelpOpt> opts,
                           std::span<const HelpOpt> subcmds = {});

// Print styled top-level help text
void print_help(std::string_view version);

// Print a styled tip message
void print_tip(std::string_view message);

// Print a styled usage message
void print_usage(std::string_view usage);

} // namespace xlings::ui
