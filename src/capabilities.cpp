module xlings.capabilities;

import std;
import xlings.platform;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;
import xlings.libs.json;
import xlings.core.xim.commands;
import xlings.core.xim.index_cmd;
import xlings.core.xvm.commands;
import xlings.core.config;
import xlings.core.home_config;
import xlings.core.subos;
import xlings.core.xself;
import xlings.platform;
import xlings.runtime.cancellation;
import xlings.core.utf8;

namespace xlings::capabilities {

Result exit_result(int code) {
    return nlohmann::json({{"exitCode", code}}).dump();
}

capability::Registry build_registry() {
    capability::Registry reg;
    // xlings core capabilities
    reg.register_capability(std::make_unique<SearchPackages>());
    reg.register_capability(std::make_unique<InstallPackages>());
    reg.register_capability(std::make_unique<PlanInstall>());
    reg.register_capability(std::make_unique<RemovePackage>());
    reg.register_capability(std::make_unique<UpdatePackages>());
    reg.register_capability(std::make_unique<ListPackages>());
    reg.register_capability(std::make_unique<PackageInfo>());
    reg.register_capability(std::make_unique<ListVersions>());
    reg.register_capability(std::make_unique<UseVersion>());
    reg.register_capability(std::make_unique<SystemStatus>());
    reg.register_capability(std::make_unique<ListIndexVersions>());
    // sub-OS + env (added 2026-04-26 per interface-api-v1-eval)
    reg.register_capability(std::make_unique<ListSubos>());
    reg.register_capability(std::make_unique<ListSubosShims>());
    reg.register_capability(std::make_unique<CreateSubos>());
    reg.register_capability(std::make_unique<SwitchSubos>());
    reg.register_capability(std::make_unique<RemoveSubos>());
    reg.register_capability(std::make_unique<Env>());
    // Index repo management (added 2026-04-26 per interface-api-v1-eval P1)
    reg.register_capability(std::make_unique<ListRepos>());
    reg.register_capability(std::make_unique<AddRepo>());
    reg.register_capability(std::make_unique<RemoveRepo>());
    return reg;
}

}


// ── out-of-line class members ─────────────────────────────────

namespace xlings::capabilities {

auto SearchPackages::spec() const -> CapabilitySpec{
        return {
            .name = "search_packages",
            .description = "Search available packages by keyword (fuzzy match). Use plain name without version or namespace (e.g. gc matches gcc, musl-gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"keyword":{"type":"string","description":"Keyword for fuzzy matching package names, e.g. gc, nod, rust"}},"required":["keyword"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto SearchPackages::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto keyword = json.value("keyword", "");
        return exit_result(xim::cmd_search(keyword, stream));
    }

auto InstallPackages::spec() const -> CapabilitySpec{
        return {
            .name = "install_packages",
            .description = "Install one or more packages",
            .inputSchema = R"({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"},"description":"Format: name, name@version, or namespace:name@version"},"yes":{"type":"boolean","description":"Auto-confirm without prompting"},"noDeps":{"type":"boolean","description":"Skip dependency installation"},"global":{"type":"boolean","description":"Install to global scope"},"useAfterInstall":{"type":"boolean","description":"Activate the installed version even if another version is currently active"}},"required":["targets"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto InstallPackages::execute(Params params, EventStream& stream) -> Result{
        return execute(std::move(params), stream, nullptr);
    }

auto InstallPackages::execute(Params params, EventStream& stream, CancellationToken* cancel) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        std::vector<std::string> targets;
        if (json.contains("targets") && json["targets"].is_array()) {
            for (auto& t : json["targets"]) targets.push_back(t.get<std::string>());
        }
        bool yes = json.value("yes", false);
        bool noDeps = json.value("noDeps", false);
        bool global = json.value("global", false);
        bool useAfter = json.value("useAfterInstall", false);
        return exit_result(xim::cmd_install(targets, yes, noDeps, stream, global,
                                             cancel, /*dryRun=*/false, useAfter));
    }

auto PlanInstall::spec() const -> CapabilitySpec{
        return {
            .name = "plan_install",
            .description = "Resolve targets and report what install_packages WOULD do (no download, no install)",
            .inputSchema = R"({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"}},"noDeps":{"type":"boolean"},"global":{"type":"boolean"}},"required":["targets"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto PlanInstall::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        std::vector<std::string> targets;
        if (json.contains("targets") && json["targets"].is_array()) {
            for (auto& t : json["targets"]) targets.push_back(t.get<std::string>());
        }
        bool noDeps = json.value("noDeps", false);
        bool global = json.value("global", false);
        return exit_result(xim::cmd_install(targets, /*yes=*/true, noDeps, stream,
                                             global, /*cancel=*/nullptr, /*dryRun=*/true));
    }

auto RemovePackage::spec() const -> CapabilitySpec{
        return {
            .name = "remove_package",
            .description = "Remove one package or one specific version. Only removes one target per call — to remove multiple versions, call multiple times",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string","description":"Format: name, name@version, or namespace:name@version"},"yes":{"type":"boolean","description":"Auto-confirm without prompting"}},"required":["target"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto RemovePackage::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        bool yes = json.value("yes", false);
        bool force = json.value("force", false);
        return exit_result(
            xim::cmd_remove(json.value("target", ""), yes, stream, force));
    }

auto UpdatePackages::spec() const -> CapabilitySpec{
        return {
            .name = "update_packages",
            .description = "Update package index or upgrade a specific package to the latest declared version",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string","description":"Package name to upgrade, or omit to refresh the index only"},"yes":{"type":"boolean","description":"Auto-confirm the upgrade prompt"}}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto UpdatePackages::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        bool yes = json.value("yes", false);
        return exit_result(xim::cmd_update(json.value("target", ""), yes, stream));
    }

auto ListPackages::spec() const -> CapabilitySpec{
        return {
            .name = "list_packages",
            .description = "List installed packages. Filter is a plain package name keyword (fuzzy match, not regex)",
            .inputSchema = R"JSON({"type":"object","properties":{"filter":{"type":"string","description":"Plain package name to filter, e.g. gcc, node"}}})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto ListPackages::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_list(json.value("filter", ""), stream));
    }

auto PackageInfo::spec() const -> CapabilitySpec{
        return {
            .name = "package_info",
            .description = "Get detailed package info: versions, install status, metadata. Use namespace:name format (e.g. xim:gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Package in namespace:name format, e.g. xim:gcc"}},"required":["target"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto PackageInfo::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_info(json.value("target", ""), stream));
    }

auto ListVersions::spec() const -> CapabilitySpec{
        return {
            .name = "list_installed_versions",
            .description = "List installed versions for a package and show which is active. Use plain name (e.g. gcc not xim:gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Plain package name, e.g. gcc, node"}},"required":["target"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto ListVersions::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xvm::cmd_list_versions(json.value("target", ""), stream));
    }

auto UseVersion::spec() const -> CapabilitySpec{
        return {
            .name = "use_version",
            .description = "Switch to a specific version of a package. Use plain name (e.g. gcc not xim:gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Plain package name, e.g. gcc, node"},"version":{"type":"string","description":"Version to switch to, e.g. 15, 22"}},"required":["target","version"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto UseVersion::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xvm::cmd_use(json.value("target", ""), json.value("version", ""), stream));
    }

auto SystemStatus::spec() const -> CapabilitySpec{
        return {
            .name = "system_status",
            .description = "Show xlings system configuration and status",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto SystemStatus::execute(Params params, EventStream& stream) -> Result{
        auto& p = Config::paths();
        nlohmann::json fields = nlohmann::json::array();
        auto addField = [&](const std::string& label, const std::string& value, bool hl = false) {
            fields.push_back({{"label", label}, {"value", value}, {"highlight", hl}});
        };
        addField("XLINGS_HOME", p.homeDir.string());
        addField("XLINGS_DATA", p.dataDir.string());
        addField("XLINGS_SUBOS", p.subosDir.string());
        addField("active subos", p.activeSubos, true);
        addField("bin", p.binDir.string());

        auto mirror = Config::mirror();
        if (!mirror.empty()) addField("mirror", mirror);
        auto lang = Config::lang();
        if (!lang.empty()) addField("lang", lang);

        nlohmann::json payload;
        payload["title"] = "xlings status";
        payload["fields"] = std::move(fields);
        stream.emit(DataEvent{"system_info", payload.dump()});
        return exit_result(0);
    }

auto ListIndexVersions::spec() const -> CapabilitySpec{
        return {
            .name = "list_index_versions",
            .description = "List published index snapshots and which one is in use",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto ListIndexVersions::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        const auto filter = json.is_object() ? json.value("name", std::string{})
                                             : std::string{};
        auto views = xim::collect_index_sources();
        if (!filter.empty()) {
            std::erase_if(views, [&](const auto& v) { return v.name != filter; });
            if (views.empty()) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::NotFound,
                    .message = std::format("no index source named '{}'", filter),
                    .recoverable = false,
                });
                return exit_result(1);
            }
        }
        stream.emit(DataEvent{"index_versions",
                              xim::index_sources_json(views).dump()});
        return exit_result(0);
    }

auto ListSubos::spec() const -> CapabilitySpec{
        return {
            .name = "list_subos",
            .description = "List all sub-OSs and which one is active",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto ListSubos::execute(Params, EventStream& stream) -> Result{
        auto all = subos::candidate_view().candidates;
        nlohmann::json entries = nlohmann::json::array();
        for (auto& s : all) {
            entries.push_back({
                {"name",     s.name},
                {"dir",      s.dir.string()},
                {"pkgCount", s.toolCount},
                {"active",   s.isActive},
            });
        }
        nlohmann::json payload;
        payload["entries"] = std::move(entries);
        stream.emit(DataEvent{"subos_list", payload.dump()});
        return exit_result(0);
    }

auto ListSubosShims::spec() const -> CapabilitySpec{
        return {
            .name = "list_subos_shims",
            .description = "List installed shim binaries in the active sub-OS bin directory",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto ListSubosShims::execute(Params, EventStream& stream) -> Result{
        auto& p = Config::paths();
        std::vector<std::string> shims;
        if (std::filesystem::exists(p.binDir)) {
            for (auto& e : platform::dir_entries(p.binDir)) {
                auto stem = e.path().stem().string();
                if (xself::is_builtin_shim(stem) || stem == "xvm-alias") continue;
                shims.push_back(std::move(stem));
            }
        }
        std::ranges::sort(shims);
        nlohmann::json payload;
        payload["shims"]  = shims;
        payload["binDir"] = p.binDir.string();
        stream.emit(DataEvent{"subos_shims", payload.dump()});
        return exit_result(0);
    }

auto CreateSubos::spec() const -> CapabilitySpec{
        return {
            .name = "create_subos",
            .description = "Create a new sub-OS. The name must be alphanumeric (plus _ or -); 'current' is reserved",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"dir":{"type":"string","description":"Optional custom directory; defaults to $XLINGS_HOME/subos/<name>"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto CreateSubos::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto name = json.value("name", "");
        auto dir  = json.value("dir", "");
        return exit_result(subos::create(
            name,
            dir.empty() ? std::filesystem::path{} : std::filesystem::path{dir},
            stream));
    }

auto SwitchSubos::spec() const -> CapabilitySpec{
        return {
            .name = "switch_subos",
            .description = "Switch the active sub-OS to <name>",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto SwitchSubos::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(subos::use(json.value("name", ""), stream));
    }

auto RemoveSubos::spec() const -> CapabilitySpec{
        return {
            .name = "remove_subos",
            .description = "Remove a sub-OS. Refuses to remove 'default' or the currently active sub-OS",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto RemoveSubos::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(subos::remove(json.value("name", ""), stream));
    }

auto ListRepos::spec() const -> CapabilitySpec{
        return {
            .name = "list_repos",
            .description = "List configured xim package index repositories (global + project)",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto ListRepos::execute(Params, EventStream& stream) -> Result{
        nlohmann::json repos = nlohmann::json::array();
        for (auto& r : Config::global_index_repos()) {
            repos.push_back({{"name", r.name}, {"url", r.url}, {"scope", "global"}});
        }
        if (Config::has_project_config()) {
            for (auto& r : Config::project_index_repos()) {
                repos.push_back({{"name", r.name}, {"url", r.url}, {"scope", "project"}});
            }
        }
        nlohmann::json payload;
        payload["repos"] = std::move(repos);
        payload["override"] = (Config::has_project_config() && !Config::project_index_repos().empty())
                              ? "project" : "global";
        stream.emit(DataEvent{"repo_list", payload.dump()});
        return exit_result(0);
    }

auto AddRepo::spec() const -> CapabilitySpec{
        return {
            .name = "add_repo",
            .description = "Add or update a global xim index repository. If the name already exists, the URL is updated in place",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"url":{"type":"string","description":"git URL of the index repository"}},"required":["name","url"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto AddRepo::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto name = json.value("name", "");
        auto url  = json.value("url",  "");
        if (name.empty() || url.empty()) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = "add_repo requires non-empty name and url",
                .recoverable = false,
            });
            return exit_result(1);
        }

        auto committed = update_home_config(
            Config::paths().homeDir, [&](nlohmann::json& cfg) {
                if (!cfg.contains("index_repos")
                    || !cfg["index_repos"].is_array()) {
                    cfg["index_repos"] = nlohmann::json::array();
                }
                for (auto& entry : cfg["index_repos"]) {
                    if (entry.is_object() && entry.contains("name")
                        && entry["name"].get<std::string>() == name) {
                        entry["url"] = url;
                        return true;
                    }
                }
                nlohmann::json entry;
                entry["name"] = name;
                entry["url"]  = url;
                cfg["index_repos"].push_back(std::move(entry));
                return true;
            });
        if (!committed) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Permission,
                .message = "write .xlings.json failed: " + committed.error(),
                .recoverable = false,
            });
            return exit_result(1);
        }
        return exit_result(0);
    }

auto RemoveRepo::spec() const -> CapabilitySpec{
        return {
            .name = "remove_repo",
            .description = "Remove a global xim index repository by name",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }

auto RemoveRepo::execute(Params params, EventStream& stream) -> Result{
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto name = json.value("name", "");
        if (name.empty()) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = "remove_repo requires non-empty name",
                .recoverable = false,
            });
            return exit_result(1);
        }

        // "not found" is decided inside the lock too. Deciding it from an
        // earlier read could report success for a repo another process had
        // already deleted, or -- worse -- write back an index_repos array
        // that predates a repo it had just added.
        auto committed = update_home_config(
            Config::paths().homeDir, [&](nlohmann::json& cfg) {
                if (!cfg.contains("index_repos")
                    || !cfg["index_repos"].is_array()) {
                    return false;
                }
                auto& arr = cfg["index_repos"];
                bool removed = false;
                for (auto it = arr.begin(); it != arr.end(); ) {
                    if (it->is_object() && it->contains("name")
                        && (*it)["name"].get<std::string>() == name) {
                        it = arr.erase(it);
                        removed = true;
                    } else { ++it; }
                }
                return removed;
            });
        if (!committed) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Permission,
                .message = "write .xlings.json failed: " + committed.error(),
                .recoverable = false,
            });
            return exit_result(1);
        }
        if (!*committed) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "no repo named '" + name + "'",
                .recoverable = true,
            });
            return exit_result(1);
        }
        return exit_result(0);
    }

auto Env::spec() const -> CapabilitySpec{
        return {
            .name = "env",
            .description = "Return effective xlings environment (home, active sub-OS, paths, mirror, lang)",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }

auto Env::execute(Params, EventStream& stream) -> Result{
        auto& p = Config::paths();
        nlohmann::json payload;
        payload["xlingsHome"]  = p.homeDir.string();
        payload["dataDir"]     = p.dataDir.string();
        payload["subosDir"]    = p.subosDir.string();
        payload["binDir"]      = p.binDir.string();
        payload["libDir"]      = p.libDir.string();
        payload["activeSubos"] = p.activeSubos;
        payload["mirror"]      = Config::mirror();
        payload["lang"]        = Config::lang();
        stream.emit(DataEvent{"env", payload.dump()});
        return exit_result(0);
    }

} // namespace xlings::capabilities
