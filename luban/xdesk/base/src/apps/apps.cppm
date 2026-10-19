// xdesk.apps: the applications a desktop offers, as freedesktop declares
// them -- share/applications/*.desktop -- found, searched as a person types,
// and started (Exec= into argv). No drawing: a launcher draws what this
// answers.
export module xdesk.apps;

import std;

export namespace xdesk::apps {

namespace fs = std::filesystem;

struct App {
    std::string id;             // the file's name without .desktop
    std::string name;           // Name[<lang>] if there is one, else Name
    std::string generic;        // GenericName
    std::string comment;
    std::string keywords;       // Keywords, ';' between them
    std::string exec;
    std::string icon;
    bool terminal { false };    // run in a terminal
};

// A desktop entry's [Desktop Entry]; nullopt for what is not to be shown
// (Hidden, NoDisplay, no Exec, a Type other than Application). `lang` picks
// localized keys ("zh_CN" matches Name[zh_CN], then Name[zh]).
std::optional<App> parse(std::string_view text, std::string id, std::string_view lang = {});

// Where they are, most specific first: $XDG_DATA_HOME (or
// ~/.local/share), then $XDG_DATA_DIRS (or /usr/local/share:/usr/share).
std::vector<fs::path> dirs(const std::map<std::string, std::string>& env);
// Every application there, an id once (the more specific wins), by name.
std::vector<App> scan(const std::vector<fs::path>& dirs, std::string_view lang = {});

// The applications that match `query`, best first: the name starting with
// it, then a word of the name, then anywhere in the name, then in the
// generic name, keywords or comment; case does not matter. An empty query
// is every application by name.
std::vector<const App*> search(const std::vector<App>& apps, std::string_view query);

// Exec= as argv: quoting as the specification has it, field codes (%f %U
// ...) removed, %% a percent sign.
std::vector<std::string> argv(std::string_view exec);

// The default terminal, as the proposed freedesktop convention declares it
// (xdg-terminal-exec): share/xdg-terminals/<id>.desktop in the data dirs,
// chosen by the first id an xdg-terminals.list names (the desktop's own
// <desktop>-xdg-terminals.list first, from $XDG_CURRENT_DESKTOP), else the
// first entry there. Its X-TerminalArg* keys say how it takes a command, a
// title and an application id.
struct Terminal {
    std::vector<std::string> exec;   // Exec=, as argv
    std::optional<std::string> arg_exec, arg_title, arg_app_id;   // absent: not declared
};
std::optional<Terminal> default_terminal(const std::map<std::string, std::string>& env);
// argv running `command` in `t`, its window titled `title` and identified as
// `app_id` where the terminal can take them (nothing given: just the terminal).
std::vector<std::string> in_terminal(const Terminal& t, const std::vector<std::string>& command,
                                     std::string_view title = {}, std::string_view app_id = {});

}  // namespace xdesk::apps
