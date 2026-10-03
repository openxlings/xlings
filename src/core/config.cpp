module;
#include <cstdio>

module xlings.core.config;

import std;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.libs.tinyhttps;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.home_config;

namespace xlings {

namespace {

// Size and mtime of a file, or nothing when it cannot be stat'ed. Used to tell
// whether the home config changed since this process last read it.
std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>>
file_stat_(const std::filesystem::path& p) {
    std::error_code sizeEc;
    std::error_code timeEc;
    const auto size = std::filesystem::file_size(p, sizeEc);
    const auto mtime = std::filesystem::last_write_time(p, timeEc);
    if (sizeEc || timeEc) return std::nullopt;
    return std::pair{size, mtime};
}

// The `configured` record of a subos file (#632). Anything that is not a
// non-negative integer is dropped: a record that cannot be read proves nothing,
// and a missing entry only means "configure it", the safe direction.
std::map<std::string, int> configured_from_json_(const nlohmann::json& doc) {
    std::map<std::string, int> out;
    auto it = doc.find("configured");
    if (it == doc.end() || !it->is_object()) return out;
    for (auto e = it->begin(); e != it->end(); ++e) {
        if (e.key().empty() || !e->is_number_integer()) continue;
        const auto revision = e->get<long long>();
        if (revision < 0 || revision > std::numeric_limits<int>::max()) continue;
        out.emplace(e.key(), static_cast<int>(revision));
    }
    return out;
}

} // namespace

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

std::vector<ArtifactBase> parse_region_chain(const nlohmann::json& value,
                                             const std::string& mirror) {
    std::vector<ArtifactBase> chain;
    auto push = [&](std::string region, std::string url) {
        url = utils::trim_string(std::move(url));
        while (url.size() > 1 && url.ends_with('/')) url.pop_back();
        if (url.empty()) return;
        // Two regions naming the same URL is one location, not two attempts.
        for (auto& e : chain) if (e.url == url) return;
        chain.push_back(ArtifactBase{std::move(region), std::move(url)});
    };

    if (value.is_string()) { push({}, value.get<std::string>()); return chain; }
    if (!value.is_object()) return chain;

    // Preferred region, then GLOBAL, then everything else as declared. The
    // mirror decides the ORDER; it never decides the set.
    const std::string key = mirror.empty() ? "GLOBAL" : mirror;
    if (auto it = value.find(key); it != value.end() && it->is_string())
        push(key, it->get<std::string>());
    if (key != "GLOBAL")
        if (auto it = value.find("GLOBAL"); it != value.end() && it->is_string())
            push("GLOBAL", it->get<std::string>());
    for (auto it = value.begin(); it != value.end(); ++it)
        if (it->is_string()) push(it.key(), it->get<std::string>());
    return chain;
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
        if (it->contains("artifact"))
            repo.artifactBases = parse_region_chain((*it)["artifact"], mirror);
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


// ── out-of-line class members ──────────────────────────────────

namespace xlings {

void Config::override_home(const std::filesystem::path& home) {
    home_override_() = home;
}

std::optional<std::filesystem::path>& Config::home_override_() {
    static std::optional<std::filesystem::path> v;
    return v;
}

std::vector<IndexRepo> Config::default_global_index_repos_(const std::string& mirror,
                                                           const std::string& declaredUrl) {
    // The default index's URL is CONFIGURATION. `xlings self install` has been
    // writing xim.index-repo (and xim.mirrors.index-repo) since it shipped, and
    // nothing read it: three writes, zero reads. Meanwhile this function kept a
    // second copy of the URL, so "which repo is the official index" was
    // answered by a literal compiled into xlings rather than by the home it was
    // running against.
    //
    // `declaredUrl` is passed IN rather than read off the singleton: this runs
    // DURING Config's own initialisation, and reaching for instance_() from
    // here is a recursive init of the function-local static (it aborts with
    // __gnu_cxx::recursive_init_error, which names nothing useful).
    //
    // The literals below are the last-resort default for a home whose config
    // predates the key -- not the source of truth.
    if (!declaredUrl.empty()) {
        return { IndexRepo{std::string(DEFAULT_INDEX_REPO_NAME), declaredUrl} };
    }
    std::string url = "https://github.com/openxlings/xim-pkgindex.git";
    if (mirror == "CN") {
        url = "https://gitee.com/sunrisepeak/xim-pkgindex.git";
    }
    return { IndexRepo{std::string(DEFAULT_INDEX_REPO_NAME), url} };
}

[[nodiscard]] std::string Config::declared_index_repo_url(std::string_view name) {
    if (name != DEFAULT_INDEX_REPO_NAME) return {};
    // Safe to reach the singleton here: every caller asks at sync time, long
    // after initialisation. (default_global_index_repos_ above cannot.)
    auto& self = instance_();
    self.ensure_index_config_();
    if (!self.defaultIndexRepoUrl_.empty()) return self.defaultIndexRepoUrl_;
    // No key in the config: fall back to the same built-in the default entry
    // itself would have used, so an entry that matches the built-in default is
    // still recognised as the declared source.
    auto defaults = default_global_index_repos_(self.mirror_, {});
    return defaults.empty() ? std::string{} : defaults.front().url;
}

MirrorServerMap Config::default_resource_servers_() {
    return {
        { "GLOBAL", { "https://github.com/xlings-res" } },
        { "CN", { "https://gitcode.com/xlings-res" } },
    };
}

std::vector<ArtifactBase> Config::resolve_index_base_(const nlohmann::json& json,
                                                     const std::string& mirror) {
    if (!json.contains("xim") || !json["xim"].is_object()) return {};
    auto& xim = json["xim"];
    if (!xim.contains("index-base")) return {};
    return parse_region_chain(xim["index-base"], mirror);
}

std::string Config::resolve_default_index_repo_(const nlohmann::json& json,
                                                const std::string& mirror) {
    // `xim.mirrors.index-repo` is the region map and `xim.index-repo` is the
    // already-resolved value for the region the home was installed for. Read
    // the region map FIRST so a home whose mirror changed since install follows
    // the change, and fall back to the flat key -- which is what a hand-edited
    // config is most likely to carry.
    if (!json.contains("xim") || !json["xim"].is_object()) return {};
    auto& xim = json["xim"];

    if (xim.contains("mirrors") && xim["mirrors"].is_object()) {
        auto& mirrors = xim["mirrors"];
        if (mirrors.contains("index-repo") && mirrors["index-repo"].is_object()) {
            // #598: read through the same chain parser as every other
            // region-keyed key, but keep only the head -- this one names a git
            // REMOTE, and a local clone has exactly one origin. Chaining here
            // would mean rewriting a checkout's origin, which is a different
            // decision; it is not an omission.
            auto chain = parse_region_chain(mirrors["index-repo"], mirror);
            if (!chain.empty()) return chain.front().url;
        }
    }
    if (xim.contains("index-repo") && xim["index-repo"].is_string())
        return xim["index-repo"].get<std::string>();
    return {};
}

std::vector<std::string> Config::parse_server_list_(const nlohmann::json& value) {
    std::vector<std::string> servers;
    auto append_server = [&](std::string server) {
        server = utils::trim_string(server);
        while (server.size() > 1 && server.ends_with('/')) {
            server.pop_back();
        }
        if (server.empty()) return;
        if (std::ranges::find(servers, server) == servers.end()) {
            servers.push_back(std::move(server));
        }
    };

    if (value.is_string()) {
        append_server(value.get<std::string>());
    } else if (value.is_array()) {
        for (auto& item : value) {
            if (item.is_string()) append_server(item.get<std::string>());
        }
    }
    return servers;
}

void Config::merge_resource_servers_into_(MirrorServerMap& dst, const MirrorServerMap& src) {
    for (auto& [mirror, servers] : src) {
        if (!servers.empty()) dst[mirror] = servers;
    }
}

void Config::load_resource_servers_from_json_(const nlohmann::json& json,
                                                 MirrorServerMap& out) {
    out.clear();

    auto normalize_key = [](std::string key) {
        key = utils::trim_string(key);
        if (key == "default" || key == "DEFAULT" || key == "_default") return std::string("DEFAULT");
        return key;
    };

    auto load_object = [&](const nlohmann::json& obj) {
        if (!obj.is_object()) return;
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            auto servers = parse_server_list_(it.value());
            if (!servers.empty()) out[normalize_key(it.key())] = std::move(servers);
        }
    };

    auto load_default_list = [&](const nlohmann::json& value) {
        auto servers = parse_server_list_(value);
        if (!servers.empty()) out["DEFAULT"] = std::move(servers);
    };

    if (json.contains("XLINGS_RES")) {
        if (json["XLINGS_RES"].is_object()) {
            load_object(json["XLINGS_RES"]);
        } else {
            load_default_list(json["XLINGS_RES"]);
        }
    }

    // Backward compatibility for older config shapes.
    if (out.empty()) {
        if (json.contains("resource_server")) {
            load_default_list(json["resource_server"]);
        }

        if (json.contains("resource_servers")) {
            if (json["resource_servers"].is_object()) {
                load_object(json["resource_servers"]);
            } else {
                load_default_list(json["resource_servers"]);
            }
        }

        if (json.contains("res_servers")) {
            if (json["res_servers"].is_object()) {
                load_object(json["res_servers"]);
            } else {
                load_default_list(json["res_servers"]);
            }
        }

        if (json.contains("xim") && json["xim"].is_object()) {
            auto& xim = json["xim"];
            if (xim.contains("mirrors") && xim["mirrors"].is_object()) {
                auto& mirrors = xim["mirrors"];
                if (mirrors.contains("res-server")) {
                    load_object(mirrors["res-server"]);
                }
            }
        }
    }
}

std::optional<xvm::SubosWorkspace>
Config::read_workspace_file_(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        // THREE cases, not two. A missing file is not automatically "could not
        // observe": a subos that exists and has never had anything activated
        // in it legitimately has no workspace file, and that is an OBSERVED
        // empty workspace. Collapsing it into nullopt makes the consumers that
        // refuse to act on nullopt refuse on every fresh home -- measured:
        // `project_shim_mirror_test` stopped mirroring the project's shim into
        // a brand-new global bin, which is the mirror image of the bug this
        // whole change exists to fix.
        //
        // What is genuinely unobservable is being pointed at a subos that is
        // not there at all.
        ec.clear();
        if (fs::is_directory(path.parent_path(), ec) && !ec) {
            return xvm::SubosWorkspace{};
        }
        return std::nullopt;
    }
    try {
        auto content = platform::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return std::nullopt;
        // A subos file with no `workspace` key IS observed -- it is the shape a
        // freshly created subos has before anything is activated in it. Only a
        // file we could not read at all is unobserved.
        xvm::SubosWorkspace sws;
        if (auto it = json.find("workspace");
            it != json.end() && it->is_object()) {
            sws = xvm::subos_workspace_from_json(*it);
        }
        sws.configured = configured_from_json_(json);
        return sws;
    } catch (...) {}
    return std::nullopt;
}

xvm::SubosWorkspace Config::load_workspace_from_file_(const std::filesystem::path& path) {
    // The lossy view, kept for the callers whose input is a PROJECT file: a
    // project that cannot be read contributes nothing and removes nothing,
    // which `xvm::ProjectContribution::readable` already expresses for them.
    return read_workspace_file_(path).value_or(xvm::SubosWorkspace{});
}

std::string Config::load_project_subos_name_(const nlohmann::json& json) {
    if (json.contains("subos") && json["subos"].is_string()) {
        auto val = json["subos"].get<std::string>();
        if (!val.empty()) return val;
    }
    if (json.contains("projectSubos") && json["projectSubos"].is_string()) {
        auto val = json["projectSubos"].get<std::string>();
        if (!val.empty()) return val;
    }
    return {};
}

void Config::merge_versions_into_(xvm::VersionDB& dst, const xvm::VersionDB& src) {
    for (auto& [target, info] : src) {
        auto& dstInfo = dst[target];
        if (dstInfo.type.empty() && !info.type.empty())
            dstInfo.type = info.type;
        if (dstInfo.filename.empty() && !info.filename.empty())
            dstInfo.filename = info.filename;
        for (auto& [ver, vdata] : info.versions) {
            dstInfo.versions[ver] = vdata;
        }
        for (auto& [name, vermap] : info.bindings) {
            for (auto& [ver, value] : vermap) {
                dstInfo.bindings[name][ver] = value;
            }
        }
    }
}

void Config::merge_workspace_into_(xvm::Workspace& dst, const xvm::Workspace& src) {
    for (auto& [target, version] : src) {
        dst[target] = version;
    }
}

std::string Config::effective_mirror_name_(std::string_view mirror,
                                              std::string_view fallback) {
    auto name = std::string(mirror.empty() ? fallback : mirror);
    if (name.empty()) return "GLOBAL";
    return name;
}

std::vector<std::string> Config::workspace_targets_from_workspace_(const xvm::Workspace& ws) {
    std::vector<std::string> targets;
    targets.reserve(ws.size());
    for (auto& [name, version] : ws) {
        if (name.empty()) continue;
        if (version.empty()) {
            targets.push_back(name);
        } else {
            targets.push_back(name + "@" + version);
        }
    }
    return targets;
}

[[nodiscard]] std::filesystem::path Config::project_data_dir_() const {
    return projectDir_.empty() ? std::filesystem::path{} : projectDir_ / ".xlings" / "data";
}

[[nodiscard]] std::filesystem::path Config::project_home_dir_() const {
    return projectDir_.empty() ? std::filesystem::path{} : projectDir_ / ".xlings";
}

[[nodiscard]] std::filesystem::path Config::project_state_path_() const {
    auto homeDir = project_home_dir_();
    if (homeDir.empty()) return {};
    return homeDir / ".xlings.json";
}

[[nodiscard]] std::filesystem::path Config::project_manifest_path_() const {
    return projectDir_.empty() ? std::filesystem::path{} : projectDir_ / ".xlings.json";
}

[[nodiscard]] std::filesystem::path Config::project_subos_dir_() const {
    if (projectDir_.empty()) return {};
    if (!projectSubosName_.empty()) return projectDir_ / ".xlings" / "subos" / projectSubosName_;
    if (projectSubosMode_ == ProjectSubosMode::Anonymous) return projectDir_ / ".xlings" / "subos" / "_";
    return {};
}

[[nodiscard]] std::string Config::global_subos_name_() const {
    // Override first. Every caller of `set_active_subos_override` pairs it with
    // XLINGS_ACTIVE_SUBOS today (xim/commands.cpp, subos.cpp x2,
    // xself/doctor.cpp), so honoring it here changes no behaviour -- it removes
    // the asymmetry that made "set only one of the two" a recurring defect.
    if (!activeSubosOverride_.empty()) return activeSubosOverride_;
    auto env = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
    if (!env.empty()) return env;
    return globalActiveSubos_;
}

[[nodiscard]] std::filesystem::path Config::global_subos_dir_() const {
    return paths_.homeDir / "subos" / global_subos_name_();
}

[[nodiscard]] std::vector<std::string> Config::lookup_resource_servers_in_(const MirrorServerMap& source, std::string_view mirror) {
    auto key = effective_mirror_name_(mirror, "GLOBAL");
    if (auto it = source.find(key); it != source.end() && !it->second.empty()) {
        return it->second;
    }
    if (auto it = source.find("DEFAULT"); it != source.end() && !it->second.empty()) {
        return it->second;
    }
    return {};
}

[[nodiscard]] std::vector<std::string> Config::candidate_resource_servers_for_(std::string_view mirror) const {
    auto key = effective_mirror_name_(mirror, mirror_);

    auto project = lookup_resource_servers_in_(projectResourceServers_, key);
    if (!project.empty()) return project;

    auto global = lookup_resource_servers_in_(globalResourceServers_, key);
    if (!global.empty()) return global;

    auto defaults = lookup_resource_servers_in_(default_resource_servers_(), key);
    if (!defaults.empty()) return defaults;

    auto fallback = lookup_resource_servers_in_(default_resource_servers_(), "GLOBAL");
    if (!fallback.empty()) return fallback;
    return {};
}

[[nodiscard]] std::vector<std::string> Config::all_resource_servers_for_(std::string_view mirror) const {
    auto servers = candidate_resource_servers_for_(mirror);
    auto key = effective_mirror_name_(mirror, mirror_);

    std::set<std::string> seen(servers.begin(), servers.end());
    auto append_other_regions = [&](const MirrorServerMap& source) {
        // unordered_map: sort the keys so the fallback order is the same
        // on every run and every machine.
        std::vector<std::string> regions;
        for (const auto& [region, _] : source) {
            if (region != key) regions.push_back(region);
        }
        std::ranges::sort(regions);
        for (const auto& region : regions) {
            for (const auto& server : source.at(region)) {
                if (seen.insert(server).second) servers.push_back(server);
            }
        }
    };
    append_other_regions(projectResourceServers_);
    append_other_regions(globalResourceServers_);
    append_other_regions(default_resource_servers_());
    return servers;
}

double Config::probe_resource_server_latency_(const std::string& server) {
    return tinyhttps::probe_latency(server, 2000);
}

[[nodiscard]] std::string Config::selected_resource_server_for_(std::string_view mirror) const {
    auto key = effective_mirror_name_(mirror, mirror_);
    {
        std::scoped_lock lock(resourceServerMutex_);
        if (auto it = selectedResourceServerCache_.find(key);
            it != selectedResourceServerCache_.end()) {
            return it->second;
        }
    }

    auto candidates = candidate_resource_servers_for_(key);
    if (candidates.empty()) return {};

    auto selected = candidates.front();
    if (candidates.size() > 1) {
        double bestLatency = std::numeric_limits<double>::infinity();
        for (auto& candidate : candidates) {
            auto latency = probe_resource_server_latency_(candidate);
            if (latency < bestLatency) {
                bestLatency = latency;
                selected = candidate;
            }
            if (std::isfinite(latency) && latency <= 0.1) {
                selected = candidate;
                break;
            }
        }
    }

    {
        std::scoped_lock lock(resourceServerMutex_);
        selectedResourceServerCache_[key] = selected;
    }
    return selected;
}

[[nodiscard]] SubosScope Config::resolve_subos_scope_() const {
    // An explicit override wins over everything, including a project
    // config. It exists for the case where a command must act on a subos
    // it just created rather than on the one the user is standing in —
    // `subos new --runtime` installing what it declared.
    if (!activeSubosOverride_.empty()) {
        return {activeSubosOverride_,
                paths_.homeDir / "subos" / activeSubosOverride_};
    }
    const bool useProject = hasProjectConfig_ && !forceGlobalScope_;
    if (useProject && projectSubosMode_ == ProjectSubosMode::Named
        && !projectSubosName_.empty()) {
        return {projectSubosName_, project_subos_dir_()};
    }
    if (useProject && projectSubosMode_ == ProjectSubosMode::Anonymous) {
        return {"_", project_subos_dir_()};
    }
    auto name = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
    if (name.empty()) name = globalActiveSubos_;
    return {name, paths_.homeDir / "subos" / name};
}

void Config::update_effective_paths_() {
    auto scope = resolve_subos_scope_();
    paths_.activeSubos = std::move(scope.name);
    paths_.subosDir = std::move(scope.root);
    paths_.binDir = paths_.subosDir / "bin";
    paths_.libDir = paths_.subosDir / "lib";
}

Config::Config() {
    namespace fs = std::filesystem;

    auto envHome = utils::get_env_or_default("XLINGS_HOME");
    if (auto& anchored = home_override_(); anchored && !anchored->empty()) {
        // Owner-anchored shim dispatch: the dispatch home was chosen
        // before Config construction (main.cpp → resolve_dispatch_home).
        paths_.homeDir = *anchored;
    } else if (!envHome.empty()) {
        paths_.homeDir = envHome;
    } else {
        auto exePath   = platform::get_executable_path();
        auto exeParent = exePath.parent_path();
        auto candidate = exeParent.parent_path();
        auto hasRootConfig = !exePath.empty() && fs::exists(candidate / ".xlings.json");
        constexpr auto kBinName = (platform::OS_NAME == "windows")
            ? "bin/xlings.exe" : "bin/xlings";
        auto hasRootBin = !exePath.empty() && fs::exists(candidate / kBinName);

        bool isSelfContained = hasRootConfig && hasRootBin;
        if constexpr (platform::OS_NAME == "windows") {
            // Windows: shims are hardlinks/copies (not symlinks), so
            // get_executable_path() returns the shim path inside a subos
            // dir (e.g. ~/.xlings/subos/current/bin/xlings.exe). That
            // directory has both .xlings.json and bin/xlings.exe, falsely
            // matching the selfContained pattern. Disambiguate by checking
            // json content: real home/selfContained config always has both
            // "version" and "activeSubos"; subos config only has workspace.
            if (isSelfContained) {
                try {
                    auto cfg = platform::read_file_to_string(
                        (candidate / ".xlings.json").string());
                    auto j = nlohmann::json::parse(cfg, nullptr, false);
                    isSelfContained = !j.is_discarded()
                        && j.contains("version")
                        && j.contains("activeSubos");
                } catch (...) { isSelfContained = false; }
            }
        }
        if (isSelfContained) {
            paths_.homeDir       = candidate;
            paths_.selfContained = true;
        } else {
            paths_.homeDir = fs::path(platform::get_home_dir()) / ".xlings";
        }
    }

    auto configPath = paths_.homeDir / ".xlings.json";
    // The cheap capture, not a full parse: it stops before `versions`,
    // which is ~90% of the bytes on a real home and which nothing here
    // reads anymore (globalVersions_ loads on first use, from the versions
    // DB file when the home has one). Index config waits too -- it needs
    // `xim`, which sorts after `versions` -- so `ensure_index_config_`
    // re-captures with SkipVersions on first use.
    if (auto cap = home_config_capture(configPath, HomeCaptureMode::AbortAtVersions);
        cap && cap->ok) {
        remember_known_projects_(cap->json, cap->size, cap->mtime);
        const auto& json = cap->json;
        if (json.contains("activeSubos") && json["activeSubos"].is_string()) {
            auto val = json["activeSubos"].get<std::string>();
            if (!val.empty()) globalActiveSubos_ = val;
        }
        if (json.contains("mirror") && json["mirror"].is_string())
            mirror_ = json["mirror"].get<std::string>();
        if (json.contains("lang") && json["lang"].is_string())
            lang_ = json["lang"].get<std::string>();
        load_ui_prefs_from_json_(json);
    }
    paths_.dataDir  = paths_.homeDir / "data";
    update_effective_paths_();

    log::debug("config: home={}, selfContained={}", paths_.homeDir.string(), paths_.selfContained);

    // The global subos workspace loads lazily too (ensure_global_workspace_):
    // a shim dispatch on a shim-view cache hit never reads it.

    // Load project-level config (walk up from cwd)
    load_project_config_();
    update_effective_paths_();
}

void Config::load_project_config_from_dir_(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    auto cfg = dir / ".xlings.json";
    log::debug("config: loading project config from {}", cfg.string());
    try {
        auto content = platform::read_file_to_string(cfg.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (!json.is_discarded()) {
            // Build-deps-only files (e.g. xlings's own repo-root
            // /.xlings.json declaring CI dependencies) opt out of
            // project mode by setting `"projectScope": false`. This
            // lets `xlings install` from the repo root still read
            // the `workspace` field, but skips project-subos
            // activation, project-state writes, and the project_dir
            // env-export that downstream shims would otherwise pick
            // up. Search continues upward as if this file weren't here.
            //
            // TODO(2026-): this flag is a workaround for the
            // chicken-and-egg of "xlings's own repo wants to declare
            // build deps via .xlings.json without becoming a
            // managed xlings project". Cleaner long-term options:
            //
            //   1. Embrace project mode at the repo root: rewrite
            //      tests / dev workflow to assume the xlings repo
            //      IS a managed project; drop this flag entirely.
            //   2. Add `xlings env` / `xlings config --pkgdir <name>`
            //      so CI doesn't need to assume payload paths or
            //      hand-export toolchain env vars at all — the
            //      installed-subos PATH + xmake auto-detect already
            //      cover it for the common case (see CI workflows
            //      that drop --sdk/--cross thanks to musl-gcc's
            //      gcc-flavor shims), but cross-compile / non-default
            //      toolchain selections still need explicit values.
            //   3. Split: keep `.xlings.json` for `xlings install`
            //      workspace reads, but introduce a separate
            //      schema-level marker (e.g. top-level
            //      `"kind": "build-deps"`) checked here.
            //
            // Track these in a single follow-up issue once the CI
            // self-host PR has soaked in.
            if (json.contains("projectScope") &&
                json["projectScope"].is_boolean() &&
                !json["projectScope"].get<bool>()) {
                log::debug("config: skipping {} (projectScope=false)", cfg.string());
                return;
            }
            projectDir_ = dir;
            hasProjectConfig_ = true;
            // Export project dir so child processes (shims, os.execute)
            // can find project workspace even when CWD changes.
            platform::set_env_variable("XLINGS_PROJECT_DIR", dir.string());
            // Project-level mirror/lang override global
            if (json.contains("mirror") && json["mirror"].is_string())
                mirror_ = json["mirror"].get<std::string>();
            if (json.contains("lang") && json["lang"].is_string())
                lang_ = json["lang"].get<std::string>();
            // Project overrides global, same as mirror/lang: a repo may ship
            // its own colours, and `theme` is a path reference so it can point
            // at a file inside the checkout.
            load_ui_prefs_from_json_(json);
            if (json.contains("workspace") && json["workspace"].is_object()) {
                projectWorkspace_ = xvm::workspace_from_json(json["workspace"]);
            }
            projectIndexRepos_ = parse_index_repos_json(json, mirror_);
            load_resource_servers_from_json_(json, projectResourceServers_);
            if (auto v = resolve_index_base_(json, mirror_); !v.empty())
                indexBases_ = std::move(v);   // project overrides global
            projectSubosName_ = load_project_subos_name_(json);

            auto projectStatePath = project_state_path_();
            nlohmann::json projectStateJson;
            bool hasProjectStateJson = false;
            if (!projectStatePath.empty() && fs::exists(projectStatePath)) {
                try {
                    auto stateContent = platform::read_file_to_string(projectStatePath.string());
                    projectStateJson = nlohmann::json::parse(stateContent, nullptr, false);
                    hasProjectStateJson = !projectStateJson.is_discarded() && projectStateJson.is_object();
                } catch (...) {
                    hasProjectStateJson = false;
                }
            }

            if (hasProjectStateJson && projectStateJson.contains("versions") &&
                projectStateJson["versions"].is_object()) {
                projectVersions_ = xvm::versions_from_json(projectStateJson["versions"]);
            } else if (json.contains("versions") && json["versions"].is_object()) {
                projectVersions_ = xvm::versions_from_json(json["versions"]);
            }

            if (!projectSubosName_.empty()) {
                projectSubosMode_ = ProjectSubosMode::Named;
                auto sws = load_workspace_from_file_(project_subos_dir_() / ".xlings.json");
                projectSubosWorkspace_ = std::move(sws.active);
                projectSubosInstalled_ = std::move(sws.installed);
                projectSubosConfigured_ = std::move(sws.configured);
            } else {
                projectSubosMode_ = ProjectSubosMode::Anonymous;
                if (hasProjectStateJson && projectStateJson.contains("workspace") &&
                    projectStateJson["workspace"].is_object()) {
                    auto sws = xvm::subos_workspace_from_json(projectStateJson["workspace"]);
                    projectSubosWorkspace_ = std::move(sws.active);
                    projectSubosInstalled_ = std::move(sws.installed);
                    projectSubosConfigured_ = configured_from_json_(projectStateJson);
                } else {
                    auto sws = load_workspace_from_file_(project_subos_dir_() / ".xlings.json");
                    projectSubosWorkspace_ = std::move(sws.active);
                    projectSubosInstalled_ = std::move(sws.installed);
                    projectSubosConfigured_ = std::move(sws.configured);
                }
            }
        }
    } catch (...) {}
}

void Config::load_project_config_() {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path startDir = fs::current_path(ec);
    if (ec) return;

    auto homeNorm = fs::weakly_canonical(paths_.homeDir, ec);

    // Walk cwd → root, looking for a `.xlings.json` that activates project
    // mode. A file with `projectScope: false` is "deps-manifest only" — it
    // declares deps for `xlings install` but does NOT mean its directory is
    // a project root (e.g. xlings's own repo uses this to install build
    // deps without making the repo look like a user project to nested
    // commands). load_project_config_from_dir_ honors that opt-out by
    // returning without setting hasProjectConfig_.
    //
    // Critical: when from_dir_ skips a projectScope:false file, we must
    // NOT early-return — that would hide the real project from any
    // subprocess whose cwd traversal happens to hit such a file before
    // reaching the actual project. Instead, continue walking up; if no
    // real project is found in the rest of the walk, the env-var fallback
    // below picks it up (the parent xlings exports XLINGS_PROJECT_DIR
    // whenever it loads a real project, so subprocesses can recover the
    // intended project context even when their cwd is outside the project
    // tree).
    // 0.4.20+: xlings-home boundary detection.
    //
    // A directory that contains BOTH `.xlings.json` and a `subos/`
    // sibling directory is an xlings home (ours or some other one) —
    // never a user project. Project layouts have their state under
    // `.xlings/subos/`, NOT a bare `subos/` at the same level as
    // the manifest. Stop walking at any such boundary.
    //
    // Without this, a nested xlings home (e.g. mcpp packaged under
    // ~/.xlings/data/xpkgs/<repo>-x-mcpp/<ver>/registry/, where the
    // inner xlings sets XLINGS_HOME to the registry dir) would see
    // its CWD walk skip the inner home (homeNorm match), continue
    // upward through the package layers, and ultimately mis-load the
    // OUTER ~/.xlings/.xlings.json as if it were a user project root.
    // That polluted projectDir_ with the outer home and routed every
    // subsequent install/shim/workspace path into a phantom
    // ~/.xlings/.xlings/{subos,data,...} tree disjoint from where
    // mcpp's recipe actually puts its files.
    //
    // The boundary check subsumes the previous `curNorm != homeNorm`
    // gate: our own home also matches the signature, so the break
    // covers it. Any directory that "looks like" an xlings home
    // (regardless of whose) terminates the project walk.
    auto looks_like_xlings_home = [&](const fs::path& dir) {
        std::error_code lec;
        return fs::is_directory(dir / "subos", lec);
    };

    fs::path cur = startDir;
    while (!cur.empty()) {
        auto cfg = cur / ".xlings.json";
        if (fs::exists(cfg, ec) && fs::is_regular_file(cfg, ec)) {
            if (looks_like_xlings_home(cur)) {
                break;                              // hit an xlings home — never project
            }
            load_project_config_from_dir_(cur);
            if (hasProjectConfig_) return;          // real project loaded
            // else: projectScope:false skip — keep walking, then env fallback
        }
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }

    // CWD traversal did not find project config (either no .xlings.json
    // at all, or only projectScope:false ones) — check XLINGS_PROJECT_DIR
    // env var as a last resort.
    if (!hasProjectConfig_) {
        auto env_project = utils::get_env_or_default("XLINGS_PROJECT_DIR");
        if (!env_project.empty()) {
            auto dir = fs::path(env_project);
            auto cfgFile = dir / ".xlings.json";
            if (fs::exists(cfgFile, ec) && fs::is_regular_file(cfgFile, ec)) {
                // Same xlings-home boundary check as the CWD walk:
                // an env-var pointing at any xlings home (ours or a
                // different one) is rejected, since project mode
                // would derive paths that don't match the home's
                // actual layout.
                if (!looks_like_xlings_home(dir)) {
                    load_project_config_from_dir_(dir);
                }
            }
        }
    }
}

void Config::ensure_global_versions_() {
    if (globalVersionsLoaded_) return;
    globalVersionsLoaded_ = true;
    // ONE source decision, owned by load_versions_json: the versions DB
    // file when the home has one, else the home config's `versions` field,
    // and never "unreadable" mistaken for "empty".
    if (auto root = load_versions_json(paths_.homeDir)) {
        globalVersions_ = xvm::versions_from_json(*root);
    } else {
        log::debug("config: versions unreadable in {}; treating as empty",
                   paths_.homeDir.string());
        globalVersions_.clear();
    }
}

void Config::ensure_global_workspace_() {
    if (globalWorkspaceLoaded_) return;
    globalWorkspaceLoaded_ = true;
    // Load subos workspace from the path resolved by global_subos_dir_(),
    // which honors XLINGS_ACTIVE_SUBOS env overrides. Using
    // `globalActiveSubos_` directly here (a snapshot of
    // `~/.xlings.json activeSubos`) would silently load the wrong subos's
    // workspace whenever the user is in a spawned subos shell with
    // XLINGS_ACTIVE_SUBOS set, which then corrupts that subos when
    // `xvm use` writes back through save_workspace().
    load_global_workspace_();
}

void Config::ensure_index_config_() {
    if (indexConfigLoaded_) return;
    indexConfigLoaded_ = true;
    // SkipVersions, not AbortAtVersions: this layer needs `xim`, which
    // sorts AFTER `versions` and is therefore absent from the cheap
    // capture by design. The capture still skips `versions` itself, so an
    // oversized database costs a lex pass here, not a DOM.
    auto configPath = paths_.homeDir / ".xlings.json";
    nlohmann::json json = nlohmann::json::object();
    if (auto cap = home_config_capture(configPath, HomeCaptureMode::SkipVersions);
        cap && cap->ok) {
        json = cap->json;
    }
    // BEFORE parse_index_repos_json / default_global_index_repos_:
    // both of those ask what the default index's URL is, and this
    // is where the answer comes from. `mirror_` is already read
    // (at construction), which the region lookup needs.
    defaultIndexRepoUrl_ = resolve_default_index_repo_(json, mirror_);
    globalIndexRepos_ = parse_index_repos_json(json, mirror_);
    load_resource_servers_from_json_(json, globalResourceServers_);
    if (auto v = resolve_index_base_(json, mirror_); !v.empty()) indexBases_ = std::move(v);

    if (globalIndexRepos_.empty()) {
        globalIndexRepos_ = default_global_index_repos_(mirror_, defaultIndexRepoUrl_);
    } else {
        // Ensure the default index repo is always present when user
        // defines custom index_repos (e.g. adding "ros2").  Without
        // this, user-defined repos replace the default and packages
        // in the primary index (like "python") become unfindable.
        auto defaults = default_global_index_repos_(mirror_, defaultIndexRepoUrl_);
        for (auto& def : defaults) {
            bool found = false;
            for (auto& repo : globalIndexRepos_) {
                if (repo.name == def.name) { found = true; break; }
            }
            if (!found) {
                globalIndexRepos_.insert(globalIndexRepos_.begin(), std::move(def));
            }
        }
    }
}

std::optional<xvm::VInfo> Config::find_vinfo_(const std::string& target) {
    ensure_global_versions_();
    std::optional<xvm::VInfo> out;
    if (auto it = globalVersions_.find(target); it != globalVersions_.end()) {
        out = it->second;
    }
    // The overlay merge_versions_into_ performs, restricted to this target:
    // the project's versions and bindings WIN per entry, type and filename
    // are filled from global when the project leaves them empty.
    if (!forceGlobalScope_ && hasProjectConfig_) {
        if (auto it = projectVersions_.find(target); it != projectVersions_.end()) {
            auto& p = it->second;
            if (!out) out = xvm::VInfo{};
            if (out->type.empty() && !p.type.empty()) out->type = p.type;
            if (out->filename.empty() && !p.filename.empty()) out->filename = p.filename;
            for (auto& [ver, vdata] : p.versions) out->versions[ver] = vdata;
            for (auto& [name, vermap] : p.bindings) {
                for (auto& [ver, value] : vermap) out->bindings[name][ver] = value;
            }
        }
    }
    return out;
}

void Config::load_global_workspace_() {
    // `global_subos_dir_()`, NOT `paths_.activeSubos`.
    //
    // `paths_.activeSubos` answers "which subos does this command act on". In
    // project scope that is the project's subos, and reading the GLOBAL
    // workspace out of it meant reading `<home>/subos/_/.xlings.json` (absent
    // -> empty) or `<home>/subos/<projectSubos>/.xlings.json` (a different
    // subos's state). `reload_state_()` runs this BEFORE it recomputes the
    // paths, so the constructor's correct ordering did not save it.
    auto subosConfigPath = global_subos_dir_() / ".xlings.json";
    auto sws = read_workspace_file_(subosConfigPath);
    globalWorkspaceObserved_ = sws.has_value();
    if (!sws) {
        log::debug("config: global workspace at {} is unreadable; "
                   "consumers that remove state must not act on it",
                   subosConfigPath.string());
        globalWorkspace_.clear();
        globalInstalled_.clear();
        globalConfigured_.clear();
        return;
    }
    globalWorkspace_ = std::move(sws->active);
    globalInstalled_ = std::move(sws->installed);
    globalConfigured_ = std::move(sws->configured);
}

void Config::reload_state_() {
    namespace fs = std::filesystem;
    // Drop the lazily-loaded layers; the next accessor re-reads them from
    // disk. The capture memo self-invalidates against the file's stat, so
    // a capture taken after the write this reload follows reflects it.
    globalVersionsLoaded_ = false;
    globalWorkspaceLoaded_ = false;
    indexConfigLoaded_ = false;
    if (auto cap = home_config_capture(paths_.homeDir / ".xlings.json",
                                       HomeCaptureMode::AbortAtVersions);
        cap && cap->ok) {
        remember_known_projects_(cap->json, cap->size, cap->mtime);
    }
    load_global_workspace_();
    if (hasProjectConfig_) load_project_config_();
    update_effective_paths_();
}

[[nodiscard]] std::vector<std::string> Config::workspace_install_targets(const xvm::Workspace& ws) {
    return workspace_targets_from_workspace_(ws);
}

[[nodiscard]] xvm::VersionDB Config::merged_versions(const xvm::VersionDB& globalVersions,
                                                        const xvm::VersionDB& projectVersions) {
    auto db = globalVersions;
    merge_versions_into_(db, projectVersions);
    return db;
}

[[nodiscard]] xvm::Workspace Config::merged_workspace(const xvm::Workspace& globalWorkspace,
                                                         const xvm::Workspace& projectWorkspace,
                                                         const xvm::Workspace& projectSubosWorkspace,
                                                         ProjectSubosMode mode) {
    if (mode == ProjectSubosMode::Named) {
        auto ws = projectWorkspace;
        merge_workspace_into_(ws, projectSubosWorkspace);
        return ws;
    }

    if (mode == ProjectSubosMode::Anonymous) {
        auto ws = globalWorkspace;
        merge_workspace_into_(ws, projectWorkspace);
        merge_workspace_into_(ws, projectSubosWorkspace);
        return ws;
    }

    return globalWorkspace;
}

void Config::reload_state() { instance_().reload_state_(); }

[[nodiscard]] const Config::PathInfo& Config::paths() { return instance_().paths_; }

[[nodiscard]] std::string Config::display_path(
            const std::filesystem::path& p) {
    const auto& home = paths().homeDir;
    if (home.empty()) return p.string();
    // lexically_relative rather than a string prefix test: it compares
    // whole components, so a sibling `.xlings-backup` cannot match, and
    // it resolves `..` on the way. A prefix test needed both of those
    // bolted on by hand, and the hand-rolled version also normalized the
    // separators of paths it was supposed to return untouched -- on
    // Windows `/usr/lib` came back as `\usr\lib`, breaking the one
    // promise this function makes about paths outside the home.
    const auto rel = p.lexically_normal()
                      .lexically_relative(home.lexically_normal());
    if (rel.empty() || *rel.begin() == "..") return p.string();
    if (rel == std::filesystem::path(".")) return "@xlings";
    return (std::filesystem::path("@xlings") / rel).string();
}

[[nodiscard]] std::string Config::display_path(const std::string& p) {
    return display_path(std::filesystem::path{p});
}

[[nodiscard]] const std::string& Config::mirror() { return instance_().mirror_; }

void Config::load_ui_prefs_from_json_(const nlohmann::json& json) {
    if (json.contains("uiMode") && json["uiMode"].is_string())
        uiMode_ = json["uiMode"].get<std::string>();
    if (json.contains("theme") && json["theme"].is_string())
        theme_ = json["theme"].get<std::string>();
    // Nested under `tui` because it is that frontend's own setting -- it means
    // nothing in `cli` mode. Contrast `uiMode`/`theme`/`lang`, which are
    // global choices and stay flat.
    if (json.contains("tui") && json["tui"].is_object()) {
        const auto& tui = json["tui"];
        if (tui.contains("interactive") && tui["interactive"].is_boolean())
            tuiInteractive_ = tui["interactive"].get<bool>();
    }
}

[[nodiscard]] const std::string& Config::lang() { return instance_().lang_; }

[[nodiscard]] const std::string& Config::ui_mode() { return instance_().uiMode_; }

[[nodiscard]] const std::string& Config::theme() { return instance_().theme_; }

[[nodiscard]] std::optional<bool> Config::tui_interactive() {
    return instance_().tuiInteractive_;
}

[[nodiscard]] bool Config::hint_seen(std::string_view id) {
    namespace fs = std::filesystem;
    auto configPath = instance_().paths_.homeDir / ".xlings.json";
    std::error_code ec;
    if (!fs::exists(configPath, ec)) return false;
    try {
        auto content = platform::read_file_to_string(configPath.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return false;
        auto it = json.find("hintsSeen");
        if (it == json.end() || !it->is_array()) return false;
        for (const auto& e : *it) {
            if (e.is_string() && e.get<std::string>() == id) return true;
        }
    } catch (...) {}
    return false;
}

[[nodiscard]] std::filesystem::path Config::resolve_theme_path() {
    return resolve_theme_path_for(instance_().theme_);
}

[[nodiscard]] std::filesystem::path Config::resolve_theme_path_for(std::string value) {
    namespace fs = std::filesystem;
    auto& self = instance_();
    if (value.empty() || value == "default") return {};

    const bool looksLikePath = value.contains('/') || value.contains('\\')
                            || value.ends_with(".json") || value.starts_with(".");
    if (!looksLikePath) {
        // Bare name: one of the shipped files. Not searched anywhere else --
        // a name that resolves differently depending on the working directory
        // is how a config value stops being reproducible.
        return self.paths_.homeDir / "config" / "themes" / (value + ".json");
    }

    fs::path source(value);
    if (source.is_absolute()) return source.lexically_normal();
    // Relative to whichever config claimed it. `hasProjectConfig_` is the same
    // discriminator resolve_repo_source uses, so the two path-valued keys
    // cannot disagree about what "./x" means.
    const auto base = (self.hasProjectConfig_ && !self.forceGlobalScope_
                       && !self.projectDir_.empty())
        ? self.projectDir_ : self.paths_.homeDir;
    return (base / source).lexically_normal();
}

[[nodiscard]] std::vector<std::string> Config::resource_servers(std::string_view mirror) {
    instance_().ensure_index_config_();
    return instance_().candidate_resource_servers_for_(mirror);
}

[[nodiscard]] std::vector<std::string> Config::resource_servers_with_cross_region(std::string_view mirror) {
    instance_().ensure_index_config_();
    return instance_().all_resource_servers_for_(mirror);
}

[[nodiscard]] std::string Config::resource_server(std::string_view mirror) {
    instance_().ensure_index_config_();
    return instance_().selected_resource_server_for_(mirror);
}

[[nodiscard]] std::vector<ArtifactBase> Config::index_bases() {
    instance_().ensure_index_config_();
    return instance_().indexBases_;
}

[[nodiscard]] xvm::VersionDB Config::versions() {
    auto& self = instance_();
    self.ensure_global_versions_();
    return merged_versions(self.globalVersions_, self.projectVersions_);
}

[[nodiscard]] std::optional<xvm::VInfo> Config::find_vinfo(const std::string& target) {
    return instance_().find_vinfo_(target);
}

[[nodiscard]] xvm::VersionDB& Config::versions_mut() {
    auto& self = instance_();
    if (self.forceGlobalScope_ || !self.hasProjectConfig_) {
        self.ensure_global_versions_();
        return self.globalVersions_;
    }
    return self.projectVersions_;
}

[[nodiscard]] const xvm::VersionDB& Config::global_versions() {
    instance_().ensure_global_versions_();
    return instance_().globalVersions_;
}

[[nodiscard]] const xvm::Workspace& Config::global_workspace() {
    instance_().ensure_global_workspace_();
    return instance_().globalWorkspace_;
}

[[nodiscard]] bool Config::global_workspace_observed() {
    instance_().ensure_global_workspace_();
    return instance_().globalWorkspaceObserved_;
}

[[nodiscard]] const xvm::VersionDB& Config::project_versions() { return instance_().projectVersions_; }

[[nodiscard]] std::filesystem::path Config::global_data_dir() {
    return instance_().paths_.dataDir;
}

[[nodiscard]] std::filesystem::path Config::project_data_dir() {
    return instance_().project_data_dir_();
}

[[nodiscard]] std::filesystem::path Config::project_home_dir() {
    return instance_().project_home_dir_();
}

[[nodiscard]] std::filesystem::path Config::project_state_path() {
    return instance_().project_state_path_();
}

[[nodiscard]] std::filesystem::path Config::project_manifest_path() {
    return instance_().project_manifest_path_();
}

[[nodiscard]] std::filesystem::path Config::effective_data_dir() {
    auto& self = instance_();
    if (self.hasProjectConfig_ && !self.projectDir_.empty() && !self.projectIndexRepos_.empty()) {
        return self.project_data_dir_();
    }
    return self.paths_.dataDir;
}

[[nodiscard]] const std::vector<IndexRepo>& Config::global_index_repos() {
    instance_().ensure_index_config_();
    return instance_().globalIndexRepos_;
}

[[nodiscard]] const std::vector<IndexRepo>& Config::project_index_repos() {
    return instance_().projectIndexRepos_;
}

[[nodiscard]] const std::vector<IndexRepo>& Config::index_repos() {
    auto& self = instance_();
    if (self.hasProjectConfig_ && !self.projectIndexRepos_.empty()) {
        return self.projectIndexRepos_;
    }
    self.ensure_index_config_();
    return self.globalIndexRepos_;
}

[[nodiscard]] std::filesystem::path Config::repo_dir_for(const IndexRepo& repo,
                                                            bool projectScope) {
    auto root = projectScope ? project_data_dir() : global_data_dir();
    auto dirName = repo.name == DEFAULT_INDEX_REPO_NAME ? DEFAULT_INDEX_REPO_DIR : repo.name;
    return root / dirName;
}

[[nodiscard]] std::filesystem::path Config::resolve_repo_source(const IndexRepo& repo,
                                                                   bool projectScope) {
    namespace fs = std::filesystem;
    auto value = repo.url;
    if (value.rfind("file://", 0) == 0) {
        value.erase(0, 7);
#ifdef _WIN32
        // file:///C:/path → /C:/path after erase; strip leading '/' before drive letter
        if (value.size() >= 3 && value[0] == '/' && std::isalpha(static_cast<unsigned char>(value[1])) && value[2] == ':') {
            value.erase(0, 1);
        }
#endif
        return fs::path(value).lexically_normal();
    }
    if (value.find("://") != std::string::npos) {
        return {};
    }

    fs::path source(value);
    if (source.is_absolute()) return source.lexically_normal();

    auto base = projectScope ? project_dir() : paths().homeDir;
    if (base.empty()) return source.lexically_normal();
    return (base / source).lexically_normal();
}

[[nodiscard]] bool Config::is_local_repo_source(const IndexRepo& repo,
                                                   bool projectScope) {
    return !resolve_repo_source(repo, projectScope).empty();
}

[[nodiscard]] const std::filesystem::path& Config::project_dir() {
    return instance_().projectDir_;
}

[[nodiscard]] ProjectSubosMode Config::project_subos_mode() {
    return instance_().projectSubosMode_;
}

[[nodiscard]] const std::string& Config::project_subos_name() {
    return instance_().projectSubosName_;
}

[[nodiscard]] xvm::Workspace Config::effective_workspace() {
    auto& self = instance_();
    self.ensure_global_workspace_();
    // `-g` means "act on the home, not on this project" -- and that has to
    // include which workspace is authoritative, not only which directory
    // the artifacts land in. Honoring it for paths alone is how
    // `install -g` wrote a shim into the home's subos while `remove -g`
    // asked the project's workspace whether the package was there, got
    // "no", and stopped: the same package, present and absent at once,
    // depending on which half of the scope you read.
    if (!self.hasProjectConfig_ || self.forceGlobalScope_) {
        return self.globalWorkspace_;
    }
    return merged_workspace(self.globalWorkspace_,
                            self.projectWorkspace_,
                            self.projectSubosWorkspace_,
                            self.projectSubosMode_);
}

[[nodiscard]] Config::VersionOrigin Config::version_origin(const std::string& target) {
    auto& self = instance_();
    self.ensure_global_workspace_();

    const auto claims = [&](const xvm::Workspace& ws) {
        auto it = ws.find(target);
        return it != ws.end() && !it->second.empty();
    };

    // Walk the layers in the order `merged_workspace` resolves them, LAST
    // writer first: whoever the merge would let win is the one to name. Asking
    // in the other order names a layer that is being overridden, which is
    // worse than saying nothing -- it sends the user to edit a file that is
    // not in control.
    if (self.hasProjectConfig_ && !self.forceGlobalScope_) {
        if (claims(self.projectSubosWorkspace_)) {
            // NOT `fromProjectManifest`: a bare `xlings install` does not read
            // this file, so offering it as the fix would exit 0 and change
            // nothing here.
            return { display_path(self.project_subos_dir_() / ".xlings.json"),
                     false };
        }
        if (claims(self.projectWorkspace_)) {
            // The project manifest is the layer users actually hand-edit, so
            // name the field too -- "which file" is half an answer when the
            // file has a dozen keys.
            //
            // Rendered relative to the project root, not absolutely: the
            // absolute form of a scratch checkout runs past 100 columns on its
            // own, and the reader is standing in that directory. `display_path`
            // is the wrong tool here -- it abbreviates against XLINGS_HOME,
            // which a project directory is not under.
            return { "./.xlings.json  ->  workspace." + target, true };
        }
    }
    if (claims(self.globalWorkspace_)) {
        // The global workspace lives in the ACTIVE SUBOS's file, not in
        // `~/.xlings.json` -- naming the latter would send the user to a file
        // that does not contain the pin.
        //
        // Reachable WITH a project config present: the project may declare
        // other packages and say nothing about this one. That is why the flag
        // below cannot be `hasProjectConfig_`.
        return { display_path(self.paths_.subosDir / ".xlings.json"), false };
    }
    return {};
}

[[nodiscard]] const xvm::Workspace& Config::workspace() {
    auto& self = instance_();
    if (self.forceGlobalScope_ || !self.hasProjectConfig_) {
        self.ensure_global_workspace_();
        return self.globalWorkspace_;
    }
    if (self.projectSubosMode_ == ProjectSubosMode::Named ||
        self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosWorkspace_;
    return self.projectWorkspace_;
}

[[nodiscard]] xvm::Workspace& Config::workspace_mut() {
    auto& self = instance_();
    if (self.forceGlobalScope_ || !self.hasProjectConfig_) {
        self.ensure_global_workspace_();
        return self.globalWorkspace_;
    }
    if (self.projectSubosMode_ == ProjectSubosMode::Named ||
        self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosWorkspace_;
    return self.projectWorkspace_;
}

[[nodiscard]] const xvm::WorkspaceInstalled& Config::workspace_installed() {
    auto& self = instance_();
    if (self.forceGlobalScope_ || !self.hasProjectConfig_) {
        self.ensure_global_workspace_();
        return self.globalInstalled_;
    }
    if (self.projectSubosMode_ == ProjectSubosMode::Named ||
        self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosInstalled_;
    self.ensure_global_workspace_();
    return self.globalInstalled_;
}

[[nodiscard]] xvm::WorkspaceInstalled& Config::workspace_installed_mut() {
    auto& self = instance_();
    if (self.forceGlobalScope_ || !self.hasProjectConfig_) {
        self.ensure_global_workspace_();
        return self.globalInstalled_;
    }
    if (self.projectSubosMode_ == ProjectSubosMode::Named ||
        self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosInstalled_;
    self.ensure_global_workspace_();
    return self.globalInstalled_;
}

[[nodiscard]] bool Config::has_project_config() { return instance_().hasProjectConfig_; }

void Config::set_force_global_scope(bool force) {
    auto& self = instance_();
    if (self.forceGlobalScope_ == force) return;
    self.forceGlobalScope_ = force;
    self.update_effective_paths_();
}

std::string Config::set_active_subos_override(std::string name) {
    auto& self = instance_();
    auto previous = self.activeSubosOverride_;
    if (previous == name) return previous;
    self.activeSubosOverride_ = std::move(name);
    // reload_state_, not just update_effective_paths_: the paths are
    // derived state but so is the WORKSPACE, and that one is read by the
    // activation path. Recomputing only the paths put a package in the
    // right subos and then reported it absent, because the two readers
    // were looking at different subos.
    self.reload_state_();
    return previous;
}

std::filesystem::path Config::subos_dir(const std::string& name) {
    return instance_().paths_.homeDir / "subos" / name;
}

[[nodiscard]] std::filesystem::path Config::global_subos_dir() {
    return instance_().global_subos_dir_();
}

[[nodiscard]] std::filesystem::path Config::global_subos_bin_dir() {
    return global_subos_dir() / "bin";
}

[[nodiscard]] std::filesystem::path Config::xvm_artifact_subos_dir() {
    return instance_().resolve_subos_scope_().root;
}

[[nodiscard]] SubosScope Config::subos_scope() {
    return instance_().resolve_subos_scope_();
}

std::vector<std::string> Config::list_subos_names() {
    std::vector<std::string> names;
    auto dir = instance_().paths_.homeDir / "subos";
    if (!std::filesystem::exists(dir)) return names;
    for (auto& entry : platform::dir_entries(dir)) {
        if (entry.is_directory()) {
            auto name = entry.path().filename().string();
            if (name != "current") names.push_back(name);
        }
    }
    std::ranges::sort(names);
    return names;
}

void Config::save_versions() {
    namespace fs = std::filesystem;
    auto& self = instance_();
    bool useGlobal = self.forceGlobalScope_ || !self.hasProjectConfig_ || self.projectDir_.empty();
    if (useGlobal) self.ensure_global_versions_();
    auto configPath = useGlobal
        ? self.paths_.homeDir / ".xlings.json"
        : self.project_state_path_();

    if (!useGlobal) {
        auto projectHomeDir = self.project_home_dir_();
        if (!projectHomeDir.empty()) fs::create_directories(projectHomeDir);
    }

    nlohmann::json json;
    if (fs::exists(configPath)) {
        try {
            auto content = platform::read_file_to_string(configPath.string());
            json = nlohmann::json::parse(content, nullptr, false);
            if (json.is_discarded()) json = nlohmann::json::object();
        } catch (...) { json = nlohmann::json::object(); }
    }

    auto& versions = useGlobal ? self.globalVersions_ : self.projectVersions_;
    json["versions"] = xvm::versions_to_json(versions);

    if (useGlobal) {
        // The DB file gets its own copy of the versions map
        // (load_versions_json reads it in preference to the home config's
        // `versions` field, which stays for older clients). Written BEFORE
        // the home config: a crash between the two writes leaves the file
        // newer than the config, and readers prefer the file -- the other
        // order leaves them preferring an older config.
        //
        // If the file cannot be written (read-only home, full disk), any
        // stale copy is removed so the home config remains the authority
        // readers fall back to; a DB file shadowing newer config content
        // would be exactly the divergence the fallback exists to avoid.
        auto dbPath = xlings::versions_db_path(self.paths_.homeDir);
        std::error_code ec;
        fs::create_directories(dbPath.parent_path(), ec);
        try {
            platform::write_file_atomic(dbPath.string(),
                                        xvm::versions_to_json(versions).dump(2));
        } catch (const std::exception& e) {
            log::warn("could not write {} ({}); the home config's versions "
                      "field stays the authoritative copy",
                      dbPath.string(), e.what());
            std::error_code rmEc;
            fs::remove(dbPath, rmEc);
        }

        // The program-name index the cheap home-config capture answers
        // "does this home know this program" from, without lexing
        // `versions`. Written in the same dump as `versions`, so this
        // writer cannot make the two diverge. (A hand-edited config can;
        // the index is a fast path whose miss falls through to the real
        // database, never an authority.)
        json["dbIndex"] = xvm::program_index_to_json(versions);
    }
    platform::write_string_to_file(configPath.string(), json.dump(2));
    // The home config was just parsed and written whole; what it now says
    // about known projects is `json`. Taken after the write, under the state
    // lock every caller holds, so the next rebuild's stat matches it.
    if (useGlobal) {
        if (const auto after = file_stat_(configPath)) {
            self.remember_known_projects_(json, after->first, after->second);
        }
    }
}

namespace {

// RMW of `~/.xlings.json` under the home-wide state lock, for the three
// near-every-command writers below (the client-version stamp, the
// verified-version stamp, the hints-seen memo) -- the same file the
// versions DB lives in, mutated without the lock `install`/`remove`/`use`
// already take for it. `list` and `use` now call `notice::notice_once` on
// nearly every invocation (2026.9.12), and a `list` racing an `install`'s
// own read-modify-write of this file was a plain lost update: two readers
// of the old bytes, two writers of a new version each containing only
// their own change, last writer wins over the versions DB, not just the
// field this function meant to touch.
//
// Deliberately NOT worth what `install`/`remove` pay to take this lock.
// None of these three writes is worth blocking a `list` over, so the
// timeout is short and the failure mode is silent: skip the write, let the
// note repeat (or the version stamp lag) until the next call gets a clear
// run at the lock. A LOST hint is an inconvenience; a lost versions-DB
// entry -- which is what writing without the lock risked -- is not.
//
// `XLINGS_LOCK_TIMEOUT` is cleared for the one call below and restored
// immediately after. That variable exists so a machine doing a genuinely
// long install can wait longer than the ten-minute default for ANOTHER
// xlings to get out of the way -- exactly the wait this helper must not
// inherit, since honouring it here would turn a passive version-stamp
// write into another blocked-on-that-same-install command.
//
// Reentry is handled by `acquire_state_lock` itself, not by this function:
// its marker is a process environment variable, read the same way whether
// the holder is an ancestor process or an outer call further up THIS
// process's own stack, so a caller that already holds the lock (a doctor
// `--fix` stamping the home at the end of a repair pass it took the lock
// for) gets `inherited()` back immediately -- no second flock, no wait, no
// deadlock.
//
// `mutate` receives the parsed document (a fresh empty object when the
// file is absent) and returns false to refuse the write outright -- what
// every one of these three writers already did for a file that exists but
// does not parse as JSON: overwriting it would trade a stale field for a
// lost versions DB, and that is never the right trade. Refusing (parse
// failure or `mutate` declining) is reported the same way it always was --
// success, having done nothing. A genuine write I/O failure still comes
// back as an error, exactly as before this lock existed. The one new
// outcome is the lock itself: unlike the other two refusals, "another
// xlings has this home right now" is worth telling a caller that already
// has somewhere to put it (doctor's stamp-failure note), so it comes back
// as an error too rather than being silently folded into "did nothing".
std::expected<void, std::string> rmw_home_config_locked_(
        const std::filesystem::path& homeDir,
        const std::filesystem::path& configPath,
        const std::function<bool(nlohmann::json&)>& mutate) {
    constexpr auto kTimeout = std::chrono::seconds{2};

    const auto savedTimeout =
        utils::get_env_or_default(std::string(xvm::lock_timeout_env()));
    platform::set_env_variable(std::string(xvm::lock_timeout_env()), "");
    auto lock = xvm::acquire_state_lock(homeDir, kTimeout);
    platform::set_env_variable(std::string(xvm::lock_timeout_env()),
                               savedTimeout);
    if (!lock) return std::unexpected(lock.error());

    nlohmann::json json = nlohmann::json::object();
    if (std::filesystem::exists(configPath)) {
        try {
            auto content = platform::read_file_to_string(configPath.string());
            json = nlohmann::json::parse(content, nullptr, false);
            if (json.is_discarded() || !json.is_object()) return {};
        } catch (...) { return {}; }
    }

    if (!mutate(json)) return {};

    try {
        platform::write_string_to_file(configPath.string(), json.dump(2));
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    }
    return {};
}

}  // namespace

[[nodiscard]] std::string Config::recorded_client_version() {
    namespace fs = std::filesystem;
    auto configPath = instance_().paths_.homeDir / ".xlings.json";
    if (!fs::exists(configPath)) return {};
    try {
        auto content = platform::read_file_to_string(configPath.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return {};
        auto it = json.find("version");
        if (it == json.end() || !it->is_string()) return {};
        return it->get<std::string>();
    } catch (...) { return {}; }
}

std::expected<void, std::string>
Config::record_client_version(const std::string& version) {
    auto& self = instance_();
    auto configPath = self.paths_.homeDir / ".xlings.json";
    // Under the state lock (see rmw_home_config_locked_): this is the same
    // file the versions DB lives in, and a mutating command running at the
    // same moment must not have its own write clobbered by this one.
    return rmw_home_config_locked_(self.paths_.homeDir, configPath,
                                   [&](nlohmann::json& json) {
                                       json["version"] = version;
                                       return true;
                                   });
}

[[nodiscard]] std::string Config::recorded_verified_version() {
    namespace fs = std::filesystem;
    auto configPath = instance_().paths_.homeDir / ".xlings.json";
    if (!fs::exists(configPath)) return {};
    try {
        auto content = platform::read_file_to_string(configPath.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return {};
        auto it = json.find("verifiedBy");
        if (it == json.end() || !it->is_string()) return {};
        return it->get<std::string>();
    } catch (...) { return {}; }
}

std::expected<void, std::string>
Config::record_verified_version(const std::string& version) {
    auto& self = instance_();
    auto configPath = self.paths_.homeDir / ".xlings.json";
    // Under the state lock: see record_client_version and
    // rmw_home_config_locked_. `self doctor --fix` stamps this at the very
    // end of a repair pass, by which point every lock this same process
    // took for its own repairs has already gone out of scope -- so this is
    // an ordinary, uncontended lock/unlock in the common case, not a
    // reentrant one.
    return rmw_home_config_locked_(self.paths_.homeDir, configPath,
                                   [&](nlohmann::json& json) {
                                       json["verifiedBy"] = version;
                                       return true;
                                   });
}

void Config::mark_hint_seen(std::string_view id) {
    auto& self = instance_();
    auto configPath = self.paths_.homeDir / ".xlings.json";
    // Under the state lock: see record_client_version and
    // rmw_home_config_locked_. This one fires from `notice::notice_once`,
    // called by `list`/`use`/`install`'s own upgrade notice on nearly every
    // invocation -- the highest-frequency of the three writers this lock
    // protects, and a lost `mark` here just means the same note shows up
    // again next time. The error is not surfaced: this function always
    // returned void, and a repeated hint was already the accepted failure
    // mode of its pre-lock parse refusal.
    std::string idCopy(id);
    (void)rmw_home_config_locked_(self.paths_.homeDir, configPath,
                                  [&](nlohmann::json& json) {
                                      auto& seen = json["hintsSeen"];
                                      if (!seen.is_array()) {
                                          seen = nlohmann::json::array();
                                      }
                                      for (const auto& e : seen) {
                                          if (e.is_string()
                                              && e.get<std::string>() == idCopy) {
                                              return false;  // already there
                                          }
                                      }
                                      seen.push_back(idCopy);
                                      return true;
                                  });
}

void Config::remember_known_projects_(const nlohmann::json& json,
                                      std::uintmax_t size,
                                      std::filesystem::file_time_type mtime) {
    KnownProjectsSnapshot_ snap;
    snap.valid = true;
    snap.size = size;
    snap.mtime = mtime;
    if (auto it = json.find("knownProjects");
        it != json.end() && it->is_object()) {
        for (auto e = it->begin(); e != it->end(); ++e) {
            if (e.key().empty()) continue;
            std::string stamp;
            if (e->is_object()) {
                if (auto ls = e->find("lastSeen"); ls != e->end() && ls->is_string()) {
                    stamp = ls->get<std::string>();
                }
            }
            snap.lastSeen.emplace(e.key(), std::move(stamp));
        }
    }
    knownProjects_ = std::move(snap);
}

// Re-read the home config's `knownProjects` only when the file is not the one
// the snapshot was taken from. The stat is taken BEFORE the read: a write that
// lands between the two leaves new content under an old stat, and the next
// call re-reads -- the harmless direction. The opposite order would pair old
// content with a new stat and keep it.
void Config::refresh_known_projects_() {
    namespace fs = std::filesystem;
    auto configPath = paths_.homeDir / ".xlings.json";
    const auto before = file_stat_(configPath);
    if (!before) {
        knownProjects_ = {};
        return;
    }
    if (knownProjects_.valid && knownProjects_.size == before->first
        && knownProjects_.mtime == before->second) {
        return;
    }
    try {
        auto content = platform::read_file_to_string(configPath.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            knownProjects_ = {};
            return;
        }
        remember_known_projects_(json, before->first, before->second);
    } catch (...) {
        knownProjects_ = {};
    }
}

[[nodiscard]] std::vector<std::filesystem::path> Config::known_projects() {
    auto& self = instance_();
    self.refresh_known_projects_();
    std::vector<std::filesystem::path> out;
    out.reserve(self.knownProjects_.lastSeen.size());
    for (const auto& [key, _] : self.knownProjects_.lastSeen) out.push_back(key);
    std::ranges::sort(out);
    return out;
}

void Config::register_known_project(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    if (dir.empty()) return;

    // Absolute and normalised, because this key is what the rebuild reads a
    // state file from. A relative key recorded from one cwd names a different
    // directory read from another.
    std::error_code ec;
    auto key = fs::weakly_canonical(dir, ec);
    if (ec || key.empty()) key = fs::absolute(dir, ec);
    if (ec || key.empty()) return;

    std::string stamp;
    {
        auto now = std::chrono::system_clock::now();
        stamp = std::format("{:%FT%TZ}",
                            std::chrono::floor<std::chrono::seconds>(
                                std::chrono::time_point_cast<
                                    std::chrono::seconds>(now)));
    }

    // Written at most once a day per project. This runs on every routing-table
    // rebuild in project scope -- once per installed package -- and each write
    // is a read, parse and rewrite of the whole home config (3.6 MB on a
    // measured home) to move `lastSeen` forward by a few seconds. Nothing reads
    // `lastSeen`; a day's resolution is all it can carry anyway.
    auto& self = instance_();
    self.refresh_known_projects_();
    if (auto it = self.knownProjects_.lastSeen.find(key);
        it != self.knownProjects_.lastSeen.end()
        && it->second.size() >= 10 && stamp.size() >= 10
        && it->second.compare(0, 10, stamp, 0, 10) == 0) {
        return;
    }

    auto configPath = self.paths_.homeDir / ".xlings.json";
    nlohmann::json json = nlohmann::json::object();
    if (fs::exists(configPath)) {
        try {
            auto content = platform::read_file_to_string(configPath.string());
            json = nlohmann::json::parse(content, nullptr, false);
            // Same refusal as record_client_version / mark_hint_seen: never
            // replace a document we could not parse. Trading an unregistered
            // project for a lost versions DB is not a trade.
            if (json.is_discarded() || !json.is_object()) return;
        } catch (...) { return; }
    }

    auto& projects = json["knownProjects"];
    if (!projects.is_object()) projects = nlohmann::json::object();
    projects[key.string()] = nlohmann::json{{"lastSeen", stamp}};

    platform::write_string_to_file(configPath.string(), json.dump(2));
    if (const auto after = file_stat_(configPath)) {
        self.remember_known_projects_(json, after->first, after->second);
    }
}

std::filesystem::path Config::workspace_config_path(bool createDirs) {
    namespace fs = std::filesystem;
    auto& self = instance_();
    bool useProject = self.hasProjectConfig_ && !self.forceGlobalScope_;

    const auto ensure_project_subos_dirs = [&](const fs::path& projSubosDir) {
        if (!createDirs) return;
        std::error_code ec;
        fs::create_directories(projSubosDir, ec);
        for (auto sub : {"bin", "lib", "usr", "generations"}) {
            fs::create_directories(projSubosDir / sub, ec);
        }
    };
    const auto ensure_project_home = [&] {
        if (!createDirs) return;
        auto projectHomeDir = self.project_home_dir_();
        if (projectHomeDir.empty()) return;
        std::error_code ec;
        fs::create_directories(projectHomeDir, ec);
    };

    if (useProject && self.projectSubosMode_ == ProjectSubosMode::Named) {
        auto projSubosDir = self.project_subos_dir_();
        ensure_project_subos_dirs(projSubosDir);
        return projSubosDir / ".xlings.json";
    }
    if (useProject && self.projectSubosMode_ == ProjectSubosMode::Anonymous) {
        ensure_project_home();
        ensure_project_subos_dirs(self.project_subos_dir_());
        return self.project_state_path_();
    }
    if (useProject && !self.projectDir_.empty()) {
        ensure_project_home();
        return self.project_state_path_();
    }
    // useProject=false here — either no project config, or forceGlobalScope_
    // is on (e.g. `xlings install -g`). Both mean "act on global scope".
    //
    // `global_subos_dir_()`, never `paths_.activeSubos`: inside an anonymous
    // project the latter is "_" and stays "_" even after forceGlobalScope_
    // flips useProject to false. Writing to `~/.xlings/subos/_/.xlings.json`
    // then fails the save — observed as `Failed to write file` during `self
    // install`'s patchelf step when run from inside a project tree.
    return self.global_subos_dir_() / ".xlings.json";
}

void Config::save_workspace() {
    namespace fs = std::filesystem;
    auto& self = instance_();
    bool useProject = self.hasProjectConfig_ && !self.forceGlobalScope_;
    fs::path subosConfigPath = workspace_config_path(/*createDirs=*/true);

    // XLINGS_HOME is a supported explicit scope, not proof that `self
    // init` has already run. A first package install into a cold home
    // reaches this writer after the payload is complete; create the state
    // directory before persisting installed/active ownership so success
    // is never followed by a spurious write failure (issue #471).
    fs::create_directories(subosConfigPath.parent_path());

    nlohmann::json json = nlohmann::json::object();
    if (fs::exists(subosConfigPath)) {
        bool parsedOk = false;
        try {
            auto content = platform::read_file_to_string(subosConfigPath.string());
            auto parsed = nlohmann::json::parse(content, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                json = std::move(parsed);
                parsedOk = true;
            }
        } catch (...) { /* parsedOk stays false */ }
        if (!parsedOk) {
            // Refuse rather than replace: an unreadable file is not an
            // empty one. Blanking it here would discard `subos_info`, envs,
            // and anything else this write does not itself own -- turning
            // "this subos could not be read" into "this subos is now
            // empty", the one failure mode a repair cannot walk back.
            // profile::save_subos_workspace already makes this same
            // promise for every OTHER subos's file (doctor's cross-subos
            // repairs write through it); this is THIS subos's own writer,
            // reached on nearly every install/remove/use, making it too.
            log::warn(
                "{}: could not be parsed as JSON; leaving it untouched "
                "rather than overwriting it with a blank workspace. Run "
                "`xlings self doctor` to see what needs repair.",
                display_path(subosConfigPath));
            return;
        }
    }

    // All four destination paths above target subos-side files
    // (named project subos, anonymous project subos / state file, or
    // global subos directory). The user-authored project manifest
    // (`<proj>/.xlings.json`) is read-only from save_workspace's
    // perspective and never reached here. So we always emit the
    // 0.4.19+ C2 form via subos_workspace_to_json — backward-compat
    // for legacy string-form values is in the *read* path
    // (subos_workspace_from_json).
    xvm::SubosWorkspace sws;
    if (useProject &&
        (self.projectSubosMode_ == ProjectSubosMode::Named ||
         self.projectSubosMode_ == ProjectSubosMode::Anonymous)) {
        sws.active = self.projectSubosWorkspace_;
        sws.installed = self.projectSubosInstalled_;
        sws.configured = self.projectSubosConfigured_;
    } else if (useProject) {
        // Reachable only via the third save-path branch above (project
        // mode without a subos mode, currently unreachable in practice
        // because load_project_config_from_dir_ always forces
        // Anonymous when subos is unset). Write the project manifest
        // workspace through with no installed[] info.
        sws.active = self.projectWorkspace_;
    } else {
        sws.active = self.globalWorkspace_;
        sws.installed = self.globalInstalled_;
        sws.configured = self.globalConfigured_;
    }
    json["workspace"] = xvm::subos_workspace_to_json(sws);
    if (sws.configured.empty()) {
        json.erase("configured");
    } else {
        json["configured"] = sws.configured;
    }
    platform::write_string_to_file(subosConfigPath.string(), json.dump(2));
}

// The map `save_workspace` writes for the scope this command acts on -- the
// same selection as workspace_installed_mut(), so a record always lands next
// to the installed[] it is checked against.
std::map<std::string, int>& Config::configured_mut_() {
    if (forceGlobalScope_ || !hasProjectConfig_) {
        ensure_global_workspace_();
        return globalConfigured_;
    }
    if (projectSubosMode_ == ProjectSubosMode::Named ||
        projectSubosMode_ == ProjectSubosMode::Anonymous) return projectSubosConfigured_;
    ensure_global_workspace_();
    return globalConfigured_;
}

[[nodiscard]] std::optional<int> Config::configured_revision(std::string_view identity) {
    const auto& map = instance_().configured_mut_();
    if (auto it = map.find(std::string(identity)); it != map.end()) return it->second;
    return std::nullopt;
}

void Config::mark_configured(const std::string& identity, int revision) {
    instance_().configured_mut_()[identity] = revision;
}

bool Config::forget_configured(std::string_view identity) {
    return instance_().configured_mut_().erase(std::string(identity)) > 0;
}

bool Config::forget_configured_binding(std::string_view binding) {
    // A binding is "<name>@<version>" with no namespace; a record key is
    // "<ns>:<name>@<version>". Every namespace's record of that name and
    // version goes -- the binding that was unbound names no namespace to keep.
    auto& map = instance_().configured_mut_();
    bool any = false;
    for (auto it = map.begin(); it != map.end();) {
        const auto& key = it->first;
        const auto colon = key.find(':');
        const std::string_view bare = colon == std::string::npos
            ? std::string_view(key) : std::string_view(key).substr(colon + 1);
        if (bare == binding) { it = map.erase(it); any = true; }
        else ++it;
    }
    return any;
}

void Config::print_paths() {
    auto& p = paths();
    std::println(stdout, "XLINGS_HOME:     {}", p.homeDir.string());
    std::println(stdout, "XLINGS_DATA:     {}", display_path(p.dataDir));
    if (has_project_config() && !project_index_repos().empty()) {
        std::println(stdout, "XLINGS_DATA_PROJECT: {}", display_path(project_data_dir()));
    }
    std::println(stdout, "XLINGS_SUBOS:    {}", display_path(p.subosDir));
    std::println(stdout, "  activeSubos:   {}", p.activeSubos);
    std::println(stdout, "  selfContained: {}", p.selfContained);
    std::println(stdout, "  bin:           {}", display_path(p.binDir));
}

} // namespace xlings
