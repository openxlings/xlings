module xlings.core.xim.index_cmd;

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.home_config;
import xlings.core.log;
import xlings.core.version_order;
import xlings.core.xim.indexfetch;
import xlings.platform;
import xlings.runtime;

namespace xlings::xim {

std::string installed_index_version(const std::filesystem::path& repoDir) {
    std::error_code ec;
    auto marker = repoDir / ".xlings-index-version";
    if (!std::filesystem::exists(marker, ec)) return {};
    try {
        auto v = platform::read_file_to_string(marker.string());
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
            v.pop_back();
        return v;
    } catch (...) { return {}; }
}

std::vector<IndexSourceView> collect_index_sources() {
    std::vector<IndexSourceView> views;
    for (const auto& repo : Config::global_index_repos()) {
        IndexSourceView view;
        view.name = repo.name;
        view.pin  = repo.version;
        view.installed = installed_index_version(Config::repo_dir_for(repo, false));

        auto source = artifact_source_for(repo);
        const auto& pointers = load_index_pointers(Config::mirror(),
                                                   source ? &*source : nullptr);
        const auto key = source ? source->key : repo.name;
        const auto* manifest = select_manifest(pointers, key, source.has_value());
        if (!manifest) {
            view.error = pointers.empty()
                ? "pointer unavailable (offline, or this repo is git-only)"
                : std::format("no pointer entry for '{}'", key);
            views.push_back(std::move(view));
            continue;
        }
        view.hasPointer = true;
        view.snapshots  = snapshots_of(*manifest);
        view.truncated  = manifest->history_truncated;
        if (auto choice = choose_snapshot(*manifest, Info::VERSION, repo.version)) {
            view.current = choice->snapshot.index_version;
        } else {
            view.error = choice.error();
        }
        views.push_back(std::move(view));
    }
    return views;
}

nlohmann::json index_sources_json(const std::vector<IndexSourceView>& views) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& view : views) {
        nlohmann::json entry;
        entry["name"]      = view.name;
        entry["pinned"]    = view.pin;
        entry["current"]   = view.current;
        // What is on disk, which is not the same question as what would be
        // picked: between a pointer moving and the next `update`, the two differ
        // and only this one describes the tree the client is actually reading.
        entry["installed"] = view.installed;
        entry["truncated"] = view.truncated;
        if (!view.error.empty()) entry["error"] = view.error;
        entry["snapshots"] = nlohmann::json::array();
        for (const auto& s : view.snapshots) {
            nlohmann::json row;
            row["index_version"] = s.index_version;
            row["generated_at"]  = s.generated_at;
            row["current"]       = s.index_version == view.current;
            row["installed"]     = !view.installed.empty()
                                   && s.index_version == view.installed;
            // Verbatim. Normalising here would make xlings an interpreter of
            // contracts it does not own.
            row["requires"]      = s.requirements;
            row["artifact"]      = { {"name", s.artifact_name},
                                     {"sha256", s.artifact_sha256},
                                     {"size", s.artifact_size} };
            entry["snapshots"].push_back(std::move(row));
        }
        out.push_back(std::move(entry));
    }
    return out;
}

int cmd_index_list(const std::string& filter, bool asJson, EventStream& stream) {
    auto views = collect_index_sources();
    if (!filter.empty()) {
        std::erase_if(views, [&](const auto& v) { return v.name != filter; });
        if (views.empty()) {
            log::error("no index source named '{}'", filter);
            return 1;
        }
    }

    if (asJson) {
        std::println("{}", index_sources_json(views).dump(2));
        return 0;
    }

    for (const auto& view : views) {
        log::println("");
        log::println("  ◆ {}{}", view.name,
                     view.pin.empty() ? "" : std::format("  (pinned: {})", view.pin));
        if (!view.error.empty()) {
            log::println("    {}", view.error);
            continue;
        }
        for (const auto& s : view.snapshots) {
            const auto req = requirement_for(s.requirements, "xlings");
            std::string note;
            if (req) {
                if (!req->min.empty()) note += ">= " + req->min;
                if (!req->max.empty()) note += (note.empty() ? "" : " ") + std::string("< ") + req->max;
                note = "  requires xlings " + note;
            }
            // `*` is the tree on disk -- the packages this client resolves
            // against right now. `>` is what the next `update` would fetch.
            // They differ whenever the pointer moved since the last update, and
            // reporting only the second would answer a question the user did
            // not ask while looking like it answered theirs.
            const bool onDisk = !view.installed.empty()
                                && s.index_version == view.installed;
            log::println("    {} {}{}{}",
                         onDisk ? "*" : (s.index_version == view.current ? ">" : " "),
                         s.index_version,
                         s.generated_at.empty() ? "" : "  " + s.generated_at,
                         note);
        }
        if (view.truncated) {
            log::println("      … older snapshots exist but are not listed");
        }
        if (view.installed.empty()) {
            log::println("      (no index on disk yet — run `xlings update`)");
        } else if (!view.current.empty() && view.current != view.installed) {
            log::println("      on disk: {} — run `xlings update` to move to {}",
                         view.installed, view.current);
        }
    }
    log::println("");
    return 0;
}

int cmd_index_use(const std::string& name, const std::string& version,
                  EventStream& stream) {
    if (name.empty()) {
        log::error("usage: xlings index use <name> <version|latest>");
        return 2;
    }
    const bool clearing = version.empty() || version == "latest";

    // Refuse a version the pointer does not offer -- accepting it would leave a
    // config that only fails on the next update, far from the command that
    // wrote it.
    if (!clearing) {
        auto views = collect_index_sources();
        const auto hit = std::ranges::find_if(views,
            [&](const auto& v) { return v.name == name; });
        if (hit == views.end()) {
            log::error("no index source named '{}'", name);
            return 1;
        }
        const bool known = std::ranges::any_of(hit->snapshots,
            [&](const auto& s) { return s.index_version == version; });
        if (!known) {
            log::error("index '{}' has no published snapshot '{}'", name, version);
            std::string list;
            for (const auto& s : hit->snapshots) {
                if (!list.empty()) list += ", ";
                list += s.index_version;
            }
            log::error("  available: {}", list.empty() ? "(none)" : list);
            return 1;
        }
    }

    auto applied = update_home_config(Config::paths().homeDir,
        [&](nlohmann::json& json) {
            if (!json.contains("index_repos") || !json["index_repos"].is_array()) {
                json["index_repos"] = nlohmann::json::array();
            }
            for (auto& entry : json["index_repos"]) {
                if (!entry.is_object() || !entry.contains("name")) continue;
                if (entry["name"].get<std::string>() != name) continue;
                if (clearing) entry.erase("version");
                else entry["version"] = version;
                return true;
            }
            // The source exists in the effective config but not in the file --
            // it is a built-in default. Materialise the entry so the pin has
            // somewhere to live.
            for (const auto& repo : Config::global_index_repos()) {
                if (repo.name != name) continue;
                nlohmann::json entry;
                entry["name"] = repo.name;
                entry["url"]  = repo.url;
                if (!clearing) entry["version"] = version;
                json["index_repos"].push_back(entry);
                return true;
            }
            return false;
        });

    if (!applied) {
        log::error("{}", applied.error());
        return 1;
    }
    if (!*applied) {
        log::error("no index source named '{}'", name);
        return 1;
    }
    if (clearing) {
        log::println("index '{}' follows the newest compatible snapshot", name);
    } else {
        log::println("index '{}' pinned to {}", name, version);
    }
    log::println("  run `xlings update` to apply");
    return 0;
}

}
