export module xlings.core.config;

import std;

import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.libs.tinyhttps;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

namespace xlings {

export struct Info {
    static constexpr std::string_view VERSION = "2026.8.1.1";
    static constexpr std::string_view REPO = "https://github.com/openxlings/xlings";
};

export struct IndexRepo {
    std::string name;
    std::string url;
    std::string artifactBase;  // #377: resolved artifact base URL ("" = git-only)
    std::string source;        // #377: per-repo override: "" | "auto" | "artifact" | "git"
};

// #377: parse index_repos entries. `artifact` is a flat string or a region
// object {"GLOBAL":..,"CN":..} (same shape as xim.index-base), resolved
// against `mirror` with GLOBAL fallback. `source` optionally overrides the
// global index source for this repo only.
export std::vector<IndexRepo> parse_index_repos_json(const nlohmann::json& json,
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
        out.push_back(std::move(repo));
    }
    return out;
}

using MirrorServerMap = std::unordered_map<std::string, std::vector<std::string>>;

// Which subos a process is acting on: a name and the root of its tree.
//
// One value rather than two lookups. Every "which subos" question in the code
// base used to be answered by one of seven spellings, and the two that
// mattered disagreed (see Config::resolve_subos_scope_). Handing back both
// halves together removes the shape of that bug: a caller cannot take the
// name from one resolution and the root from another.
export struct SubosScope {
    std::string name;
    std::filesystem::path root;
};

export enum class ProjectSubosMode {
    None,
    Anonymous,
    Named,
};

export class Config {
public:
    struct PathInfo {
        std::filesystem::path homeDir;      // XLINGS_HOME (~/.xlings)
        std::filesystem::path dataDir;      // $homeDir/data - global shared
        std::filesystem::path subosDir;     // effective subos dir
        std::filesystem::path binDir;       // $subosDir/bin
        std::filesystem::path libDir;       // $subosDir/lib
        std::string           activeSubos;  // effective active subos name
        bool                  selfContained = false;
    };

    // Owner-anchored shim dispatch (0.4.48; see
    // .agents/docs/2026-06-04-shim-owner-anchoring-design.md): inject the
    // dispatch home BEFORE first Config use. main.cpp calls this for shim
    // invocations only — the xlings CLI keeps the env-first resolution.
    // Has no effect once the singleton is constructed.
    static void override_home(const std::filesystem::path& home) {
        home_override_() = home;
    }

private:
    static std::optional<std::filesystem::path>& home_override_() {
        static std::optional<std::filesystem::path> v;
        return v;
    }

    PathInfo paths_;
    std::string mirror_;
    std::string indexBase_;   // xim.index-base override (region-resolved); empty = default xlings-res
    std::string lang_;
    std::string globalActiveSubos_ = "default";
    xvm::VersionDB globalVersions_;
    xvm::VersionDB projectVersions_;
    xvm::Workspace globalWorkspace_;
    xvm::Workspace projectWorkspace_;       // from project .xlings.json
    xvm::Workspace projectSubosWorkspace_;  // from project-local subos file
    // Per-subos installed[] sets, paired with the matching workspace map.
    // Only subos files (global subos and project subos) carry installed[];
    // the project manifest's workspace is intent-only and has no
    // corresponding installed map (Plan 3 of the C2 schema design).
    xvm::WorkspaceInstalled globalInstalled_;
    xvm::WorkspaceInstalled projectSubosInstalled_;
    bool hasProjectConfig_ = false;
    bool forceGlobalScope_ = false;
    std::filesystem::path projectDir_;      // directory containing project .xlings.json
    std::vector<IndexRepo> globalIndexRepos_;
    std::vector<IndexRepo> projectIndexRepos_;
    MirrorServerMap globalResourceServers_;
    MirrorServerMap projectResourceServers_;
    ProjectSubosMode projectSubosMode_ = ProjectSubosMode::None;
    std::string projectSubosName_;
    mutable std::mutex resourceServerMutex_;
    mutable std::unordered_map<std::string, std::string> selectedResourceServerCache_;

    static constexpr std::string_view DEFAULT_INDEX_REPO_NAME = "xim";
    static constexpr std::string_view DEFAULT_INDEX_REPO_DIR = "xim-pkgindex";

    static std::vector<IndexRepo> default_global_index_repos_(const std::string& mirror) {
        std::string url = "https://github.com/openxlings/xim-pkgindex.git";
        if (mirror == "CN") {
            url = "https://gitee.com/sunrisepeak/xim-pkgindex.git";
        }
        return { IndexRepo{std::string(DEFAULT_INDEX_REPO_NAME), url} };
    }

    static MirrorServerMap default_resource_servers_() {
        return {
            { "GLOBAL", { "https://github.com/xlings-res" } },
            { "CN", { "https://gitcode.com/xlings-res" } },
        };
    }

    // Resolve xim.index-base from a config json. Accepts a flat string or a
    // region object {"GLOBAL":"...","CN":"..."}. Lets a deployment point the
    // index pointer+artifact at a self-hosted server without code changes.
    static std::string resolve_index_base_(const nlohmann::json& json, const std::string& mirror) {
        if (!json.contains("xim") || !json["xim"].is_object()) return {};
        auto& xim = json["xim"];
        if (!xim.contains("index-base")) return {};
        auto& ib = xim["index-base"];
        if (ib.is_string()) return ib.get<std::string>();
        if (ib.is_object()) {
            std::string key = mirror.empty() ? "GLOBAL" : mirror;
            if (ib.contains(key) && ib[key].is_string()) return ib[key].get<std::string>();
            if (ib.contains("GLOBAL") && ib["GLOBAL"].is_string()) return ib["GLOBAL"].get<std::string>();
        }
        return {};
    }

    static std::vector<std::string> parse_server_list_(const nlohmann::json& value) {
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

    static void merge_resource_servers_into_(MirrorServerMap& dst, const MirrorServerMap& src) {
        for (auto& [mirror, servers] : src) {
            if (!servers.empty()) dst[mirror] = servers;
        }
    }

    static void load_resource_servers_from_json_(const nlohmann::json& json,
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

    // Read a subos `.xlings.json` workspace section. Returns the new
    // SubosWorkspace bundle so callers get both `active` (Workspace) and
    // `installed[]` (WorkspaceInstalled). Legacy string-form values still
    // parse cleanly — installed[] just stays empty until the next save
    // re-emits the file in C2 form.
    static xvm::SubosWorkspace load_workspace_from_file_(const std::filesystem::path& path) {
        namespace fs = std::filesystem;
        if (!fs::exists(path)) return {};
        try {
            auto content = platform::read_file_to_string(path.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (!json.is_discarded() && json.contains("workspace") && json["workspace"].is_object()) {
                return xvm::subos_workspace_from_json(json["workspace"]);
            }
        } catch (...) {}
        return {};
    }

    static std::string load_project_subos_name_(const nlohmann::json& json) {
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

    static void merge_versions_into_(xvm::VersionDB& dst, const xvm::VersionDB& src) {
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

    static void merge_workspace_into_(xvm::Workspace& dst, const xvm::Workspace& src) {
        for (auto& [target, version] : src) {
            dst[target] = version;
        }
    }

    static std::string effective_mirror_name_(std::string_view mirror,
                                              std::string_view fallback) {
        auto name = std::string(mirror.empty() ? fallback : mirror);
        if (name.empty()) return "GLOBAL";
        return name;
    }

    static std::vector<std::string> workspace_targets_from_workspace_(const xvm::Workspace& ws) {
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

    [[nodiscard]] std::filesystem::path project_data_dir_() const {
        return projectDir_.empty() ? std::filesystem::path{} : projectDir_ / ".xlings" / "data";
    }

    [[nodiscard]] std::filesystem::path project_home_dir_() const {
        return projectDir_.empty() ? std::filesystem::path{} : projectDir_ / ".xlings";
    }

    [[nodiscard]] std::filesystem::path project_state_path_() const {
        auto homeDir = project_home_dir_();
        if (homeDir.empty()) return {};
        return homeDir / ".xlings.json";
    }

    [[nodiscard]] std::filesystem::path project_manifest_path_() const {
        return projectDir_.empty() ? std::filesystem::path{} : projectDir_ / ".xlings.json";
    }

    [[nodiscard]] std::filesystem::path project_subos_dir_() const {
        if (projectDir_.empty()) return {};
        if (!projectSubosName_.empty()) return projectDir_ / ".xlings" / "subos" / projectSubosName_;
        if (projectSubosMode_ == ProjectSubosMode::Anonymous) return projectDir_ / ".xlings" / "subos" / "_";
        return {};
    }

    [[nodiscard]] std::filesystem::path global_subos_dir_() const {
        auto activeSubos = utils::get_env_or_default(
            "XLINGS_ACTIVE_SUBOS");
        if (activeSubos.empty()) {
            activeSubos = globalActiveSubos_;
        }
        return paths_.homeDir / "subos" / activeSubos;
    }

    [[nodiscard]] static std::vector<std::string>
    lookup_resource_servers_in_(const MirrorServerMap& source, std::string_view mirror) {
        auto key = effective_mirror_name_(mirror, "GLOBAL");
        if (auto it = source.find(key); it != source.end() && !it->second.empty()) {
            return it->second;
        }
        if (auto it = source.find("DEFAULT"); it != source.end() && !it->second.empty()) {
            return it->second;
        }
        return {};
    }

    [[nodiscard]] std::vector<std::string>
    candidate_resource_servers_for_(std::string_view mirror) const {
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

    // Every resource server worth trying: this region's candidates first, then
    // every other region's, deduplicated.
    //
    // The regional buckets are a routing preference, not a partition of the
    // content -- both hosts mirror the same releases. Treating them as a
    // partition is what turned a mirroring gap into an outage: `xlings-res`
    // publishes to GitHub and then mirrors to GitCode, that second step is
    // manual for large assets (the GitHub runner cannot push them across the
    // border), and until it runs a CN client's candidate list is
    // `{gitcode}` -- one host, no alternative. `2026.7.30.1` shipped with the
    // four tarballs missing from GitCode and CN users got a flat HTTP 404
    // while the same files sat on GitHub, reachable, unlisted.
    //
    // Ordering keeps the preference intact: the cross-region entries are only
    // ever tried after every same-region one has failed, so nobody in CN
    // starts pulling from GitHub while GitCode is serving.
    [[nodiscard]] std::vector<std::string>
    all_resource_servers_for_(std::string_view mirror) const {
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

    static double probe_resource_server_latency_(const std::string& server) {
        return tinyhttps::probe_latency(server, 2000);
    }

    [[nodiscard]] std::string selected_resource_server_for_(std::string_view mirror) const {
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

    // The ONE answer to "which subos is this process acting on".
    //
    // Resolution priority:
    //   1. Project-mode (Named or Anonymous) — strongest, "this directory
    //      asks for subos X" overrides everything else. Unless `-g` was
    //      passed, which is exactly what forceGlobalScope_ means: act on the
    //      home, not on this project.
    //   2. $XLINGS_ACTIVE_SUBOS env var — shell-level override; the shell
    //      profile honors the same priority so PATH and `xpkg install`
    //      target stay in sync.
    //   3. globalActiveSubos_ from ~/.xlings.json — the persistent default
    //      that new shells inherit when no env override is set.
    //
    // Everything that needs a subos goes through here. There used to be two
    // implementations of this question that disagreed: `update_effective_paths_`
    // (which ignored forceGlobalScope_ and was computed once at construction)
    // and `xvm_artifact_subos_dir()` (which honored it and recomputed). They
    // diverged on exactly one case -- `xlings install -g` run inside a project
    // -- and the three paths that matter had picked different ones: install
    // resolved artifacts with the second, while `use` and the removal path
    // read `paths()`, i.e. the first. Installing into one subos and cleaning
    // another is not a bug that can be fixed at a call site; it is what having
    // two authorities means.
    [[nodiscard]] SubosScope resolve_subos_scope_() const {
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

    void update_effective_paths_() {
        auto scope = resolve_subos_scope_();
        paths_.activeSubos = std::move(scope.name);
        paths_.subosDir = std::move(scope.root);
        paths_.binDir = paths_.subosDir / "bin";
        paths_.libDir = paths_.subosDir / "lib";
    }

    Config() {
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
        if (fs::exists(configPath)) {
            try {
                auto content = platform::read_file_to_string(configPath.string());
                auto json    = nlohmann::json::parse(content, nullptr, false);
                if (!json.is_discarded()) {
                    if (json.contains("activeSubos") && json["activeSubos"].is_string()) {
                        auto val = json["activeSubos"].get<std::string>();
                        if (!val.empty()) globalActiveSubos_ = val;
                    }
                    if (json.contains("mirror") && json["mirror"].is_string())
                        mirror_ = json["mirror"].get<std::string>();
                    if (json.contains("lang") && json["lang"].is_string())
                        lang_ = json["lang"].get<std::string>();
                    // Load global versions
                    load_global_versions_from_json_(json);
                    globalIndexRepos_ = parse_index_repos_json(json, mirror_);
                    load_resource_servers_from_json_(json, globalResourceServers_);
                    if (auto v = resolve_index_base_(json, mirror_); !v.empty()) indexBase_ = v;
                }
            } catch (...) {}
        }
        if (globalIndexRepos_.empty()) {
            globalIndexRepos_ = default_global_index_repos_(mirror_);
        } else {
            // Ensure the default index repo is always present when user
            // defines custom index_repos (e.g. adding "ros2").  Without
            // this, user-defined repos replace the default and packages
            // in the primary index (like "python") become unfindable.
            auto defaults = default_global_index_repos_(mirror_);
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

        paths_.dataDir  = paths_.homeDir / "data";
        update_effective_paths_();

        log::debug("config: home={}, selfContained={}", paths_.homeDir.string(), paths_.selfContained);

        // Load subos workspace from the path resolved by
        // update_effective_paths_, which honors XLINGS_ACTIVE_SUBOS env
        // overrides. Using `globalActiveSubos_` directly here (a snapshot
        // of `~/.xlings.json activeSubos`) would silently load the wrong
        // subos's workspace whenever the user is in a spawned subos shell
        // with XLINGS_ACTIVE_SUBOS set, which then corrupts that subos
        // when `xvm use` writes back through save_workspace().
        load_global_workspace_();

        // Load project-level config (walk up from cwd)
        load_project_config_();
        update_effective_paths_();
    }

    void load_project_config_from_dir_(const std::filesystem::path& dir) {
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
                if (json.contains("workspace") && json["workspace"].is_object()) {
                    projectWorkspace_ = xvm::workspace_from_json(json["workspace"]);
                }
                projectIndexRepos_ = parse_index_repos_json(json, mirror_);
                load_resource_servers_from_json_(json, projectResourceServers_);
                if (auto v = resolve_index_base_(json, mirror_); !v.empty()) indexBase_ = v;  // project overrides global
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
                } else {
                    projectSubosMode_ = ProjectSubosMode::Anonymous;
                    if (hasProjectStateJson && projectStateJson.contains("workspace") &&
                        projectStateJson["workspace"].is_object()) {
                        auto sws = xvm::subos_workspace_from_json(projectStateJson["workspace"]);
                        projectSubosWorkspace_ = std::move(sws.active);
                        projectSubosInstalled_ = std::move(sws.installed);
                    } else {
                        auto sws = load_workspace_from_file_(project_subos_dir_() / ".xlings.json");
                        projectSubosWorkspace_ = std::move(sws.active);
                        projectSubosInstalled_ = std::move(sws.installed);
                    }
                }
            }
        } catch (...) {}
    }

    void load_project_config_() {
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

    void load_global_versions_from_json_(const nlohmann::json& json) {
        if (json.contains("versions") && json["versions"].is_object()) {
            globalVersions_ = xvm::versions_from_json(json["versions"]);
        } else {
            globalVersions_.clear();
        }
    }

    void load_global_workspace_() {
        auto subosConfigPath =
            paths_.homeDir / "subos" / paths_.activeSubos / ".xlings.json";
        auto sws = load_workspace_from_file_(subosConfigPath);
        globalWorkspace_ = std::move(sws.active);
        globalInstalled_ = std::move(sws.installed);
    }

    // Re-read the mutable state layers from disk.
    //
    // Config loads state during construction, and main.cpp touches it before
    // dispatching a command, so by the time a mutating command starts it is
    // already holding a snapshot taken outside any lock. Serializing writes
    // alone would not prevent a lost update: both processes would still have
    // read the same old state and the second write would erase the first.
    // Commands re-read through here *after* taking the state lock.
    //
    // Deliberately narrow: versions, workspace and project state only.
    // Mirror, language and index-repo resolution stay as they were resolved
    // at startup -- re-deriving those mid-command would change behavior
    // under the user rather than protect it.
    void reload_state_() {
        namespace fs = std::filesystem;
        auto configPath = paths_.homeDir / ".xlings.json";
        if (fs::exists(configPath)) {
            try {
                auto content = platform::read_file_to_string(configPath.string());
                auto json = nlohmann::json::parse(content, nullptr, false);
                if (!json.is_discarded()) load_global_versions_from_json_(json);
            } catch (...) {}
        } else {
            globalVersions_.clear();
        }
        load_global_workspace_();
        if (hasProjectConfig_) load_project_config_();
        update_effective_paths_();
    }

    static Config& instance_() {
        static Config inst;
        return inst;
    }

public:
    [[nodiscard]] static std::vector<std::string> workspace_install_targets(const xvm::Workspace& ws) {
        return workspace_targets_from_workspace_(ws);
    }

    [[nodiscard]] static xvm::VersionDB merged_versions(const xvm::VersionDB& globalVersions,
                                                        const xvm::VersionDB& projectVersions) {
        auto db = globalVersions;
        merge_versions_into_(db, projectVersions);
        return db;
    }

    [[nodiscard]] static xvm::Workspace merged_workspace(const xvm::Workspace& globalWorkspace,
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

    // Re-read versions/workspace/project state from disk. Call after taking
    // the state lock so a command operates on what is actually stored rather
    // than on the snapshot taken at process start.
    static void reload_state() { instance_().reload_state_(); }

    [[nodiscard]] static const PathInfo& paths() { return instance_().paths_; }

    // Render a path for a human: anything under the xlings home shows as
    // `@xlings/...` instead of its absolute form.
    //
    // Output is full of these -- doctor lists payload and shim paths, config
    // prints the layout, install and error hints quote destinations -- and an
    // absolute home path is both noise and, in shared logs or issue reports,
    // the user's account name. `@xlings` says the one thing that matters
    // about the prefix: it is inside the installation.
    //
    // Display only. `${XLINGS_HOME}` remains the *storage* placeholder in the
    // version database (see expand_path in xvm/db.cppm); the two must not be
    // confused -- one is expanded on read, this one is never read back.
    //
    // A path outside the home is returned unchanged: substituting a prefix
    // that does not apply would be a lie about where the file is.
    [[nodiscard]] static std::string display_path(
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

    [[nodiscard]] static std::string display_path(const std::string& p) {
        return display_path(std::filesystem::path{p});
    }
    [[nodiscard]] static const std::string& mirror() { return instance_().mirror_; }
    [[nodiscard]] static const std::string& lang() { return instance_().lang_; }
    [[nodiscard]] static std::vector<std::string> resource_servers(std::string_view mirror = {}) {
        return instance_().candidate_resource_servers_for_(mirror);
    }
    // Same list, extended with the other regions' servers as a last resort.
    // Use this for download fallbacks; `resource_servers()` remains the
    // "where should this region be pointed" answer used for selection.
    [[nodiscard]] static std::vector<std::string>
    resource_servers_with_cross_region(std::string_view mirror = {}) {
        return instance_().all_resource_servers_for_(mirror);
    }
    [[nodiscard]] static std::string resource_server(std::string_view mirror = {}) {
        return instance_().selected_resource_server_for_(mirror);
    }
    // xim.index-base override (env XLINGS_INDEX_BASE_URL takes precedence in the
    // caller). Empty => default xlings-res raw-pointer + release-artifact path.
    [[nodiscard]] static std::string index_base() { return instance_().indexBase_; }

    // Returns BY VALUE -- global and project state are merged into a fresh
    // map, so there is no long-lived object to hand out a reference to.
    //
    // Bind it to a named local before taking any pointer or reference into
    // it. `get_vinfo(Config::versions(), name)` compiles, and returns a
    // pointer into a temporary that dies at the end of that full expression;
    // the next line then reads freed memory. That was a real crash --
    // `xlings remove glibc` SIGSEGV'd on the musl-static release build and
    // threw bad_alloc on a glibc one, reported 2026-07-28 and present since
    // the fallback was written.
    [[nodiscard]] static xvm::VersionDB versions() {
        auto& self = instance_();
        return merged_versions(self.globalVersions_, self.projectVersions_);
    }
    [[nodiscard]] static xvm::VersionDB& versions_mut() {
        auto& self = instance_();
        if (self.forceGlobalScope_ || !self.hasProjectConfig_) return self.globalVersions_;
        return self.projectVersions_;
    }
    [[nodiscard]] static const xvm::VersionDB& global_versions() { return instance_().globalVersions_; }
    [[nodiscard]] static const xvm::VersionDB& project_versions() { return instance_().projectVersions_; }

    // Effective data dir: project-local if project config exists, otherwise global
    [[nodiscard]] static std::filesystem::path global_data_dir() {
        return instance_().paths_.dataDir;
    }

    [[nodiscard]] static std::filesystem::path project_data_dir() {
        return instance_().project_data_dir_();
    }

    [[nodiscard]] static std::filesystem::path project_home_dir() {
        return instance_().project_home_dir_();
    }

    [[nodiscard]] static std::filesystem::path project_state_path() {
        return instance_().project_state_path_();
    }

    [[nodiscard]] static std::filesystem::path project_manifest_path() {
        return instance_().project_manifest_path_();
    }

    [[nodiscard]] static std::filesystem::path effective_data_dir() {
        auto& self = instance_();
        if (self.hasProjectConfig_ && !self.projectDir_.empty() && !self.projectIndexRepos_.empty()) {
            return self.project_data_dir_();
        }
        return self.paths_.dataDir;
    }

    [[nodiscard]] static const std::vector<IndexRepo>& global_index_repos() {
        return instance_().globalIndexRepos_;
    }

    [[nodiscard]] static const std::vector<IndexRepo>& project_index_repos() {
        return instance_().projectIndexRepos_;
    }

    [[nodiscard]] static const std::vector<IndexRepo>& index_repos() {
        auto& self = instance_();
        if (self.hasProjectConfig_ && !self.projectIndexRepos_.empty()) {
            return self.projectIndexRepos_;
        }
        return self.globalIndexRepos_;
    }

    [[nodiscard]] static std::filesystem::path repo_dir_for(const IndexRepo& repo,
                                                            bool projectScope) {
        auto root = projectScope ? project_data_dir() : global_data_dir();
        auto dirName = repo.name == DEFAULT_INDEX_REPO_NAME ? DEFAULT_INDEX_REPO_DIR : repo.name;
        return root / dirName;
    }

    [[nodiscard]] static std::filesystem::path resolve_repo_source(const IndexRepo& repo,
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

    [[nodiscard]] static bool is_local_repo_source(const IndexRepo& repo,
                                                   bool projectScope) {
        return !resolve_repo_source(repo, projectScope).empty();
    }

    [[nodiscard]] static const std::filesystem::path& project_dir() {
        return instance_().projectDir_;
    }

    [[nodiscard]] static ProjectSubosMode project_subos_mode() {
        return instance_().projectSubosMode_;
    }

    [[nodiscard]] static const std::string& project_subos_name() {
        return instance_().projectSubosName_;
    }

    // Get effective workspace: project overrides subos
    [[nodiscard]] static xvm::Workspace effective_workspace() {
        auto& self = instance_();
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

    // INVARIANT: a reader and its writer must resolve to the SAME map.
    //
    // They did not. `workspace_mut()` honored forceGlobalScope_ and
    // `workspace()` did not, so under `-g` a package was written into the
    // home's workspace and looked up in the project's -- present and absent
    // at the same time, depending on which half you asked. `install -g`
    // registered it and `remove -g` reported "not installed in current
    // subos", with the shim plainly on disk.
    //
    // The two bodies are therefore identical on purpose. If one grows a
    // condition, the other has to grow it too.
    [[nodiscard]] static const xvm::Workspace& workspace() {
        auto& self = instance_();
        if (self.forceGlobalScope_ || !self.hasProjectConfig_) return self.globalWorkspace_;
        if (self.projectSubosMode_ == ProjectSubosMode::Named ||
            self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosWorkspace_;
        return self.projectWorkspace_;
    }
    [[nodiscard]] static xvm::Workspace& workspace_mut() {
        auto& self = instance_();
        if (self.forceGlobalScope_ || !self.hasProjectConfig_) return self.globalWorkspace_;
        if (self.projectSubosMode_ == ProjectSubosMode::Named ||
            self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosWorkspace_;
        return self.projectWorkspace_;
    }

    // Read access to per-subos installed[] sets, paired with the
    // workspace returned by workspace()/workspace_mut(). Only meaningful
    // for subos-side writes (project manifest path returns an empty
    // installed map since the project file format has no installed).
    // Same invariant as workspace()/workspace_mut() above, same reason.
    [[nodiscard]] static const xvm::WorkspaceInstalled& workspace_installed() {
        auto& self = instance_();
        if (self.forceGlobalScope_ || !self.hasProjectConfig_) return self.globalInstalled_;
        if (self.projectSubosMode_ == ProjectSubosMode::Named ||
            self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosInstalled_;
        return self.globalInstalled_;
    }
    [[nodiscard]] static xvm::WorkspaceInstalled& workspace_installed_mut() {
        auto& self = instance_();
        if (self.forceGlobalScope_ || !self.hasProjectConfig_) return self.globalInstalled_;
        if (self.projectSubosMode_ == ProjectSubosMode::Named ||
            self.projectSubosMode_ == ProjectSubosMode::Anonymous) return self.projectSubosInstalled_;
        return self.globalInstalled_;
    }
    [[nodiscard]] static bool has_project_config() { return instance_().hasProjectConfig_; }

    // Force all version/workspace writes to go to global scope.
    // Used by `install -g` to ensure tools are available outside project context.
    // Recomputes the cached paths: `-g` changes which subos this process acts
    // on, and paths_ is derived from that. Without the recompute the flag was
    // honored by one reader and ignored by another for the rest of the run.
    static void set_force_global_scope(bool force) {
        auto& self = instance_();
        if (self.forceGlobalScope_ == force) return;
        self.forceGlobalScope_ = force;
        self.update_effective_paths_();
    }

    static std::filesystem::path subos_dir(const std::string& name) {
        return instance_().paths_.homeDir / "subos" / name;
    }

    [[nodiscard]] static std::filesystem::path global_subos_dir() {
        return instance_().global_subos_dir_();
    }

    [[nodiscard]] static std::filesystem::path global_subos_bin_dir() {
        return global_subos_dir() / "bin";
    }

    // Kept as a name because ~40 call sites read it, but it is no longer a
    // second implementation -- it is the single resolver, live. See
    // resolve_subos_scope_ for why there is exactly one.
    [[nodiscard]] static std::filesystem::path
    xvm_artifact_subos_dir() {
        return instance_().resolve_subos_scope_().root;
    }

    // The same answer as a value: name and root together, so a caller that
    // needs both cannot pick them from two different resolutions.
    [[nodiscard]] static SubosScope subos_scope() {
        return instance_().resolve_subos_scope_();
    }

    static std::vector<std::string> list_subos_names() {
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

    // Save .xlings.json versions section (project-local if project config exists)
    static void save_versions() {
        namespace fs = std::filesystem;
        auto& self = instance_();
        bool useGlobal = self.forceGlobalScope_ || !self.hasProjectConfig_ || self.projectDir_.empty();
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
        platform::write_string_to_file(configPath.string(), json.dump(2));
    }

    // Which xlings set this home up.
    //
    // Always the HOME config, never a project one: this describes the
    // installed client, not a workspace. Historically only `self install`
    // wrote it (xself/install.cppm), so `self update` -- which installs
    // xlings@latest as an ordinary package -- left it reading the previous
    // version forever. The field is what tells a user their packages predate
    // their client, so a stale one is worse than none.
    [[nodiscard]] static std::string recorded_client_version() {
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

    // Read-modify-write of the single field, deliberately: the file also
    // holds the xvm versions DB and the user's config, and this is called
    // from a command that has not necessarily loaded either.
    static void record_client_version(const std::string& version) {
        namespace fs = std::filesystem;
        auto configPath = instance_().paths_.homeDir / ".xlings.json";
        nlohmann::json json = nlohmann::json::object();
        if (fs::exists(configPath)) {
            try {
                auto content = platform::read_file_to_string(configPath.string());
                json = nlohmann::json::parse(content, nullptr, false);
                if (json.is_discarded() || !json.is_object()) {
                    // Refuse to replace a file we could not parse. Overwriting
                    // it here would trade a stale version field for a lost
                    // versions DB.
                    return;
                }
            } catch (...) { return; }
        }
        json["version"] = version;
        platform::write_string_to_file(configPath.string(), json.dump(2));
    }

    // Save current subos workspace (project-local if project config exists)
    static void save_workspace() {
        namespace fs = std::filesystem;
        auto& self = instance_();
        bool useProject = self.hasProjectConfig_ && !self.forceGlobalScope_;
        fs::path subosConfigPath;
        if (useProject &&
            self.projectSubosMode_ == ProjectSubosMode::Named) {
            auto projSubosDir = self.project_subos_dir_();
            fs::create_directories(projSubosDir);
            fs::create_directories(projSubosDir / "bin");
            fs::create_directories(projSubosDir / "lib");
            fs::create_directories(projSubosDir / "usr");
            fs::create_directories(projSubosDir / "generations");
            subosConfigPath = projSubosDir / ".xlings.json";
        } else if (useProject &&
                   self.projectSubosMode_ == ProjectSubosMode::Anonymous) {
            auto projSubosDir = self.project_subos_dir_();
            auto projectHomeDir = self.project_home_dir_();
            if (!projectHomeDir.empty()) fs::create_directories(projectHomeDir);
            fs::create_directories(projSubosDir);
            fs::create_directories(projSubosDir / "bin");
            fs::create_directories(projSubosDir / "lib");
            fs::create_directories(projSubosDir / "usr");
            fs::create_directories(projSubosDir / "generations");
            subosConfigPath = self.project_state_path_();
        } else if (useProject && !self.projectDir_.empty()) {
            auto projectHomeDir = self.project_home_dir_();
            if (!projectHomeDir.empty()) fs::create_directories(projectHomeDir);
            subosConfigPath = self.project_state_path_();
        } else {
            // useProject=false here — either no project config, or
            // forceGlobalScope_ override is on (e.g. `xlings install -g`).
            // Both paths mean "act on global scope, ignore project mode".
            //
            // We must NOT use paths_.activeSubos directly: when the user
            // is inside an anonymous-project directory, paths_.activeSubos
            // was set to "_" by update_effective_paths_ (the anonymous
            // marker) and stays "_" even after forceGlobalScope_ flips
            // useProject to false. Writing to `~/.xlings/subos/_/.xlings.json`
            // (a directory that doesn't exist) then fails the workspace
            // save — surfaced as `Failed to write file` during
            // `xlings self install`'s patchelf-runtime-dep step (which
            // spawns `xlings install -g`) when the user happens to run
            // self install from inside an xlings repo / project tree.
            //
            // Resolve the same global-scope subos root used by XVM
            // filesystem effects.
            subosConfigPath =
                self.global_subos_dir_() / ".xlings.json";
        }

        nlohmann::json json;
        if (fs::exists(subosConfigPath)) {
            try {
                auto content = platform::read_file_to_string(subosConfigPath.string());
                json = nlohmann::json::parse(content, nullptr, false);
                if (json.is_discarded()) json = nlohmann::json::object();
            } catch (...) { json = nlohmann::json::object(); }
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
        }
        json["workspace"] = xvm::subos_workspace_to_json(sws);
        platform::write_string_to_file(subosConfigPath.string(), json.dump(2));
    }

    static void print_paths() {
        auto& p = paths();
        std::println("XLINGS_HOME:     {}", p.homeDir.string());
        std::println("XLINGS_DATA:     {}", display_path(p.dataDir));
        if (has_project_config() && !project_index_repos().empty()) {
            std::println("XLINGS_DATA_PROJECT: {}", display_path(project_data_dir()));
        }
        std::println("XLINGS_SUBOS:    {}", display_path(p.subosDir));
        std::println("  activeSubos:   {}", p.activeSubos);
        std::println("  selfContained: {}", p.selfContained);
        std::println("  bin:           {}", display_path(p.binDir));
    }
};

} // namespace xlings
