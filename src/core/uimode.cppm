export module xlings.core.uimode;

import std;

// Which frontend is speaking, and what it is allowed to do.
//
// BEFORE THIS, THERE WERE EIGHT KNOBS AND NO OWNER
//
//   stdout_is_terminal / stderr_is_terminal   palette
//   NO_COLOR, TERM=dumb                       palette::opted_out_
//   XLINGS_THEME=dark|light|auto              palette background detection
//   XLINGS_TERM_WIDTH                         ui::layout::term_width
//   --agent                                   cli.cpp, ad hoc
//   platform::set_tui_mode(true)              interface.cpp only
//   -v / -q                                   log::set_level
//   Config::lang()                            read, echoed, never applied
//
// Nowhere could answer "what mode am I in". `--agent` and `interface` each
// assembled their own combination, so a third caller had to assemble a third.
//
// `palette.cppm` had already decomposed the terminal side correctly into three
// orthogonal questions (colour / cursor rewrite / live progress); this lifts
// that into something the whole program can ask.
//
// DELIBERATELY ONLY TWO MODES
//
// `gui` is a separate binary, not another way for this one to draw. `ndjson`
// is the `interface` subcommand -- a different entry point whose output is not
// a document for a human. Interactivity and colour scheme are CONFIGURATION,
// not modes: a command's non-interactive output must be byte-identical whether
// or not interaction is available, so that e2e has one expected output rather
// than two.
export namespace xlings::ui {

enum class UiMode {
    Cli,   // plain text: pipes, CI, --agent. No ftxui, no cursor control.
    Tui,   // ftxui rendering, inline. Never full-screen -- see the design doc.
};

struct UiCapabilities {
    bool color          { false };
    bool cursorRewrite  { false };  // may redraw in place
    bool interactive    { false };  // somebody can answer a question
};

// Width is deliberately NOT here. `ui::layout::term_width()` already owns it,
// including the XLINGS_TERM_WIDTH override and the "a pipe has no limit rather
// than a default of 80" rule. Carrying a copy would be a second answer to a
// question that has one -- which is the defect this whole module exists to
// remove, so reproducing it here would be a poor start.

[[nodiscard]] constexpr std::string_view to_string(UiMode m) {
    return m == UiMode::Tui ? "tui" : "cli";
}

// Parse a configured or typed value. Unknown values return nullopt so the
// caller can say so rather than silently picking a default -- a mode that
// quietly ignores what you asked for is the failure this codebase keeps
// rediscovering.
[[nodiscard]] std::optional<UiMode> parse_mode(std::string_view s);

// What the environment can actually support, before preference is applied.
struct Detected {
    bool stdoutIsTerminal { false };
    bool colorAllowed     { false };  // terminal AND not NO_COLOR/TERM=dumb
};

[[nodiscard]] Detected detect();

// Where a mode preference came from.
//
// It decides whether a degradation is worth announcing, and the distinction is
// not cosmetic. `--ui-mode tui` typed on THIS invocation is a demand: silently
// doing something else is how a user concludes the flag does nothing. A stored
// `"uiMode": "tui"` is a preference, and piping the output of a command is the
// everyday way to not honour it -- announcing that on every `xlings list |
// grep foo` would be a line of noise per invocation, forever.
enum class PreferenceOrigin { Flag, Config };

// Resolve mode from preference + environment.
//
// `preferred` is nullopt for "auto". Returns the mode AND, when a DEMAND could
// not be met, a reason.
struct Resolution {
    UiMode mode { UiMode::Cli };
    std::string degradedReason;   // empty when nothing worth announcing happened
};

[[nodiscard]] Resolution resolve(std::optional<UiMode> preferred,
                                 PreferenceOrigin origin,
                                 bool agentMode,
                                 const Detected& env);

[[nodiscard]] UiCapabilities capabilities_of(UiMode mode,
                                             bool interactivePreference,
                                             bool agentMode,
                                             const Detected& env);

// Process-wide answer, set once during startup.
void set_current(UiMode mode, UiCapabilities caps);

[[nodiscard]] UiMode current_mode();

[[nodiscard]] const UiCapabilities& current_capabilities();

}  // namespace xlings::ui
