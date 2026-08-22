module;

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;
import xlings.core.palette;
import xlings.i18n;

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
        // Same shape as select_option above; see the note there.
        return vbox({
            hbox({
                text("  " + std::string(theme::icon::package) + " ")
                    | color(theme::accent()),
                text(std::string(i18n::tr("ui.select_package")))
                    | theme::style::title(),
            }),
            component->Render() | vscroll_indicator | frame
                | size(HEIGHT, LESS_THAN, 20),
            text("    " + std::string(i18n::tr("ui.select_keys")))
                | color(theme::muted()),
        });
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
        // `▸` and a colour, NOT an inverted block.
        //
        // `inverted` paints the full row width, so on a wide terminal one
        // short option becomes a bar across the screen -- and it is the only
        // place in xlings that does that. The same marker the rest of the
        // output uses for "this one" (`xlings subos`, `xlings use --all`)
        // reads as the same product.
        auto e = text((state.focused ? "  " + std::string(theme::icon::active) + " "
                                     : "    ") + state.label);
        e = state.focused ? (e | bold | color(theme::accent()))
                          : (e | color(theme::text()));
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
        // NO BOX.
        //
        // The rounded border, the two separators and the hint line cost six
        // rows to choose between two options, and none of the rest of xlings
        // draws boxes -- `xlings subos`, `xlings list` and every diagnostic
        // are left-aligned with a `◆` title and `▸` for the current item. A
        // framed widget in the middle of that reads as another program's
        // output.
        //
        // The border was also the least readable thing on screen: it used
        // `theme::border()`, which is #334155 on the default dark theme --
        // about 1.6:1 against the #0D1117 background. Six of the seven
        // coloured elements here were that colour.
        return vbox({
            hbox({
                text("  " + std::string(theme::icon::package) + " ")
                    | color(theme::accent()),
                text(std::string(title)) | theme::style::title(),
            }),
            component->Render() | vscroll_indicator | frame
                | size(HEIGHT, LESS_THAN, 15),
            text("    " + std::string(i18n::tr("ui.select_keys")))
                | color(theme::muted()),
        });
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
