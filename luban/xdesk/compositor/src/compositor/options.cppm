// What a compositor session is started with (xdesk.compositor's Options): a
// partition of its own so the server's parts see it (server.cppm).
export module xdesk.compositor:options;

import std;
import opensal;

export namespace xdesk::compositor {

struct Options {
    int in { 0 };                                // opensal.wire from the provider
    int out { 1 };                               // and to it
    opensal::Size size { 1280, 800 };            // until the provider says
    std::string profile { "classic" };
    // Super/Alt+Return, and what runs a Terminal=true application. Empty: the
    // default terminal (share/xdg-terminals, xdesk.apps::default_terminal).
    std::vector<std::string> terminal;
    std::vector<std::vector<std::string>> autostart;           // started once the socket is up
    int inner_gap { 4 };                         // between windows (design 2026-10-20 §7.2)
    int outer_gap { 0 };                         // between them and the output's edges
    int min_column_width { 480 };                // tiled columns scroll below it (§7.3)
    std::string flow { "horizontal" };           // tiled columns side by side, or "vertical"
    bool minimap { true };                       // the canvas's minimap (design 2026-10-20 §9.6)
    bool snap { true };                          // a moved window snaps to others' edges (§9.4)
    int workspaces { 4 };
    std::optional<std::uint32_t> accent;         // 0xrrggbb: the theme's, when absent
    std::optional<std::uint32_t> background;
    std::string keyboard_layout { "us" };        // an xkb layout (others need xkeyboard-config)
    int repeat_rate { 30 }, repeat_delay { 500 };
    std::function<void(std::string_view)> log;
};

}  // namespace xdesk::compositor
