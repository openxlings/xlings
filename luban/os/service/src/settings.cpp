module luban.service.settings;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.located;
import xlings.subos.rootfs;

namespace luban::service {

namespace {

const std::map<std::string_view, std::vector<std::string_view>, std::less<>>& allowed_() {
    static const std::map<std::string_view, std::vector<std::string_view>, std::less<>> value{
        {"mode", {"sandbox", "machine"}},
        {"persist", {"all", "home", "none"}},
        {"desktop", {}},
        {"on-close", {"ask", "background", "poweroff"}},
    };
    return value;
}

std::string json_key_(std::string_view key) { return key == "on-close" ? "on_close" : std::string(key); }

}  // namespace

fs::path settings_file(const fs::path& home, std::string_view name) {
    return xlings::subos::HomeView{home}.config_dir(name) / "machine.json";
}

std::expected<Settings, std::string> read_settings(const fs::path& home, std::string_view name) {
    Settings s;
    // The edition's defaults first (its root carries them, so they follow an
    // upgrade); a file that cannot be read is no defaults, never an error here.
    if (const auto root = root_of(home, name)) {
        const auto defaults = *root / "usr" / "share" / "luban" / "settings.json";
        std::error_code dec;
        if (fs::is_regular_file(defaults, dec)) {
            auto d = nlohmann::json::parse(xlings::platform::read_file_to_string(defaults.string()), nullptr, false);
            if (d.is_object()) {
                for (const auto& [key, values] : allowed_()) {
                    const auto k = json_key_(key);
                    if (!d.contains(k) || !d[k].is_string()) continue;
                    const auto v = d[k].get<std::string>();
                    if (!values.empty() && std::ranges::find(values, v) == values.end()) continue;
                    if (k == "mode") s.mode = v;
                    else if (k == "persist") s.persist = v;
                    else if (k == "desktop") s.desktop = v;
                    else if (k == "on_close") s.on_close = v;
                }
            }
        }
    }
    const auto file = settings_file(home, name);
    std::error_code ec;
    if (!fs::exists(file, ec)) return s;
    auto j = nlohmann::json::parse(xlings::platform::read_file_to_string(file.string()), nullptr, false);
    if (!j.is_object())
        return std::unexpected(std::format("{} cannot be read; fix it or remove it to go back to the defaults",
                                           file.string()));
    s.document = j;
    s.mode = j.value("mode", s.mode);
    s.persist = j.value("persist", s.persist);
    s.desktop = j.value("desktop", s.desktop);
    s.on_close = j.value("on_close", s.on_close);
    return s;
}

std::vector<std::string_view> setting_values(std::string_view key) {
    auto it = allowed_().find(key);
    return it == allowed_().end() ? std::vector<std::string_view>{} : it->second;
}

std::expected<Settings, std::string> set_setting(const fs::path& home, std::string_view name,
                                                 std::string_view key, std::string_view value) {
    auto it = allowed_().find(key);
    if (it == allowed_().end()) return std::unexpected(std::format("no machine setting '{}'", key));
    if (!it->second.empty() && std::ranges::find(it->second, value) == it->second.end()) {
        std::string list;
        for (auto v : it->second) list += (list.empty() ? "" : ", ") + std::string(v);
        return std::unexpected(std::format("--{} {}: one of {}", key, value, list));
    }
    if (key == "desktop" && value.find_first_of("/\\ ") != std::string_view::npos)
        return std::unexpected(std::format("--desktop {}: a session's name, or none", value));
    auto current = read_settings(home, name);
    if (!current) return std::unexpected(current.error());
    auto doc = current->document;
    doc[json_key_(key)] = std::string(value);
    const auto file = settings_file(home, name);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    xlings::platform::write_file_atomic(file.string(), doc.dump(2) + "\n");
    return read_settings(home, name);
}

std::optional<fs::path> root_of(const fs::path& home, std::string_view name) {
    const auto at = xlings::subos::locate(xlings::subos::HomeView{home}, name);
    const auto tree = at.home.instance(name) / std::string(xlings::subos::rootfs::kTree);
    std::error_code ec;
    if (fs::is_directory(tree / "etc", ec)) return tree;
    return std::nullopt;
}

}  // namespace luban::service
