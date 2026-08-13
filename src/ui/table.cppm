module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/table.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:table;

import std;
import :theme;
import :layout;

export namespace xlings::ui {

namespace detail {

// Fit a bordered table to the terminal.
//
// A table that does not fit used to be clipped at the screen edge, which
// takes the right-hand border with it and leaves something that reads as a
// half-drawn frame. Column 0 carries the identifier and keeps its natural
// width; the rest give ground, widest first, until the table fits.
inline int fit_columns_(std::vector<std::vector<std::string>>& data) {
    if (data.empty() || data.front().empty()) return layout::fit_width(0);

    const std::size_t cols = data.front().size();
    std::vector<int> w(cols, 0);
    for (auto& row : data) {
        for (std::size_t c = 0; c < cols && c < row.size(); ++c) {
            w[c] = std::max(w[c], layout::display_width(row[c]));
        }
    }

    // Borders: one leading, one trailing, one between each pair of columns,
    // plus a space of padding either side of every cell.
    const int chrome = static_cast<int>(cols) + 1 + 2 * static_cast<int>(cols);
    auto total = [&] {
        int t = chrome;
        for (auto x : w) t += x;
        return t;
    };

    const int avail = layout::fit_width(total());
    while (total() > avail) {
        // Widest shrinkable column (never column 0).
        std::size_t victim = 0;
        int best = 0;
        for (std::size_t c = 1; c < cols; ++c) {
            if (w[c] > best) { best = w[c]; victim = c; }
        }
        if (victim == 0 || best <= layout::kMinDescW) break;
        w[victim] = std::max(layout::kMinDescW, best - (total() - avail));
        if (w[victim] == best) break;   // no progress; stop rather than spin
    }

    for (auto& row : data) {
        for (std::size_t c = 0; c < cols && c < row.size(); ++c) {
            row[c] = layout::truncate_to_width(row[c], w[c]);
        }
    }
    return std::min(avail, total());
}

}  // namespace detail

// Print a formatted table with headers and rows
void print_table(std::span<const std::string> headers,
                 std::span<const std::vector<std::string>> rows);

// Print search results as a table
void print_search_results(
    std::span<const std::pair<std::string, std::string>> results);

} // namespace xlings::ui
