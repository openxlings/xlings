module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/table.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;

namespace xlings::ui {

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

}
