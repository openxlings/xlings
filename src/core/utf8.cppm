export module xlings.core.utf8;

import std;
import xlings.libs.json;

namespace xlings::utf8 {

// UTF-8 safe truncation: truncates at character boundary, never cuts multi-byte characters
export auto safe_truncate(std::string_view s, std::size_t max_bytes,
                          std::string_view suffix = "...") -> std::string;

// Safe JSON dump using error_handler_t::replace for invalid UTF-8
export auto safe_dump(const nlohmann::json& j, int indent = -1) -> std::string;

} // namespace xlings::utf8
