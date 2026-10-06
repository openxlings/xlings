module xlings.subos.boot;

import std;
import xlings.libs.json;

namespace xlings::subos::boot {

Config from_json(const nlohmann::json& j) {
    Config c;
    if (!j.is_object()) return c;
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
    nlohmann::json j{{"default", c.default_entry}, {"fallback", c.fallback}};
    j["once"] = c.once ? nlohmann::json(*c.once) : nlohmann::json(nullptr);
    j["tries"] = nlohmann::json::object();
    for (auto& [k, v] : c.tries) j["tries"][k] = v;
    if (!c.booted.empty()) j["last"] = {{"subos", c.booted}, {"via", c.via}, {"good", c.good}};
    return j;
}

std::expected<Config, std::string> load(const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return Config{};
    std::ifstream in(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::unexpected(std::format("{} is not a boot configuration", file.string()));
    return from_json(j);
}

std::expected<void, std::string> save(const fs::path& file, const Config& c) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const auto staged = file.parent_path() / ("." + file.filename().string() + ".new");
    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out << to_json(c).dump(2) << "\n";
        if (!out) return std::unexpected(std::format("cannot write {}", staged.string()));
    }
    fs::rename(staged, file, ec);
    if (ec) return std::unexpected(std::format("cannot replace {}: {}", file.string(), ec.message()));
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

}  // namespace xlings::subos::boot
