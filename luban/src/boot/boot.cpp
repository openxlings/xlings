module luban.boot;

import std;
import xlings.libs.json;
import xlings.platform;

namespace luban::boot {

namespace platform = xlings::platform;

namespace {

bool valid_name(std::string_view name) {
    return std::ranges::all_of(name, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

std::expected<void, std::string> validate(const nlohmann::json& j) {
    auto name_field = [&](const nlohmann::json& object, std::string_view key,
                          bool nullable = false) {
        auto it = object.find(std::string(key));
        return it == object.end() || (nullable && it->is_null())
            || (it->is_string() && valid_name(it->get<std::string>()));
    };
    if (!j.is_object()) return std::unexpected("expected a JSON object");
    if (!name_field(j, "default") || !name_field(j, "fallback") || !name_field(j, "once", true))
        return std::unexpected("boot entries must be SubOS names (alphanumeric, underscore, dash)");
    if (auto tries = j.find("tries"); tries != j.end()) {
        if (!tries->is_object()) return std::unexpected("tries must be an object");
        for (auto it = tries->begin(); it != tries->end(); ++it) {
            if (it.key().empty() || !valid_name(it.key()) || !it->is_number_integer()
                || *it < 0 || *it > std::numeric_limits<int>::max())
                return std::unexpected("tries must map SubOS names to nonnegative integers");
        }
    }
    if (auto last = j.find("last"); last != j.end()) {
        if (!last->is_object() || !name_field(*last, "subos"))
            return std::unexpected("last must be an object naming a SubOS");
        if (auto good = last->find("good"); good != last->end() && !good->is_boolean())
            return std::unexpected("last.good must be a boolean");
        if (auto via = last->find("via"); via != last->end()) {
            if (!via->is_string()) return std::unexpected("last.via must be a string");
            const auto text = via->get<std::string>();
            if (!text.empty() && text != "once" && text != "default" && text != "fallback")
                return std::unexpected("last.via must be once, default or fallback");
        }
    }
    return {};
}

std::expected<nlohmann::json, std::string> read_document(const fs::path& file) {
    std::error_code ec;
    const auto status = fs::symlink_status(file, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected(file.string() + ": cannot be inspected: " + ec.message());
    if (status.type() == fs::file_type::not_found) return nlohmann::json::object();
    if (!fs::is_regular_file(file, ec) || ec)
        return std::unexpected(file.string() + ": is not a readable regular file");
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(file.string() + ": cannot be read");
    std::string bytes;
    try { bytes.assign(std::istreambuf_iterator<char>(in), {}); }
    catch (const std::exception& e) { return std::unexpected(file.string() + ": " + e.what()); }
    auto j = nlohmann::json::parse(bytes, nullptr, false);
    if (in.bad() || j.is_discarded())
        return std::unexpected(file.string() + ": is not a boot configuration");
    if (auto ok = validate(j); !ok) return std::unexpected(file.string() + ": " + ok.error());
    return j;
}

}  // namespace

Config from_json(const nlohmann::json& j) {
    Config c;
    if (!j.is_object()) return c;
    c.document = j;
    c.default_entry = j.value("default", c.default_entry);
    c.fallback = j.value("fallback", c.fallback);
    if (auto it = j.find("once"); it != j.end() && it->is_string() && !it->get<std::string>().empty())
        c.once = it->get<std::string>();
    if (auto it = j.find("tries"); it != j.end() && it->is_object())
        for (auto e = it->begin(); e != it->end(); ++e)
            if (e.value().is_number_integer()) c.tries[e.key()] = e.value().get<int>();
    if (auto it = j.find("last"); it != j.end() && it->is_object()) {
        c.booted = it->value("subos", "");
        c.via = it->value("via", "");
        c.good = it->value("good", false);
    }
    return c;
}

nlohmann::json to_json(const Config& c) {
    nlohmann::json j = c.document.is_object() ? c.document : nlohmann::json::object();
    j["default"] = c.default_entry;
    j["fallback"] = c.fallback;
    j["once"] = c.once ? nlohmann::json(*c.once) : nlohmann::json(nullptr);
    j["tries"] = nlohmann::json::object();
    for (auto& [k, v] : c.tries) j["tries"][k] = v;
    if (!c.booted.empty()) {
        if (!j.contains("last") || !j["last"].is_object()) j["last"] = nlohmann::json::object();
        j["last"]["subos"] = c.booted;
        j["last"]["via"] = c.via;
        j["last"]["good"] = c.good;
    } else if (j.contains("last")) {
        for (auto key : {"subos", "via", "good"}) j["last"].erase(key);
        if (j["last"].empty()) j.erase("last");
    }
    return j;
}

std::expected<Config, std::string> load(const fs::path& file) {
    auto j = read_document(file);
    if (!j) return std::unexpected(j.error());
    return from_json(*j);
}

std::expected<void, std::string> save(const fs::path& file, const Config& c) {
    auto document = read_document(file);
    if (!document) return std::unexpected(document.error());
    auto next = c;
    next.document = *document;
    const auto json = to_json(next);
    if (auto ok = validate(json); !ok) return std::unexpected(file.string() + ": " + ok.error());
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    if (ec) return std::unexpected(file.string() + ": " + ec.message());
    try {
        platform::write_file_atomic(file.string(), json.dump(2) + "\n");
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    }
    return {};
}

std::vector<Candidate> candidates(const Config& c,
                                  const std::function<bool(std::string_view)>& bootable) {
    std::vector<Candidate> out;
    auto add = [&](const std::string& name, std::string via) {
        if (name.empty() || !bootable(name)) return;
        if (std::ranges::find(out, name, &Candidate::subos) != out.end()) return;
        out.push_back({name, std::move(via)});
    };
    if (c.once) add(*c.once, "once");
    auto left = c.tries.find(c.default_entry);
    if (left == c.tries.end() || left->second > 0) add(c.default_entry, "default");
    add(c.fallback, "fallback");
    return out;
}

Config record_boot(Config c, const Candidate& chosen) {
    if (chosen.via == "once") c.once.reset();
    auto it = c.tries.find(chosen.subos);
    const int left = it == c.tries.end() ? kTries : it->second;
    c.tries[chosen.subos] = std::max(0, left - 1);
    c.booted = chosen.subos;
    c.via = chosen.via;
    c.good = false;
    return c;
}

Config mark_good(Config c) {
    if (c.booted.empty()) return c;
    c.good = true;
    c.tries[c.booted] = kTries;
    return c;
}

}  // namespace luban::boot
