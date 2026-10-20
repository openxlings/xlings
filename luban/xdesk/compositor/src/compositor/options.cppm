// What a compositor session is started with (xdesk.compositor's Options): a
// partition of its own so the server's parts see it (server.cppm).
export module xdesk.compositor:options;

import std;
import opensal;

export namespace xdesk::compositor {

struct Options {
    int in { 0 };                                // opensal.wire from the provider
    int out { 1 };                               // and to it
    // Where other windows of the desktop connect, each a display mirroring
    // the first (design 2026-10-20 §11; OPENSAL_DISPLAYS). Empty: none.
    std::filesystem::path displays;
    opensal::Size size { 1280, 800 };            // until the provider says
    std::string profile { "classic" };
    std::string config_profile { "xdesk" };      // the settings' file: xdesk.ini, whatever the session
    // Super/Alt+Return, and what runs a Terminal=true application. Empty: the
    // default terminal (share/xdg-terminals, xdesk.apps::default_terminal).
    std::vector<std::string> terminal;
    std::vector<std::vector<std::string>> autostart;           // started once the socket is up
    int inner_gap { 4 };                         // between windows (design 2026-10-20 §7.2)
    int outer_gap { 0 };                         // between them and the output's edges
    int min_column_width { 480 };                // tiled columns scroll below it (§7.3)
    std::string flow { "horizontal" };           // tiled columns side by side, or "vertical"
    bool minimap { true };                       // the WuXian view's minimap (design 2026-10-20 §9.6)
    bool snap { true };                          // a moved window snaps to others' edges (§9.4)
    bool reduce_motion { false };                // windows go at once, not by springs (§11.4)
    // The switchers and the wheel (design 2026-10-20 WuXian §4).
    int switcher_hold_ms { 500 };                // held on an application this long: its windows
    int highlight_ms { 500 };                    // the one chosen stays lit this long (0: not at all)
    double dim { 0.5 };                          // what the others are dimmed to meanwhile
    double wheel_speed { 1 };                    // slow 0.6, medium 1, fast 1.6
    bool snap_columns { true };                  // a tiled strip's scroll lands on a column
    bool status_others { true };                 // the status bar shows the other workspaces (design 2026-10-20 WuXian §3.13)
    std::string rule { "tile" };                 // a new workspace's: tile or stack (design 2026-10-20 WuXian §5.4)
    std::optional<std::uint32_t> accent;         // 0xrrggbb: the theme's, when absent
    std::optional<std::uint32_t> background;
    std::string keyboard_layout { "us" };        // an xkb layout (others need xkeyboard-config)
    int repeat_rate { 30 }, repeat_delay { 500 };
    std::function<void(std::string_view)> log;
};

}  // namespace xdesk::compositor
