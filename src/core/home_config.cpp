module xlings.core.home_config;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.xvm.lock;

namespace xlings {

std::filesystem::path home_config_path(const std::filesystem::path& home) {
    return home / ".xlings.json";
}

nlohmann::json read_home_config(const std::filesystem::path& home) {
    const auto path = home_config_path(home);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return nlohmann::json::object();
    try {
        auto content = platform::read_file_to_string(path.string());
        auto parsed = nlohmann::json::parse(content, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return nlohmann::json::object();
        }
        return parsed;
    } catch (...) {
        return nlohmann::json::object();
    }
}

std::expected<bool, std::string> update_home_config(const std::filesystem::path& home, const std::function<bool(nlohmann::json&)>& mutate, std::chrono::milliseconds timeout) {
    auto lock = xvm::acquire_state_lock(home, timeout);
    if (!lock) return std::unexpected(lock.error());

    auto json = read_home_config(home);
    if (!mutate(json)) return false;

    try {
        platform::write_string_to_file(
            home_config_path(home).string(), json.dump(2));
    } catch (const std::exception& e) {
        return std::unexpected(std::format(
            "failed to write {}: {}",
            home_config_path(home).string(), e.what()));
    }
    return true;
}

}
