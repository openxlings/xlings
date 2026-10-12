// An environment's machine settings (Luban OS architecture design §6, §10,
// §14.4): how it runs, what a session keeps, which desktop it starts and what
// closing its window does. `<home>/config/subos/<name>/machine.json`, Luban's
// own file beside xlings's instance.json -- xlings never reads it, and a key
// this luban does not know survives a write. The login shell is not here: the
// environment's passwd answers that (xlings.subos.root_etc).
export module luban.service.settings;

import std;
import xlings.libs.json;

export namespace luban::service {

namespace fs = std::filesystem;

struct Settings {
    std::string mode { "sandbox" };      // sandbox | machine
    std::string persist { "all" };       // all | home | none
    std::string desktop;                 // a session name; empty: the edition's (or none)
    std::string on_close { "ask" };      // ask | background | poweroff
    nlohmann::json document = nlohmann::json::object();   // everything as read
};

fs::path settings_file(const fs::path& home, std::string_view name);

// Absent is the defaults; unreadable is an error (never a reason to boot a
// machine some other way than it was set up to).
std::expected<Settings, std::string> read_settings(const fs::path& home, std::string_view name);

// One key, checked: mode, persist, desktop, on-close. The file is rewritten
// atomically with every other key kept.
std::expected<Settings, std::string> set_setting(const fs::path& home, std::string_view name,
                                                 std::string_view key, std::string_view value);

// The values a key takes (empty: any session name).
std::vector<std::string_view> setting_values(std::string_view key);

// The environment's root tree (`/` inside it), or nothing for a view.
std::optional<fs::path> root_of(const fs::path& home, std::string_view name);

}  // namespace luban::service
