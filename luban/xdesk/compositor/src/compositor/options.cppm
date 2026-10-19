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
    int gap { 8 };                               // between windows and the edges
    int workspaces { 4 };
    std::optional<std::uint32_t> accent;         // 0xrrggbb: the theme's, when absent
    std::optional<std::uint32_t> background;
    std::string keyboard_layout { "us" };        // an xkb layout (others need xkeyboard-config)
    int repeat_rate { 30 }, repeat_delay { 500 };
    std::function<void(std::string_view)> log;
};

}  // namespace xdesk::compositor
