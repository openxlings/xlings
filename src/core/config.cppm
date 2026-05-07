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
    static constexpr std::string_view VERSION = "0.4.19";
    static constexpr std::string_view REPO = "https://github.com/openxlings/xlings";
};

export struct IndexRepo {
    std::string name;
    std::string url;
};

using MirrorServerMap = std::unordered_map<std::string, std::vector<std::string>>;

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

private:
    PathInfo paths_;
    std::string mirror_;
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

    static void load_index_repos_from_json_(const nlohmann::json& json,
                                            std::vector<IndexRepo>& out) {
        out.clear();
        if (!json.contains("index_repos") || !json["index_repos"].is_array()) return;
        for (auto it = json["index_repos"].begin(); it != json["index_repos"].end(); ++it) {
            if (!it->is_object() || !it->contains("name") || !it->contains("url")) continue;
            IndexRepo repo;
            repo.name = (*it)["name"].get<std::string>();
            repo.url  = (*it)["url"].get<std::string>();
            if (!repo.name.empty() && !repo.url.empty()) out.push_back(std::move(repo));
        }
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

    void update_effective_paths_() {
        // Resolution priority for the effective subos:
        //   1. Project-mode (Named or Anonymous) — strongest, "this directory
        //      asks for subos X" overrides everything else.
        //   2. $XLINGS_ACTIVE_SUBOS env var — shell-level override; the shell
        //      profile honors the same priority so PATH and `xpkg install`
        //      target stay in sync.
        //   3. globalActiveSubos_ from ~/.xlings.json — the persistent default
        //      that new shells inherit when no env override is set.
        paths_.activeSubos = globalActiveSubos_;
        paths_.subosDir = paths_.homeDir / "subos" / globalActiveSubos_;

        if (auto env = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
            !env.empty()) {
            paths_.activeSubos = env;
            paths_.subosDir = paths_.homeDir / "subos" / env;
        }

        if (projectSubosMode_ == ProjectSubosMode::Named && !projectSubosName_.empty()) {
            paths_.activeSubos = projectSubosName_;
            paths_.subosDir = project_subos_dir_();
        } else if (projectSubosMode_ == ProjectSubosMode::Anonymous) {
            paths_.activeSubos = "_";
            paths_.subosDir = project_subos_dir_();
        }
        paths_.binDir = paths_.subosDir / "bin";
        paths_.libDir = paths_.subosDir / "lib";
    }

    Config() {
        namespace fs = std::filesystem;

        auto envHome = utils::get_env_or_default("XLINGS_HOME");
        if (!envHome.empty()) {
            paths_.homeDir = envHome;
        } else {
            auto exePath   = platform::get_executable_path();
            auto exeParent = exePath.parent_path();
            auto candidate = exeParent.parent_path();
            auto hasRootConfig = !exePath.empty() && fs::exists(candidate / ".xlings.json");
#ifdef _WIN32
            auto hasRootBin = !exePath.empty() && fs::exists(candidate / "bin" / "xlings.exe");
#else
            auto hasRootBin = !exePath.empty() && fs::exists(candidate / "bin" / "xlings");
#endif
            if (hasRootConfig && hasRootBin) {
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
                    if (json.contains("versions") && json["versions"].is_object())
                        globalVersions_ = xvm::versions_from_json(json["versions"]);
                    load_index_repos_from_json_(json, globalIndexRepos_);
                    load_resource_servers_from_json_(json, globalResourceServers_);
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
        auto subosConfigPath = paths_.homeDir / "subos" / paths_.activeSubos / ".xlings.json";
        {
            auto sws = load_workspace_from_file_(subosConfigPath);
            globalWorkspace_ = std::move(sws.active);
            globalInstalled_ = std::move(sws.installed);
        }

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
                load_index_repos_from_json_(json, projectIndexRepos_);
                load_resource_servers_from_json_(json, projectResourceServers_);
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
        fs::path cur = startDir;
        while (!cur.empty()) {
            auto cfg = cur / ".xlings.json";
            if (fs::exists(cfg, ec) && fs::is_regular_file(cfg, ec)) {
                auto curNorm = fs::weakly_canonical(cur, ec);
                if (curNorm != homeNorm) {
                    load_project_config_from_dir_(cur);
                    if (hasProjectConfig_) return;     // real project loaded
                    // else: projectScope:false skip — keep walking, then env fallback
                }
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
                    auto dirNorm = fs::weakly_canonical(dir, ec);
                    if (dirNorm != homeNorm) {
                        load_project_config_from_dir_(dir);
                    }
                }
            }
        }
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

    [[nodiscard]] static const PathInfo& paths() { return instance_().paths_; }
    [[nodiscard]] static const std::string& mirror() { return instance_().mirror_; }
    [[nodiscard]] static const std::string& lang() { return instance_().lang_; }
    [[nodiscard]] static std::vector<std::string> resource_servers(std::string_view mirror = {}) {
        return instance_().candidate_resource_servers_for_(mirror);
    }
    [[nodiscard]] static std::string resource_server(std::string_view mirror = {}) {
        return instance_().selected_resource_server_for_(mirror);
    }

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
        if (!self.hasProjectConfig_) return self.globalWorkspace_;
        return merged_workspace(self.globalWorkspace_,
                                self.projectWorkspace_,
                                self.projectSubosWorkspace_,
                                self.projectSubosMode_);
    }

    [[nodiscard]] static const xvm::Workspace& workspace() {
        auto& self = instance_();
        if (!self.hasProjectConfig_) return self.globalWorkspace_;
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
    [[nodiscard]] static const xvm::WorkspaceInstalled& workspace_installed() {
        auto& self = instance_();
        if (!self.hasProjectConfig_) return self.globalInstalled_;
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
    static void set_force_global_scope(bool force) { instance_().forceGlobalScope_ = force; }

    static std::filesystem::path subos_dir(const std::string& name) {
        return instance_().paths_.homeDir / "subos" / name;
    }

    [[nodiscard]] static std::filesystem::path global_subos_bin_dir() {
        auto& self = instance_();
        return self.paths_.homeDir / "subos" / self.globalActiveSubos_ / "bin";
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
            // Compose the global-scope path from XLINGS_ACTIVE_SUBOS env
            // (per-shell override) or globalActiveSubos_ (~/.xlings.json
            // snapshot) — both are valid global-scope subos names whose
            // directories exist on disk.
            std::string global_name;
            if (auto env = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
                !env.empty()) {
                global_name = env;
            } else {
                global_name = self.globalActiveSubos_;
            }
            subosConfigPath = self.paths_.homeDir / "subos" / global_name / ".xlings.json";
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
        std::println("XLINGS_DATA:     {}", p.dataDir.string());
        if (has_project_config() && !project_index_repos().empty()) {
            std::println("XLINGS_DATA_PROJECT: {}", project_data_dir().string());
        }
        std::println("XLINGS_SUBOS:    {}", p.subosDir.string());
        std::println("  activeSubos:   {}", p.activeSubos);
        std::println("  selfContained: {}", p.selfContained);
        std::println("  bin:           {}", p.binDir.string());
    }
};

} // namespace xlings
