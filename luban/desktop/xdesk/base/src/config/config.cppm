// xdesk.config: a desktop's settings, as the freedesktop way keeps them --
// a profile's defaults as data, $XDG_DATA_DIRS/xdesk/<profile>.ini (what a
// config: package places in an environment's share), a system's in
// $XDG_CONFIG_DIRS/xdesk/<profile>.ini, and a person's own in
// $XDG_CONFIG_HOME/xdesk/<profile>.ini -- each later one winning key by key.
//
// The files are INI: `[section]`, `key = value`, `#` or `;` comments; a key
// may repeat (autostart). What a file gets wrong is said, by line, and
// skipped -- a typo never keeps the desktop from starting.
export module xdesk.config;

import std;

export namespace xdesk::config {

namespace fs = std::filesystem;

class Settings {
public:
    // The last value of section.key, or `fallback`.
    std::string get(std::string_view section, std::string_view key, std::string_view fallback = {}) const;
    int get_int(std::string_view section, std::string_view key, int fallback) const;
    // Every value of a key that repeats, in order (a later file's replace
    // an earlier's: a person's autostart list is theirs).
    std::vector<std::string> all(std::string_view section, std::string_view key) const;
    // A colour "#rrggbb": 0xrrggbb, or `fallback`.
    std::uint32_t colour(std::string_view section, std::string_view key, std::uint32_t fallback) const;

    // Laid over what is there: each key it sets replaces the key here.
    void merge(const Settings& over);
    std::vector<std::string> problems;      // "file:line: what is wrong"

    static Settings parse(std::string_view text, std::string_view origin = "settings");

private:
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> values_;
};

// Where a profile's files are, read in this order (the person's last):
// $XDG_DATA_DIRS (or /usr/local/share:/usr/share), $XDG_CONFIG_DIRS (or
// /etc/xdg), then $XDG_CONFIG_HOME (or ~/.config).
std::vector<fs::path> files(std::string_view profile, const std::map<std::string, std::string>& env);
// Every file there, read in order and merged.
Settings load(std::string_view profile, const std::map<std::string, std::string>& env);

}  // namespace xdesk::config
