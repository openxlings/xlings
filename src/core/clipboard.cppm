// `xlings clipboard copy|paste`: the clipboard, without the display.
//
// On a machine: its own clipboard tool (wl-copy / xclip / xsel, pbcopy,
// clip.exe) from the one tools table; with none (no display, an SSH
// session), copy goes through the terminal as OSC 52 -- the terminal puts
// it on the clipboard of the machine the person is sitting at.
//
// Inside a SubOS sandbox: through the broker, decided by the instance's
// policy -- `clipboard` (copy into the host's clipboard) and
// `clipboard-paste` (read it) are grants of their own, so a private
// environment never needs the whole display for this. Without the grant,
// copy still works through the terminal (the bytes pass the session's
// terminal relay as they are), and says how to get the host's.
export module xlings.core.clipboard;

import std;

export namespace xlings::clipboard {

// ESC ] 52 ; c ; <base64> BEL
std::string osc52(std::string_view data);

// `clipboard <copy|paste>` here, on this machine.
int run(std::span<const std::string> args);

// `clipboard copy` from inside a sandbox: the broker, else the terminal.
int copy_inside(std::span<const std::string> argv, std::string_view instance);

}  // namespace xlings::clipboard
