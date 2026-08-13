module xlings.core.utf8;

import std;
import xlings.libs.json;

namespace xlings::utf8 {

auto safe_truncate(std::string_view s, std::size_t max_bytes, std::string_view suffix) -> std::string {
    if (s.size() <= max_bytes) return std::string(s);
    if (max_bytes < suffix.size()) return std::string(suffix.substr(0, max_bytes));

    std::size_t cut = max_bytes - suffix.size();
    // Back up past any UTF-8 continuation bytes (10xxxxxx)
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string(s.substr(0, cut)) + std::string(suffix);
}

auto safe_dump(const nlohmann::json& j, int indent) -> std::string {
    return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

}
