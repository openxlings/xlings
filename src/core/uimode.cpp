module xlings.core.uimode;

import std;
import xlings.core.palette;
import xlings.platform;

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
        // From platform rather than palette: palette answers questions about
        // what may be DRAWN, and all of those are about the output side.
        .stdinIsTerminal  = platform::stdin_is_terminal(),
        .colorAllowed     = palette::colors_enabled(),
    };
}

[[nodiscard]] Resolution resolve(std::optional<UiMode> preferred,
                                 PreferenceOrigin origin,
                                 bool agentMode,
                                 const Detected& env) {
    // Only a demand made on this invocation is worth reporting when it cannot
    // be met; see PreferenceOrigin.
    const auto because = [&](std::string reason) {
        return origin == PreferenceOrigin::Flag ? std::move(reason)
                                                : std::string{};
    };
    // --agent is not a rendering; it is "there is a machine on the other end".
    // It pins cli because a machine wants stable plain text, and it outranks a
    // configured preference because it was typed on this invocation.
    if (agentMode) {
        if (preferred == UiMode::Tui) {
            return { UiMode::Cli,
                     because("--agent was passed, which implies plain text") };
        }
        return { UiMode::Cli, {} };
    }

    if (!env.stdoutIsTerminal) {
        if (preferred == UiMode::Tui) {
            return { UiMode::Cli, because("stdout is not a terminal") };
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

    // A confirmation may be asked whenever there is an input side to answer on
    // and the caller has not declared itself a program. NOT gated on the
    // stored preference: `-y` documents that a question exists, so whether it
    // is asked is a contract, not a look-and-feel setting.
    c.canConfirm = !agentMode && env.stdinIsTerminal;

    // A selection blocks with no default, so it additionally requires the
    // stored preference -- a promise, made once by the person who would be at
    // the keyboard, which a tty never was.
    c.canSelect = c.canConfirm && interactivePreference;
    return c;
}

void set_current(UiMode mode, UiCapabilities caps) {
    gMode_ = mode;
    gCaps_ = caps;
}

[[nodiscard]] UiMode current_mode() { return gMode_; }

[[nodiscard]] const UiCapabilities& current_capabilities() { return gCaps_; }

}  // namespace xlings::ui
