module;

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:selector;

import std;
import xlings.core.palette;
import :theme;
import :layout;

export namespace xlings::ui {

// Interactive package selector from name-description pairs
std::optional<std::string>
select_package(std::span<const std::pair<std::string, std::string>> items);

// Generic option selector with title + description pairs (fullscreen, centered)
// Returns selected index or nullopt if cancelled
// default_idx: pre-selected item index (-1 for first)
std::optional<int>
select_option(std::string_view title,
              std::span<const std::pair<std::string, std::string>> items,
              std::string_view done_label = "",
              int default_idx = 0);

// Read a line from stdin with ANSI-colored prompt.
//
// Colors come from the shared palette rather than a dark-palette literal, so
// the prompt follows the terminal background and goes quiet under NO_COLOR —
// it is the one line the user has to read before they can answer it.
std::string read_line(std::string_view prompt);

// Simple yes/no confirmation with styled prompt.
//
// Indented two columns like every panel above it. It used to start at column
// 0, so the question sat outside the block it was asking about.
bool confirm(std::string_view message, bool defaultYes);

} // namespace xlings::ui
