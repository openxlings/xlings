// `xlings-gui` -- the graphical frontend's entry point.
//
// Deliberately thin. Everything that decides what to SHOW lives in
// `xlings.ui.gui` (and can be exercised headless); this file owns the window
// and the worker thread, and nothing else.
//
// IN-PROCESS ON PURPOSE
//
// A second binary, but not a second architecture: the worker calls the same
// `cmd_*` functions the CLI calls and consumes the same EventStream. No
// NDJSON, no IPC, no second copy of the protocol. Splitting the process later
// means replacing this file, not the design -- which is why `ui/gui` may not
// depend on core's data modules (see the whitelist in the design doc).
//
// Built only with `--features gui`; `required_features` makes its absence a
// silent skip rather than a build failure, so the static-musl release simply
// does not produce it.

import std;
import xlings.core;
import xlings.platform;
import xlings.runtime;
import xlings.ui.gui;
import imgui.app;

namespace {

using xlings::ui::gui::Action;
using xlings::ui::gui::Fact;
using xlings::ui::gui::Model;
using xlings::ui::gui::Panel;
using xlings::ui::gui::Severity;

std::mutex gModelMutex;

Severity to_severity_(xlings::LogLevel level) {
    switch (level) {
        case xlings::LogLevel::error: return Severity::Error;
        case xlings::LogLevel::warn:  return Severity::Warn;
        default:                      return Severity::Note;
    }
}

// Bridge the one stream both frontends read.
//
// The terminal renders these events as indented blocks; here they become
// panels. Same objects, two renderings -- if this needed a field the event
// does not carry, the event would be the thing to fix.
void attach_(xlings::EventStream& stream, Model& model) {
    stream.on_event([&model](const xlings::Event& e) {
        std::lock_guard guard(gModelMutex);
        if (auto* er = std::get_if<xlings::ErrorEvent>(&e)) {
            Panel p;
            p.severity = Severity::Error;
            p.code = std::string(to_wire_string(er->code));
            p.summary = er->message;
            if (!er->hint.empty()) p.actions.push_back(Action{ "hint", er->hint });
            model.panels.push_back(std::move(p));
        } else if (auto* l = std::get_if<xlings::LogEvent>(&e)) {
            if (l->level == xlings::LogLevel::debug) return;
            Panel p;
            p.severity = to_severity_(l->level);
            p.code = "log";
            p.summary = l->message;
            model.panels.push_back(std::move(p));
        } else if (auto* pr = std::get_if<xlings::ProgressEvent>(&e)) {
            model.transcript.push_back(
                std::format("{} {}", pr->phase, pr->message));
        }
    });
}

}  // namespace

int main(int argc, char* argv[]) {
    xlings::platform::init_console_output();
    (void)xlings::Config::paths();          // resolve the home before anything reads it

    Model model;
    model.title = std::format("xlings {}", xlings::Info::VERSION);

    xlings::EventStream stream;
    attach_(stream, model);
    // A window can wait for a human indefinitely, so unlike a pipe there is
    // somebody to ask -- but the prompt plumbing is not wired yet, and
    // claiming otherwise would make commands hang on a question nothing draws.
    stream.set_interactive(false);

    {
        std::lock_guard guard(gModelMutex);
        Panel p;
        p.severity = Severity::Note;
        p.code = "gui.hello";
        p.summary = "xlings graphical frontend";
        p.facts.push_back(Fact{ "home", xlings::Config::paths().homeDir.string() });
        p.facts.push_back(Fact{ "subos", xlings::Config::paths().activeSubos });
        p.actions.push_back(Action{ "list packages", "list" });
        model.panels.push_back(std::move(p));
    }

    // One command at a time, off the UI thread. `cmd_*` are ordinary blocking
    // functions; running one on the render thread would freeze the window for
    // the length of a download.
    std::jthread worker;
    auto dispatch = [&](const Action& action) {
        if (worker.joinable()) return;      // still busy
        {
            std::lock_guard guard(gModelMutex);
            model.busy = true;
        }
        worker = std::jthread([&stream, &model, cmd = action.command] {
            if (cmd == "list") {
                (void)xlings::xim::cmd_list("", stream, /*all=*/false);
            } else {
                std::lock_guard guard(gModelMutex);
                Panel p;
                p.severity = Severity::Warn;
                p.code = "gui.unsupported";
                p.summary = std::format("'{}' is not wired into the GUI yet", cmd);
                p.actions.push_back(Action{ "run it in a terminal",
                                            std::format("xlings {}", cmd) });
                model.panels.push_back(std::move(p));
            }
            std::lock_guard guard(gModelMutex);
            model.busy = false;
        });
    };

    ImGui::App::Options opts;
    opts.title = model.title.c_str();
    const int rc = ImGui::App::run(opts, [&] {
        std::optional<Action> pending;
        {
            std::lock_guard guard(gModelMutex);
            xlings::ui::gui::draw(model);
            pending = std::exchange(model.pending, std::nullopt);
        }
        if (pending) {
            if (worker.joinable()) worker.join();
            dispatch(*pending);
        }
    });

    if (worker.joinable()) worker.join();
    (void)argc; (void)argv;
    return rc;
}
