module xlings.core.home_health;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.home_config;

namespace xlings::home_health {

namespace fs = std::filesystem;

namespace {

// The fields xlings reads from the home config, the shape each must have,
// and what a user loses while it does not.
struct Field {
    std::string_view name;
    nlohmann::json::value_t type;
    std::string_view consequence;
};

constexpr Field kFields[] = {
    {"versions", nlohmann::json::value_t::object,
     "installed packages are not registered: commands resolve as if nothing were installed"},
    {"subos", nlohmann::json::value_t::object, "no SubOS is registered"},
    {"activeSubos", nlohmann::json::value_t::string, "the default SubOS is used"},
    {"index_repos", nlohmann::json::value_t::array, "only the default index is used"},
    {"mirror", nlohmann::json::value_t::string, "the default mirror is used"},
    {"knownProjects", nlohmann::json::value_t::object, "projects' commands are not routed"},
    {"hintsSeen", nlohmann::json::value_t::array, "one-time notes are shown again"},
    {"lang", nlohmann::json::value_t::string, "the default language is used"},
    {"theme", nlohmann::json::value_t::string, "the default theme is used"},
};

bool same_kind(const nlohmann::json& value, nlohmann::json::value_t want) {
    if (want == nlohmann::json::value_t::object) return value.is_object();
    if (want == nlohmann::json::value_t::array) return value.is_array();
    if (want == nlohmann::json::value_t::string) return value.is_string();
    return true;
}

std::optional<nlohmann::json> parse_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    try {
        auto doc = nlohmann::json::parse(platform::read_file_to_string(path.string()), nullptr, false);
        if (doc.is_discarded()) return nlohmann::json(nlohmann::json::value_t::discarded);
        return doc;
    } catch (...) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
}

std::string stamp_now() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(now));
}

// What `data/versions.json` (the derived copy save_versions writes) holds,
// when it holds a versions map.
std::optional<nlohmann::json> versions_from_db(const fs::path& home) {
    auto db = parse_file(home / "data" / "versions.json");
    if (!db || !db->is_object()) return std::nullopt;
    auto it = db->find("versions");
    if (it == db->end() || !it->is_object()) return std::nullopt;
    return *it;
}

// Every SubOS directory with a state file, registered as `subos new` does.
nlohmann::json subos_registry(const fs::path& home) {
    nlohmann::json out = nlohmann::json::object();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(home / "subos", ec)) {
        std::error_code fec;
        const auto name = entry.path().filename().string();
        if (name == "current" || entry.is_symlink(fec) || !entry.is_directory(fec)) continue;
        if (fs::is_regular_file(entry.path() / ".xlings.json", fec)) out[name] = {{"dir", ""}};
    }
    return out;
}

}  // namespace

std::vector<Problem> problems(const fs::path& home) {
    std::vector<Problem> out;
    auto doc = parse_file(home / ".xlings.json");
    if (!doc) return out;
    if (doc->is_discarded() || !doc->is_object()) {
        out.push_back({"", "is not readable JSON",
                       "settings (mirror, indexes, language), the SubOS registry and the version "
                       "records fall back to defaults; packages and SubOS on disk are untouched"});
        return out;
    }
    for (const auto& f : kFields) {
        auto it = doc->find(std::string(f.name));
        if (it == doc->end() || same_kind(*it, f.type)) continue;
        out.push_back({std::string(f.name),
                       std::format("`{}` is a{} {}, not a{} {}", f.name,
                                   std::string_view("aeiou").find(it->type_name()[0]) != std::string_view::npos ? "n" : "",
                                   it->type_name(),
                                   f.type == nlohmann::json::value_t::object ? "n" : "",
                                   f.type == nlohmann::json::value_t::object ? "object"
                                   : f.type == nlohmann::json::value_t::array ? "array" : "string"),
                       std::string(f.consequence)});
    }
    return out;
}

std::vector<Problem> quick_problems(const fs::path& home) {
    std::vector<Problem> out;
    const auto cap = home_config_capture(home / ".xlings.json", HomeCaptureMode::SkipVersions);
    if (!cap) return out;
    if (!cap->ok) return problems(home);
    for (const auto& f : kFields) {
        if (f.name == "versions") continue;
        auto it = cap->json.find(std::string(f.name));
        if (it != cap->json.end() && !same_kind(*it, f.type)) return problems(home);
    }
    return out;
}

std::string summary(const fs::path& home, const std::vector<Problem>& found) {
    std::string what;
    for (const auto& p : found) {
        if (!what.empty()) what += "; ";
        what += p.field.empty() ? p.what : p.what + " (" + p.consequence + ")";
    }
    const auto& first = found.front();
    return std::format("{} {}{}; fix: xlings self doctor --fix (keeps the file as it is "
                       "beside it)", (home / ".xlings.json").string(), what,
                       first.field.empty() ? " -- " + first.consequence : "");
}

std::expected<Repaired, std::string> repair(const fs::path& home) {
    const auto found = problems(home);
    if (found.empty()) return Repaired{};
    const auto path = home / ".xlings.json";
    Repaired out;
    out.kept = home / (".xlings.json.corrupt-" + stamp_now());
    std::error_code ec;
    fs::copy_file(path, out.kept, fs::copy_options::overwrite_existing, ec);
    if (ec) return std::unexpected("cannot keep a copy of " + path.string() + ": " + ec.message());

    nlohmann::json doc = nlohmann::json::object();
    const bool whole = found.front().field.empty();
    if (!whole) {
        doc = *parse_file(path);
        for (const auto& p : found) doc.erase(p.field);
    }
    const auto rebuilt = [&](std::string_view field) {
        return whole || std::ranges::any_of(found, [&](const Problem& p) { return p.field == field; });
    };
    if (rebuilt("versions")) {
        if (auto versions = versions_from_db(home)) {
            doc["versions"] = *versions;
            out.notes.push_back(std::format("version records: {} program(s), from data/versions.json",
                                            versions->size()));
        } else {
            doc["versions"] = nlohmann::json::object();
            out.notes.push_back("version records: none could be recovered; `xlings install` of a "
                                "package registers it again (its payload is reused)");
        }
    }
    if (rebuilt("subos")) {
        doc["subos"] = subos_registry(home);
        out.notes.push_back(std::format("SubOS registry: {} SubOS, from subos/", doc["subos"].size()));
    }
    if (whole) {
        doc["activeSubos"] = "default";
        out.notes.push_back(std::format("settings (mirror, indexes, language) are defaults; the "
                                        "old ones are in {}", out.kept.filename().string()));
    }
    try {
        platform::write_file_atomic(path.string(), doc.dump(2) + "\n");
    } catch (const std::exception& e) {
        return std::unexpected(std::string("cannot write ") + path.string() + ": " + e.what());
    }
    return out;
}

}  // namespace xlings::home_health
