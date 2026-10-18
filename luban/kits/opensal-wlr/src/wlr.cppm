// opensal-wlr: a wlroots compositor's output as an opensal surface.
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
//   shell     zwlr_foreign_toplevel_manager_v1 (the windows, listed and
//             brought forward) and programs started with the session's
//             environment -- for a session without a shell of its own
//
// A desktop session is what freedesktop already declares:
// share/wayland-sessions/<id>.desktop, `Exec=` the compositor, and one key
// of opensal's -- `X-Opensal-Interfaces=` the interfaces it uses. A session
// that speaks opensal.wire itself (xdesk) says `X-Opensal-Native=true`: it is
// given the stream, and nothing is bridged. A session whose shell is the
// window's (`X-Luban-Shell=none`, Luban's desktop convention) uses `shell`.
export module opensal.wlr;

import std;
import opensal;
export import opensal.keys;

export namespace opensal::wlr {

namespace fs = std::filesystem;

struct Session {
    std::string id;                 // the file's name without .desktop
    std::string name;
    std::string exec;
    opensal::Interfaces interfaces { 0 };
    bool native { false };          // speaks opensal.wire on its stdin/stdout
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

// An application its session lists (share/applications/<id>.desktop), for a
// shell that is the provider's: what it shows and what starting it runs.
struct Application {
    std::string id;
    std::string name;
    std::vector<std::string> argv;  // Exec, its field codes dropped
    bool terminal { false };        // Terminal=true: run in the default terminal
};
std::optional<Application> parse_application(std::string_view text, std::string id);
// Exec's words (the desktop entry specification's quoting), field codes dropped.
std::vector<std::string> exec_argv(std::string_view exec);
// $XDG_DATA_HOME's (or ~/.local/share's), then as session_dirs, each /applications.
std::vector<fs::path> application_dirs(const std::map<std::string, std::string>& env);
std::vector<Application> applications(const std::vector<fs::path>& dirs);
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

struct Options {
    int in { 0 };                   // opensal.wire from the provider
    int out { 1 };                  // and to it
    fs::path socket;                // the compositor's
    std::function<bool()> alive;    // whether the compositor still runs
    int max_fps { 30 };
    std::function<void(std::string_view)> log;
    // Shell, when the session has none of its own: what a launched program
    // is started with (the session's, WAYLAND_DISPLAY its compositor).
    bool shell { false };
    std::map<std::string, std::string> env;
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

// A session as a headless desktop on `in` / `out` -- given them when it is
// native, bridged otherwise: the compositor
// (wlroots' headless backend, software rendering) in a runtime directory of
// its own, stopped when the bridge ends. What a machine's agent runs for the
// Luban OS window, and what a sandbox runs for it.
int run_session(const Session& s, int in, int out, const SessionOptions& o);

}  // namespace opensal::wlr
