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
                 std::span<const std::vector<std::string>> rows) {
    using namespace ftxui;

    std::vector<std::vector<std::string>> tableData;
    tableData.push_back(std::vector<std::string>(headers.begin(), headers.end()));
    for (auto& row : rows) {
        tableData.push_back(row);
    }
    int width = detail::fit_columns_(tableData);

    auto table = Table(std::move(tableData));
    table.SelectAll().Border(ROUNDED);
    table.SelectRow(0).Decorate(bold);
    table.SelectRow(0).Decorate(color(theme::cyan()));
    table.SelectRow(0).SeparatorVertical(LIGHT);
    table.SelectRow(0).BorderBottom(LIGHT);

    layout::print_doc(table.Render(), width);
    std::println("");
}

// Print search results as a table
void print_search_results(
    std::span<const std::pair<std::string, std::string>> results) {
    using namespace ftxui;

    if (results.empty()) return;

    std::vector<std::vector<std::string>> tableData;
    tableData.push_back({ "Package", "Description" });
    for (auto& [name, desc] : results) {
        tableData.push_back({ name, desc });
    }
    int width = detail::fit_columns_(tableData);

    auto table = Table(std::move(tableData));
    table.SelectAll().Border(ROUNDED);
    table.SelectRow(0).Decorate(bold);
    table.SelectRow(0).Decorate(color(theme::cyan()));
    table.SelectRow(0).BorderBottom(LIGHT);

    // Style package name column with magenta
    for (int i = 1; i < static_cast<int>(results.size()) + 1; ++i) {
        table.SelectCell(i, 0).Decorate(bold);
        table.SelectCell(i, 0).Decorate(color(theme::magenta()));
    }

    layout::print_doc(table.Render(), width);
    std::println("");
}

} // namespace xlings::ui
