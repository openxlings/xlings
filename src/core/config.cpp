module xlings.core.config;

import std;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.libs.tinyhttps;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

namespace xlings {

void capture_ambient_home_env() {
    if (detail_::ambientHomeCaptured_) return;   // first call wins
    detail_::ambientHomeCaptured_ = true;
    if (const char* v = std::getenv("XLINGS_HOME"); v != nullptr && *v != '\0') {
        detail_::ambientHome_ = std::string(v);
    }
}

const std::optional<std::string>& ambient_home_env() {
    return detail_::ambientHome_;
}

std::vector<IndexRepo> parse_index_repos_json(const nlohmann::json& json,
                                                     const std::string& mirror) {
    std::vector<IndexRepo> out;
    if (!json.contains("index_repos") || !json["index_repos"].is_array()) return out;
    for (auto it = json["index_repos"].begin(); it != json["index_repos"].end(); ++it) {
        if (!it->is_object() || !it->contains("name") || !it->contains("url")) continue;
        IndexRepo repo;
        repo.name = (*it)["name"].get<std::string>();
        repo.url  = (*it)["url"].get<std::string>();
        if (repo.name.empty() || repo.url.empty()) continue;
        if (it->contains("artifact")) {
            auto& a = (*it)["artifact"];
            std::string base;
            if (a.is_string()) base = a.get<std::string>();
            else if (a.is_object()) {
                std::string key = mirror.empty() ? "GLOBAL" : mirror;
                if (a.contains(key) && a[key].is_string()) base = a[key].get<std::string>();
                else if (a.contains("GLOBAL") && a["GLOBAL"].is_string())
                    base = a["GLOBAL"].get<std::string>();
            }
            base = utils::trim_string(base);
            while (base.size() > 1 && base.ends_with('/')) base.pop_back();
            repo.artifactBase = base;
        }
        if (it->contains("source") && (*it)["source"].is_string())
            repo.source = (*it)["source"].get<std::string>();
        // Not validated: the version namespace belongs to the index publisher,
        // and xlings should not have an opinion about its shape.
        if (it->contains("version") && (*it)["version"].is_string())
            repo.version = (*it)["version"].get<std::string>();
        out.push_back(std::move(repo));
    }
    return out;
}

}
