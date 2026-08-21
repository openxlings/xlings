module xlings.ui.gui;

import std;
import imgui.core;
namespace xlings::ui::gui {

namespace {

// The severity as a leading tag, because `imgui.core` re-exports a small
// surface -- Begin/End/Button/TextUnformatted and the docking helpers -- and
// no colour or layout calls at all.
//
// There WAS a `severity_color()` here mapping Severity onto the shared theme
// slots. It had no caller, and an exported function with no caller is exactly
// what `ui/selector.cpp` was for a year before this change went and wired it
// up. When `imgui.core` exports PushStyleColor/TextColored, the mapping is
// three lines (Error->Slot::Error, Warn->Slot::Warn, Note->Slot::Accent) and
// belongs here then -- not now, sitting unused and claiming to prove that the
// GUI and the terminal share a theme.
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
