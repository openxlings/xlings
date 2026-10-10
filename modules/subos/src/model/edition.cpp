module xlings.subos.edition;

import std;
import xlings.libs.json;

namespace xlings::subos::edition {

std::pair<std::string, std::string> split(std::string_view spec) {
    const auto at = spec.rfind('@');
    if (at == std::string_view::npos) return {std::string(spec), {}};
    return {std::string(spec.substr(0, at)), std::string(spec.substr(at + 1))};
}

std::optional<Record> read(const nlohmann::json& instance) {
    if (!instance.is_object()) return std::nullopt;
    auto it = instance.find("edition");
    if (it == instance.end() || !it->is_object()) return std::nullopt;
    Record r;
    r.ref = it->value("ref", std::string());
    if (r.ref.empty()) return std::nullopt;
    if (auto c = it->find("chain"); c != it->end() && c->is_array())
        for (const auto& v : *c) if (v.is_string()) r.chain.push_back(v.get<std::string>());
    if (auto p = it->find("packages"); p != it->end() && p->is_object())
        for (auto e = p->begin(); e != p->end(); ++e)
            r.packages[e.key()] = e.value().is_string() ? e.value().get<std::string>() : std::string();
    r.policy = it->value("policy", std::string());
    return r;
}

nlohmann::json to_json(const Record& r) {
    nlohmann::json packages = nlohmann::json::object();
    for (const auto& [k, v] : r.packages) packages[k] = v.empty() ? nlohmann::json(nullptr) : nlohmann::json(v);
    nlohmann::json j{{"ref", r.ref}, {"chain", r.chain}, {"packages", packages}};
    if (!r.policy.empty()) j["policy"] = r.policy;
    return j;
}

Plan plan(const Record& now, const std::vector<std::string>& declared,
          const std::vector<std::string>& configured) {
    // What the environment has of each package: every configured version.
    std::map<std::string, std::set<std::string>, std::less<>> has;
    for (const auto& c : configured) {
        auto [key, version] = split(c);
        if (!version.empty()) has[key].insert(version);
    }
    Plan p;
    std::set<std::string, std::less<>> next;
    for (const auto& spec : declared) {
        auto [key, version] = split(spec);
        next.insert(key);
        const auto recorded = now.packages.find(key);
        const auto present = has.find(key);
        const std::string current = present == has.end() || present->second.empty() ? std::string()
                                                                                       : *present->second.rbegin();
        if (recorded == now.packages.end()) {
            // New in the edition -- unless the user already has it, then theirs.
            if (present == has.end()) p.add.push_back({key, "", spec});
            else if (!version.empty() && !present->second.contains(version)) p.kept.push_back({key, current, spec});
            continue;
        }
        // The user moved it when the environment holds a version the edition
        // did not bring (or no longer the one it did).
        const bool moved = present == has.end() || !present->second.contains(recorded->second)
                           || present->second.size() > 1;
        if (moved) { p.kept.push_back({key, current, spec}); continue; }
        // An unpinned package follows its newest; a pinned one, its pin.
        if (version.empty() || version != recorded->second) p.upgrade.push_back({key, recorded->second, spec});
    }
    for (const auto& [key, version] : now.packages)
        if (!next.contains(key)) p.dropped.push_back(key);
    return p;
}

}  // namespace xlings::subos::edition
