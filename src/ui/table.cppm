module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/table.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

export module xlings.ui:table;

import std;
import :theme;

export namespace xlings::ui {

// Print a formatted table with headers and rows
void print_table(std::span<const std::string> headers,
                 std::span<const std::vector<std::string>> rows) {
    using namespace ftxui;

    std::vector<std::vector<std::string>> tableData;
    tableData.push_back(std::vector<std::string>(headers.begin(), headers.end()));
    for (auto& row : rows) {
        tableData.push_back(row);
    }

    auto table = Table(std::move(tableData));
    table.SelectAll().Border(ROUNDED);
    table.SelectRow(0).Decorate(bold);
    table.SelectRow(0).Decorate(color(theme::cyan()));
    table.SelectRow(0).SeparatorVertical(LIGHT);
    table.SelectRow(0).BorderBottom(LIGHT);

    auto doc = table.Render();
    auto screen = Screen::Create(Dimension::Full(), theme::fit_full_height(doc));
    Render(screen, doc);
    screen.Print();
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

    auto doc = table.Render();
    auto screen = Screen::Create(Dimension::Full(), theme::fit_full_height(doc));
    Render(screen, doc);
    screen.Print();
    std::println("");
}

} // namespace xlings::ui
