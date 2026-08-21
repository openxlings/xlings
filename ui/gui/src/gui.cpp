module xlings.ui.gui;

import std;
import imgui.core;
import xlings.theme;

namespace xlings::ui::gui {

namespace {

// The severity as a leading tag, because `imgui.core` re-exports a small
// surface -- Begin/End/Button/TextUnformatted and the docking helpers -- and
// no colour or layout calls. `severity_color` below is still the contract
// point and is covered by tests; when the imgui module widens, this becomes a
// colour instead of a prefix and nothing else moves.
std::string_view tag_(Severity s) {
    switch (s) {
        case Severity::Error: return "[error] ";
        case Severity::Warn:  return "[warn]  ";
        case Severity::Note:  return "[note]  ";
    }
    return "";
}

void line_(std::string_view text) {
    // TextUnformatted takes a range, which also means a `%` in a package
    // description cannot be read as a format specifier.
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

}  // namespace

[[nodiscard]] theme::Rgb severity_color(Severity s, theme::Background bg) {
    // Straight through the shared theme. A GUI with its own idea of "error
    // red" is a second place to change when somebody picks a theme, and the
    // one that gets forgotten.
    switch (s) {
        case Severity::Error: return theme::color(theme::Slot::Error, bg);
        case Severity::Warn:  return theme::color(theme::Slot::Warn,  bg);
        case Severity::Note:  return theme::color(theme::Slot::Accent, bg);
    }
    return theme::color(theme::Slot::Text, bg);
}

bool draw(Model& model) {
    bool open = true;
    ImGui::Begin(model.title.c_str(), &open);

    if (model.busy) line_("working...");

    for (auto& panel : model.panels) {
        line_(std::string(tag_(panel.severity)) + panel.summary);

        // Where the constraint came from, first -- for the same reason the
        // terminal puts it first: a version the user never typed is
        // unactionable until they know who asked for it.
        if (!panel.source.empty()) line_("        from  " + panel.source);
        for (const auto& f : panel.facts) {
            line_(std::format("        {}  {}", f.label, f.value));
        }

        // The same `actions` the terminal prints as copyable command lines.
        // If a button here needed something the Action does not carry, the
        // Action would be under-specified -- that check is the reason this
        // frontend earns its place at this stage.
        for (const auto& a : panel.actions) {
            if (a.command.empty()) continue;
            // Labelled with the command so two actions of a panel cannot
            // collide in imgui's id stack (PushID is not exported yet).
            const auto id = std::format("{}##{}", a.label, a.command);
            if (ImGui::Button(id.c_str())) model.pending = a;
        }
    }

    for (const auto& l : model.transcript) line_(l);

    ImGui::End();
    return open;
}

}  // namespace xlings::ui::gui
