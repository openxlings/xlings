export module xlings.ui.gui;

import std;
import xlings.theme;

// The graphical frontend, drawn from the SAME objects the terminal draws from.
//
// This is the whole point of the exercise and the only thing worth checking
// about it: a `Diagnostic` carries a summary, where the constraint came from,
// the evidence, and the ways out. A terminal renders those as an indented
// block with copyable command lines; this renders them as a panel with
// buttons. If the GUI ever needs a fact the object does not carry, the object
// is wrong -- the GUI does not get to go and look it up itself.
//
// SCOPE
//
// A working frame, not a product: show what a command produced, offer its
// actions, run one. Layout, navigation and the rest are deliberately not
// designed yet.
//
// WHY THE VIEW MODEL IS SEPARATE FROM THE DRAWING
//
// Everything below the `draw_*` functions is plain data with no ImGui in it,
// so the interesting half can be tested without a display. The drawing itself
// is exercised against `imgui.backend.headless`, which runs real frames with
// no window -- see tests/unit/test_gui_view.cpp.
export namespace xlings::ui::gui {

// Mirrors `diag::Level` without importing it: this package must not depend on
// core (see the dependency whitelist in the design doc), and the mapping is
// one line at the boundary.
enum class Severity { Note, Warn, Error };

struct Action {
    std::string label;
    std::string command;
};

struct Fact {
    std::string label;
    std::string value;
};

// One rendered problem or report.
struct Panel {
    Severity severity { Severity::Note };
    std::string code;
    std::string summary;
    std::string source;
    std::vector<Fact>   facts;
    std::vector<Action> actions;
};

// What the window is showing right now.
struct Model {
    std::string title { "xlings" };
    // Free-running log of what the command said, in order.
    std::vector<std::string> transcript;
    std::vector<Panel> panels;
    // Set by the UI when a button is pressed; the host drains it.
    std::optional<Action> pending;
    bool busy { false };
};

// The colour a severity draws in, taken from the shared theme so the GUI and
// the terminal cannot drift into different ideas of "error".
[[nodiscard]] theme::Rgb severity_color(Severity s, theme::Background bg);

// Immediate-mode draw of the whole window. Call once per frame between
// NewFrame and Render. Returns true while the window should stay open.
bool draw(Model& model);

}  // namespace xlings::ui::gui
