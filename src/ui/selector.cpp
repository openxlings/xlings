module;

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;
import xlings.core.palette;
import :theme;
import :layout;

namespace xlings::ui {

std::optional<std::string>
select_package(std::span<const std::pair<std::string, std::string>> items) {
    using namespace ftxui;

    if (items.empty()) return std::nullopt;
    if (items.size() == 1) return std::string { items[0].first };

    int selected { 0 };
    bool confirmed { false };

    std::vector<std::string> labels;
    labels.reserve(items.size());
    int nameW = 20;
    for (auto& [name, desc] : items) {
        nameW = std::max(nameW, layout::display_width(name));
    }
    for (auto& [name, desc] : items) {
        labels.push_back(layout::pad_to_width(name, nameW) + " " + desc);
    }

    auto menu = Menu(&labels, &selected);
    auto screen = ScreenInteractive::TerminalOutput();

    auto component = CatchEvent(menu, [&](Event event) {
        if (event == Event::Return) {
            confirmed = true;
            screen.Exit();
            return true;
        }
        if (event == Event::Escape || event == Event::Character('q')) {
            screen.Exit();
            return true;
        }
        return false;
    });

    screen.Loop(Renderer(component, [&] {
        return vbox({
            text(" Select a package:") | theme::title(),
            separator() | color(theme::border_color()),
            component->Render() | vscroll_indicator | frame
                | size(HEIGHT, LESS_THAN, 20),
            separator() | color(theme::border_color()),
            text(" \u2191\u2193 navigate  Enter select  Esc cancel") | theme::hint(),
        }) | borderRounded | color(theme::border_color());
    }));

    if (confirmed && selected >= 0 && selected < (int)items.size()) {
        return std::string { items[selected].first };
    }
    return std::nullopt;
}

std::optional<int>
select_option(std::string_view title, std::span<const std::pair<std::string, std::string>> items, std::string_view done_label, int default_idx) {
    using namespace ftxui;

    if (items.empty()) return std::nullopt;

    int selected = (default_idx >= 0 && default_idx < static_cast<int>(items.size()))
        ? default_idx : 0;
    bool confirmed { false };

    // Build labels: "name   description"
    std::vector<std::string> labels;
    labels.reserve(items.size() + (done_label.empty() ? 0 : 1));
    int nameW = 24;
    for (auto& [name, desc] : items) {
        nameW = std::max(nameW, layout::display_width(name));
    }
    for (auto& [name, desc] : items) {
        labels.push_back(layout::pad_to_width(name, nameW) + desc);
    }
    if (!done_label.empty()) {
        labels.push_back(std::string(done_label));
    }

    MenuOption menu_opt;
    menu_opt.entries_option.transform = [](const EntryState& state) {
        auto e = text((state.focused ? "> " : "  ") + state.label);
        if (state.focused) {
            e = e | bold | inverted;
        } else {
            e = e | color(theme::text_color());
        }
        return e;
    };
    auto menu = Menu(&labels, &selected, menu_opt);
    auto screen = ScreenInteractive::Fullscreen();

    auto component = CatchEvent(menu, [&](Event event) {
        if (event == Event::Return) {
            confirmed = true;
            screen.Exit();
            return true;
        }
        if (event == Event::Escape || event == Event::Character('q')) {
            screen.Exit();
            return true;
        }
        return false;
    });

    screen.Loop(Renderer(component, [&] {
        auto box = vbox({
            text(" " + std::string(title)) | theme::title(),
            separator() | color(theme::border_color()),
            component->Render() | vscroll_indicator | frame
                | size(HEIGHT, LESS_THAN, 15),
            separator() | color(theme::border_color()),
            text(" \u2191\u2193 navigate  Enter select  Esc cancel") | theme::hint(),
        }) | borderRounded | color(theme::border_color())
           | size(WIDTH, LESS_THAN, 72);

        return box | center;
    }));

    if (!confirmed) return std::nullopt;

    // "done" item selected
    if (!done_label.empty() && selected == static_cast<int>(items.size())) {
        return -1; // sentinel for "done"
    }

    if (selected >= 0 && selected < static_cast<int>(items.size())) {
        return selected;
    }
    return std::nullopt;
}

std::string read_line(std::string_view prompt) {
    std::print("{}  {} {}", palette::fg(palette::cyan()), prompt, palette::off());
    std::cout.flush();
    std::string line;
    std::getline(std::cin, line);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    while (!line.empty() && line.front() == ' ')
        line.erase(line.begin());
    return line;
}

bool confirm(std::string_view message, bool defaultYes) {
    std::string prompt = defaultYes ? "[Y/n] " : "[y/N] ";
    // Inline prompt (not an ftxui-rendered document), so it writes SGR
    // directly — from the shared palette, which also turns it off under
    // NO_COLOR / a pipe.
    std::print("{}  {} {}{}{}{}{}",
               palette::fg(palette::cyan()), theme::icon::arrow, palette::off(),
               message,
               palette::fg(palette::dim()), prompt, palette::off());
    std::cout.flush();

    std::string input;
    if (!std::getline(std::cin, input)) return defaultYes;
    if (input.empty()) return defaultYes;

    return input[0] == 'y' || input[0] == 'Y';
}

}
