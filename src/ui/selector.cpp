module;

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;
import xlings.core.palette;

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
            text(" Select a package:") | theme::style::title(),
            separator() | color(theme::border()),
            component->Render() | vscroll_indicator | frame
                | size(HEIGHT, LESS_THAN, 20),
            separator() | color(theme::border()),
            text(" \u2191\u2193 navigate  Enter select  Esc cancel") | theme::style::hint(),
        }) | borderRounded | color(theme::border());
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
    // ftxui tracks the SELECTED entry and the FOCUSED one separately; the
    // marker below draws from `focused`, which starts at 0 regardless of what
    // `selected` was initialised to. Without this the cursor opens on the
    // newest version while "(current)" sits two rows down -- the picker would
    // be pointing away from where the user is.
    menu_opt.focused_entry = selected;
    menu_opt.entries_option.transform = [](const EntryState& state) {
        auto e = text((state.focused ? "> " : "  ") + state.label);
        if (state.focused) {
            e = e | bold | inverted;
        } else {
            e = e | color(theme::text());
        }
        return e;
    };
    auto menu = Menu(&labels, &selected, menu_opt);
    // Inline, NOT full-screen.
    //
    // A widget that takes over the terminal cannot appear in a CI log, cannot
    // be piped, and has to restore the screen on every exit path including a
    // signal. Staying in the scrollback removes all of that: the block is
    // drawn, answered, collapsed to its result, and the history above it is
    // untouched.
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
        auto box = vbox({
            text(" " + std::string(title)) | theme::style::title(),
            separator() | color(theme::border()),
            component->Render() | vscroll_indicator | frame
                | size(HEIGHT, LESS_THAN, 15),
            separator() | color(theme::border()),
            text(" \u2191\u2193 navigate  Enter select  Esc cancel") | theme::style::hint(),
        }) | borderRounded | color(theme::border())
           | size(WIDTH, LESS_THAN, 72);

        // Left-aligned, not centred. Centring is a full-screen habit: inline,
        // the block sits in the scrollback among left-aligned log lines, and a
        // floating box reads as a different program's output.
        return box;
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

std::optional<bool> confirm(std::string_view message, bool defaultYes) {
    std::string prompt = defaultYes ? "[Y/n] " : "[y/N] ";
    // STDERR, not stdout.
    //
    // A question is not output. Written to stdout, `xlings install foo > log`
    // puts the question in the file and shows the user a process that appears
    // to have hung -- with a terminal on stdin, so it really is waiting for
    // them. This matters now in a way it did not before: confirmations were
    // effectively never asked until the gate moved off the picker preference.
    //
    // Inline (not an ftxui document), so it writes SGR directly — from the
    // shared palette, which also turns it off under NO_COLOR / a pipe.
    std::print(stderr, "{}  {} {}{}{}{}{}",
               palette::fg(palette::cyan()), theme::icon::arrow, palette::off(),
               message,
               palette::fg(palette::dim()), prompt, palette::off());
    std::fflush(stderr);

    std::string input;
    // EOF is NOT the default.
    //
    // Returning `defaultYes` here is the same guess `NobodyToAsk` exists to
    // stop, smuggled onto the interactive path: stdin ended, nobody said
    // anything, and answering on their behalf would install or refuse
    // something silently. `nullopt` lets the caller report that nothing
    // happened.
    if (!std::getline(std::cin, input)) return std::nullopt;
    if (input.empty()) return defaultYes;

    return input[0] == 'y' || input[0] == 'Y';
}

}
