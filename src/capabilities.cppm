export module xlings.capabilities;

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

using capability::Capability;
using capability::CapabilitySpec;
using capability::Params;
using capability::Result;

Result exit_result(int code);

class SearchPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "search_packages",
            .description = "Search available packages by keyword (fuzzy match). Use plain name without version or namespace (e.g. gc matches gcc, musl-gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"keyword":{"type":"string","description":"Keyword for fuzzy matching package names, e.g. gc, nod, rust"}},"required":["keyword"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto keyword = json.value("keyword", "");
        return exit_result(xim::cmd_search(keyword, stream));
    }
};

class InstallPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "install_packages",
            .description = "Install one or more packages",
            .inputSchema = R"({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"},"description":"Format: name, name@version, or namespace:name@version"},"yes":{"type":"boolean","description":"Auto-confirm without prompting"},"noDeps":{"type":"boolean","description":"Skip dependency installation"},"global":{"type":"boolean","description":"Install to global scope"},"useAfterInstall":{"type":"boolean","description":"Activate the installed version even if another version is currently active"}},"required":["targets"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        return execute(std::move(params), stream, nullptr);
    }
    auto execute(Params params, EventStream& stream, CancellationToken* cancel) -> Result override {
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
};

// Pre-flight planning entry point. Resolves targets and emits the
// install_plan dataKind, but does not download or install anything.
// Clients use this for dry-run / "would do X" UX without committing.
class PlanInstall : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "plan_install",
            .description = "Resolve targets and report what install_packages WOULD do (no download, no install)",
            .inputSchema = R"({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"}},"noDeps":{"type":"boolean"},"global":{"type":"boolean"}},"required":["targets"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
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
};

class RemovePackage : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "remove_package",
            .description = "Remove one package or one specific version. Only removes one target per call — to remove multiple versions, call multiple times",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string","description":"Format: name, name@version, or namespace:name@version"},"yes":{"type":"boolean","description":"Auto-confirm without prompting"}},"required":["target"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        bool yes = json.value("yes", false);
        bool force = json.value("force", false);
        return exit_result(
            xim::cmd_remove(json.value("target", ""), yes, stream, force));
    }
};

class UpdatePackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "update_packages",
            .description = "Update package index or upgrade a specific package to the latest declared version",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string","description":"Package name to upgrade, or omit to refresh the index only"},"yes":{"type":"boolean","description":"Auto-confirm the upgrade prompt"}}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        bool yes = json.value("yes", false);
        return exit_result(xim::cmd_update(json.value("target", ""), yes, stream));
    }
};

class ListPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "list_packages",
            .description = "List installed packages. Filter is a plain package name keyword (fuzzy match, not regex)",
            .inputSchema = R"JSON({"type":"object","properties":{"filter":{"type":"string","description":"Plain package name to filter, e.g. gcc, node"}}})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_list(json.value("filter", ""), stream));
    }
};

class PackageInfo : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "package_info",
            .description = "Get detailed package info: versions, install status, metadata. Use namespace:name format (e.g. xim:gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Package in namespace:name format, e.g. xim:gcc"}},"required":["target"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_info(json.value("target", ""), stream));
    }
};

class ListVersions : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "list_installed_versions",
            .description = "List installed versions for a package and show which is active. Use plain name (e.g. gcc not xim:gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Plain package name, e.g. gcc, node"}},"required":["target"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xvm::cmd_list_versions(json.value("target", ""), stream));
    }
};

class UseVersion : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "use_version",
            .description = "Switch to a specific version of a package. Use plain name (e.g. gcc not xim:gcc)",
            .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Plain package name, e.g. gcc, node"},"version":{"type":"string","description":"Version to switch to, e.g. 15, 22"}},"required":["target","version"]})JSON",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xvm::cmd_use(json.value("target", ""), json.value("version", ""), stream));
    }
};

class SystemStatus : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "system_status",
            .description = "Show xlings system configuration and status",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
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
};

// #476: the programmatic entry point for index snapshot selection.
//
// The consumer here is a program, not a person -- a build tool deciding which
// index snapshot its own version can use. It reads the list, applies ITS OWN
// compatibility rule to the `requires` blob (xlings interprets only the
// "xlings" key and carries every other one verbatim), and pins with
// `xlings index use`.
class ListIndexVersions : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "list_index_versions",
            .description = "List published index snapshots and which one is in use",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
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
};

class ListSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "list_subos",
            .description = "List all sub-OSs and which one is active",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params, EventStream& stream) -> Result override {
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
};

class ListSubosShims : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "list_subos_shims",
            .description = "List installed shim binaries in the active sub-OS bin directory",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params, EventStream& stream) -> Result override {
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
};

class CreateSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "create_subos",
            .description = "Create a new sub-OS. The name must be alphanumeric (plus _ or -); 'current' is reserved",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"dir":{"type":"string","description":"Optional custom directory; defaults to $XLINGS_HOME/subos/<name>"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto name = json.value("name", "");
        auto dir  = json.value("dir", "");
        return exit_result(subos::create(
            name,
            dir.empty() ? std::filesystem::path{} : std::filesystem::path{dir},
            stream));
    }
};

class SwitchSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "switch_subos",
            .description = "Switch the active sub-OS to <name>",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(subos::use(json.value("name", ""), stream));
    }
};

class RemoveSubos : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "remove_subos",
            .description = "Remove a sub-OS. Refuses to remove 'default' or the currently active sub-OS",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(subos::remove(json.value("name", ""), stream));
    }
};

// ─── Index repos ────────────────────────────────────────────

class ListRepos : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "list_repos",
            .description = "List configured xim package index repositories (global + project)",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params, EventStream& stream) -> Result override {
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
};

class AddRepo : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "add_repo",
            .description = "Add or update a global xim index repository. If the name already exists, the URL is updated in place",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"url":{"type":"string","description":"git URL of the index repository"}},"required":["name","url"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
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
};

class RemoveRepo : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "remove_repo",
            .description = "Remove a global xim index repository by name",
            .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
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
};

class Env : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "env",
            .description = "Return effective xlings environment (home, active sub-OS, paths, mirror, lang)",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params, EventStream& stream) -> Result override {
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
};

export capability::Registry build_registry();

} // namespace xlings::capabilities
