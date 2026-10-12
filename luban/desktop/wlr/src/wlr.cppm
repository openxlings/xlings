// opensal-wlr: a wlroots compositor's output as an opensal surface (Luban OS
// architecture design §9.3).
//
// The bridge is an opensal.wire CONSUMER on a byte stream (the provider is
// the Luban OS window, across a machine's virtio-serial or a sandbox's pipe)
// and a Wayland CLIENT of the compositor. Everything it does with the
// compositor is a standard protocol, so any compositor that has them is a
// Luban desktop:
//
//   picture   zwlr_screencopy_manager_v1 (copy_with_damage into wl_shm)
//   keyboard  zwp_virtual_keyboard_manager_v1 (an evdev keymap, US)
//   pointer   zwlr_virtual_pointer_manager_v1 (absolute, on the output)
//   size      zwlr_output_manager_v1 (a custom mode at the window's size;
//             without it the provider is told the size it is drawn at)
//
// A desktop session is what freedesktop already declares:
// share/wayland-sessions/<id>.desktop, `Exec=` the compositor, and one key
// of opensal's -- `X-Opensal-Interfaces=` the interfaces it uses.
export module luban.desktop.wlr;

import std;
import opensal;

export namespace luban::desktop::wlr {

namespace fs = std::filesystem;

struct Session {
    std::string id;                 // the file's name without .desktop
    std::string name;
    std::string exec;
    opensal::Interfaces interfaces { 0 };
    fs::path file;
};

// A session file's [Desktop Entry]; nullopt when it declares no Exec (or is hidden).
std::optional<Session> parse_session(std::string_view text, std::string id);
// "surface pointer keyboard" -> the bits; an unknown name is skipped.
opensal::Interfaces parse_interfaces(std::string_view names);
// Where sessions are declared, most specific first: $XDG_DATA_DIRS's, then
// /usr/local/share and /usr/share.
std::vector<fs::path> session_dirs(const std::map<std::string, std::string>& env);
std::vector<Session> sessions(const std::vector<fs::path>& dirs);
std::optional<Session> find_session(std::string_view id, const std::vector<fs::path>& dirs);

// The virtual keyboard's keymap: evdev keycodes, the US layout, compiled by
// the compositor from its own xkeyboard-config.
inline constexpr std::string_view kKeymap =
    "xkb_keymap {\n"
    "  xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
    "  xkb_types { include \"complete\" };\n"
    "  xkb_compat { include \"complete\" };\n"
    "  xkb_symbols { include \"pc+us+inet(evdev)\" };\n"
    "  xkb_geometry { include \"pc(pc105)\" };\n"
    "};\n";

// An X11 keysym (what opensal.keyboard carries) as the evdev key that types
// it on that keymap, and whether Shift is part of it.
struct Stroke { std::uint32_t code { 0 }; bool shift { false }; };
std::optional<Stroke> evdev_key(std::uint32_t keysym);
// The modifier a keysym is (opensal::Mod bit), or 0.
std::uint32_t modifier_of(std::uint32_t keysym);
// opensal::Mod bits as the keymap's modifier mask (Shift, Control, Mod1, Mod4).
std::uint32_t xkb_mods(std::uint32_t mods);
// BTN_LEFT, BTN_MIDDLE, BTN_RIGHT.
std::uint32_t evdev_button(opensal::Button b);

struct Options {
    int in { 0 };                   // opensal.wire from the provider
    int out { 1 };                  // and to it
    fs::path socket;                // the compositor's
    std::function<bool()> alive;    // whether the compositor still runs
    int max_fps { 30 };
    std::function<void(std::string_view)> log;
};

// Bridges until the provider or the compositor goes away: 0 when the
// provider closed the stream, 1 when the compositor went (or never had
// screencopy), 2 when it could not connect.
int bridge(const Options& o);

struct SessionOptions {
    opensal::Size size { 1280, 800 };       // until the provider says
    std::map<std::string, std::string> env; // the session's (HOME, PATH, LANG, ...)
    fs::path runtime_root { "/run/luban" };
    fs::path log;                           // the compositor's output
};

// A session as a headless desktop, bridged on `in` / `out`: the compositor
// (wlroots' headless backend, software rendering) in a runtime directory of
// its own, stopped when the bridge ends. What a machine's agent runs for the
// Luban OS window, and what a sandbox runs for it.
int run_session(const Session& s, int in, int out, const SessionOptions& o);

}  // namespace luban::desktop::wlr
