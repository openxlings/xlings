// xlings.core.mirror.registry — load + cache the mirror list.
//
// Resolution order (first hit wins):
//   1. ~/.xlings/data/github-mirrors.json (user override; full replacement)
//   2. compiled-in DEFAULT_MIRRORS_JSON
//
// Loaded once on first call to all() / filtered(), then memoized for the
// process lifetime. Failure to parse a user override is logged at warn
// level and the default list is used instead.

module xlings.core.mirror.registry;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.libs.json;
import xlings.platform;
import xlings.core.mirror.types;

namespace xlings::mirror {

namespace {

// Default mirror list. This is an embedded resource — updates require a
// release. See docs/plans/2026-05-01-mirror-fallback-step1.md for the
// rationale (rejecting remote hot-update because of chicken-and-egg).
//
// Ordering by priority (ascending = tried first):
//   jsdelivr     priority  5   raw-only, very stable
//   ghfast       priority 10   active 2025
//   ghproxy-net  priority 20   active 2025
//   kkgithub     priority 30   host-replace style fallback
constexpr std::string_view DEFAULT_MIRRORS_JSON = R"JSON({
  "version": 1,
  "mirrors": [
    {
      "name": "jsdelivr",
      "form": "jsdelivr",
      "host": "cdn.jsdelivr.net",
      "supports": ["raw"],
      "limit_bytes": 52428800,
      "priority": 5
    },
    {
      "name": "ghfast",
      "form": "prefix",
      "host": "ghfast.top",
      "supports": ["release", "raw", "archive", "git"],
      "priority": 10
    },
    {
      "name": "ghproxy-net",
      "form": "prefix",
      "host": "ghproxy.net",
      "supports": ["release", "raw", "archive", "git"],
      "priority": 20
    },
    {
      "name": "kkgithub",
      "form": "host-replace",
      "host": "kkgithub.com",
      "supports": ["release", "raw", "archive", "git"],
      "priority": 30
    }
  ]
})JSON";

std::optional<Form> parse_form_(std::string_view s) {
    if (s == "prefix")       return Form::Prefix;
    if (s == "host-replace") return Form::HostReplace;
    if (s == "jsdelivr")     return Form::JsDelivr;
    return std::nullopt;
}

std::optional<ResourceType> parse_resource_type_(std::string_view s) {
    if (s == "release") return ResourceType::Release;
    if (s == "raw")     return ResourceType::Raw;
    if (s == "archive") return ResourceType::Archive;
    if (s == "git")     return ResourceType::Git;
    return std::nullopt;
}

// Parse a mirrors JSON document into a vector of Mirror. Returns nullopt
// on any parse error so the caller can fall back to defaults.
std::optional<std::vector<Mirror>> parse_mirrors_json_(std::string_view json_text) {
    auto root = nlohmann::json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return std::nullopt;

    auto mirrors_node = root.find("mirrors");
    if (mirrors_node == root.end() || !mirrors_node->is_array()) return std::nullopt;

    std::vector<Mirror> out;
    out.reserve(mirrors_node->size());

    for (const auto& m : *mirrors_node) {
        if (!m.is_object()) continue;
        Mirror mir;

        if (auto it = m.find("name"); it != m.end() && it->is_string())
            mir.name = it->get<std::string>();
        if (auto it = m.find("host"); it != m.end() && it->is_string())
            mir.host = it->get<std::string>();
        if (auto it = m.find("priority"); it != m.end() && it->is_number_integer())
            mir.priority = it->get<int>();

        auto form_it = m.find("form");
        if (form_it == m.end() || !form_it->is_string()) continue;
        auto form = parse_form_(form_it->get<std::string>());
        if (!form) continue;
        mir.form = *form;

        if (auto it = m.find("supports"); it != m.end() && it->is_array()) {
            for (const auto& s : *it) {
                if (!s.is_string()) continue;
                if (auto rt = parse_resource_type_(s.get<std::string>()))
                    mir.supports.push_back(*rt);
            }
        }
        if (auto it = m.find("limit_bytes"); it != m.end() && it->is_number_unsigned())
            mir.limit_bytes = it->get<std::size_t>();

        if (mir.host.empty() || mir.name.empty() || mir.supports.empty())
            continue;
        out.push_back(std::move(mir));
    }

    // Stable sort ascending by priority for deterministic iteration.
    std::ranges::stable_sort(out, [](const Mirror& a, const Mirror& b) {
        return a.priority < b.priority;
    });
    return out;
}

// Load the effective mirror list once, memoize for process lifetime.
const std::vector<Mirror>& load_once_() {
    static const std::vector<Mirror> mirrors = [] {
        auto& paths = Config::paths();
        auto user_path = paths.dataDir / "github-mirrors.json";

        std::error_code ec;
        if (fs::exists(user_path, ec)) {
            auto content = platform::read_file_to_string(user_path.string());
            if (auto parsed = parse_mirrors_json_(content)) {
                log::debug("[mirror] loaded {} mirrors from {}",
                          parsed->size(), user_path.string());
                return std::move(*parsed);
            }
            log::warn("[mirror] failed to parse {}, falling back to defaults",
                     user_path.string());
        }

        if (auto parsed = parse_mirrors_json_(DEFAULT_MIRRORS_JSON))
            return std::move(*parsed);

        log::warn("[mirror] failed to parse compiled-in defaults; mirror "
                  "fallback disabled");
        return std::vector<Mirror>{};
    }();
    return mirrors;
}

}

std::span<const Mirror> all() {
    return load_once_();
}

std::vector<Mirror> filtered(ResourceType type, std::size_t expected_size) {
    std::vector<Mirror> out;
    for (const auto& m : load_once_()) {
        if (std::ranges::find(m.supports, type) == m.supports.end()) continue;
        if (expected_size > 0 && m.limit_bytes && expected_size > *m.limit_bytes)
            continue;
        out.push_back(m);
    }
    return out;
}

}
