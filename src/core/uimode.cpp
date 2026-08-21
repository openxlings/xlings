module xlings.core.uimode;

import std;
import xlings.core.palette;

namespace xlings::ui {

namespace {

UiMode gMode_ { UiMode::Cli };
UiCapabilities gCaps_ {};

}  // namespace

[[nodiscard]] std::optional<UiMode> parse_mode(std::string_view s) {
    if (s == "cli")  return UiMode::Cli;
    if (s == "tui")  return UiMode::Tui;
    if (s == "auto" || s.empty()) return std::nullopt;
    return std::nullopt;
}

[[nodiscard]] Detected detect() {
    // Every question routed through palette, which already owns the terminal
    // probes and their reasoning (NO_COLOR asks for no colour, not for no
    // cursor control; a pipe has no width limit rather than a default of 80).
    // Re-deriving any of it here would be a fifth knob.
    return Detected{
        .stdoutIsTerminal = palette::stdout_is_terminal(),
        .colorAllowed     = palette::colors_enabled(),
        .width            = std::nullopt,   // filled by the ui layer, which owns width
    };
}

[[nodiscard]] Resolution resolve(std::optional<UiMode> preferred,
                                 bool agentMode,
                                 const Detected& env) {
    // --agent is not a rendering; it is "there is a machine on the other end".
    // It pins cli because a machine wants stable plain text, and it outranks a
    // configured preference because it was typed on this invocation.
    if (agentMode) {
        if (preferred == UiMode::Tui) {
            return { UiMode::Cli, "--agent was passed, which implies plain text" };
        }
        return { UiMode::Cli, {} };
    }

    if (!env.stdoutIsTerminal) {
        if (preferred == UiMode::Tui) {
            // Say so. A user who put `"uiMode": "tui"` in their config and
            // pipes the output should learn why it looks different, not
            // conclude the setting does nothing.
            return { UiMode::Cli, "stdout is not a terminal" };
        }
        return { UiMode::Cli, {} };
    }

    // A terminal with no explicit preference gets the rendered form; that is
    // what xlings has always done and what most users see.
    return { preferred.value_or(UiMode::Tui), {} };
}

[[nodiscard]] UiCapabilities capabilities_of(UiMode mode,
                                             bool interactivePreference,
                                             bool agentMode,
                                             const Detected& env) {
    UiCapabilities c;
    c.color = (mode == UiMode::Tui) && env.colorAllowed;
    // Redrawing in place needs a terminal AND a frontend that draws frames.
    c.cursorRewrite = (mode == UiMode::Tui) && env.stdoutIsTerminal;
    // Interactivity is a preference, but it is gated by physics: nobody is
    // there to press a key on a pipe, and --agent says outright that the
    // reader is a program. See EventStream::set_interactive for what guessing
    // an answer cost.
    c.interactive = interactivePreference && !agentMode && env.stdoutIsTerminal;
    c.width = env.width;
    return c;
}

void set_current(UiMode mode, UiCapabilities caps) {
    gMode_ = mode;
    gCaps_ = caps;
}

[[nodiscard]] UiMode current_mode() { return gMode_; }

[[nodiscard]] const UiCapabilities& current_capabilities() { return gCaps_; }

}  // namespace xlings::ui
