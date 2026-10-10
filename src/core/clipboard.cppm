// The clipboard of a SubOS, without the display (Luban OS design part 2
// §9.2b). Configured, not a command: `xlings subos config <n> --allow
// clipboard[,clipboard-paste]` (luban: `luban config <n> allow clipboard`).
//
// A sandbox granted it finds the names programs already call for a
// clipboard -- wl-copy, wl-paste, xclip, xsel -- first on PATH: links to the
// session's xlings (session-init makes them), which hands the text to the
// broker; the broker, deciding by the grant, runs the hidden `__clipboard`
// outside with the host's own tool. Not granted, nothing is linked: a
// program copies through the terminal (OSC 52), and the terminal pastes.
export module xlings.core.clipboard;

import std;

export namespace xlings::clipboard {

// ESC ] 52 ; c ; <base64> BEL
std::string osc52(std::string_view data);

// The clipboard tool this process was started as, when it is the bridge
// (its name, in a sandbox granted the clipboard).
std::optional<std::string> bridge_tool(std::string_view argv0);

// That tool, as programs call it: copy (stdin, or wl-copy's arguments) or
// paste, through the broker.
int bridge(std::string_view tool, std::span<const std::string> args);

// `xlings __clipboard copy|paste`: the broker's side, on the host.
int host(std::span<const std::string> args);

}  // namespace xlings::clipboard
