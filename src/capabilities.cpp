module xlings.capabilities;

import std;
import xlings.platform;
import xlings.platform.stream;
import xlings.platform.loopback_http;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;
import xlings.libs.json;
import xlings.core.xim.commands;
import xlings.core.xim.index_cmd;
import xlings.core.xim.repo;
import xlings.core.xim.indexfetch;
import xlings.libs.tinyhttps;
import xlings.core.xvm.commands;
import xlings.core.xvm.inspect;
import xlings.core.profile;
import xlings.core.config;
import xlings.core.home_config;
import xlings.core.home.domain_producer;
import xlings.core.subos;
import xlings.core.subos.sandbox;
import xlings.core.xself;
import xlings.platform;
import xlings.runtime.cancellation;
import xlings.core.utf8;
import xlings.observe;
import xlings.subos.home_view;
import xlings.core.subos.ports;

namespace xlings::capabilities {

namespace {
tinyhttps::ProbeResult probe_loopback(std::string_view url, int socketTimeoutMs) {
    namespace http = platform::loopback_http;
    tinyhttps::ProbeResult result;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(std::clamp(socketTimeoutMs, 1, 3000));
    std::error_code ec;
    auto file = std::filesystem::temp_directory_path(ec);
    if (ec) { result.status = "local"; return result; }
    file /= std::format("xlings-network-{}-{}", start.time_since_epoch().count(), std::random_device{}());
    bool limited = false;
    const auto wire = http::download(url, file,
        std::chrono::seconds(std::max(1, (socketTimeoutMs + 999) / 1000)),
        [&](double total, double bytes) { limited = bytes >= 65536; },
        [&] { return limited || std::chrono::steady_clock::now() >= deadline; });
    std::filesystem::remove(file, ec);
    result.httpStatus = wire.httpStatus;
    result.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result.reachable = wire.success || (limited && wire.httpStatus == 200);
    result.status = result.reachable ? "reachable"
                  : std::chrono::steady_clock::now() >= deadline ? "timeout"
                  : wire.failure == http::Failure::Source ? "http"
                  : wire.failure == http::Failure::Local ? "local" : "connect";
    return result;
}
}

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
    reg.register_capability(std::make_unique<PayloadStatus>());
    reg.register_capability(std::make_unique<SyncIndex>());
    reg.register_capability(std::make_unique<NetworkEndpoints>());
    reg.register_capability(std::make_unique<ProbeUrl>());
    reg.register_capability(std::make_unique<UseVersion>());
    reg.register_capability(std::make_unique<SystemStatus>());
    reg.register_capability(std::make_unique<ListIndexVersions>());
    // sub-OS + env (added 2026-04-26 per interface-api-v1-eval)
    reg.register_capability(std::make_unique<ListSubos>());
    reg.register_capability(std::make_unique<ListSubosShims>());
    reg.register_capability(std::make_unique<CreateSubos>());
    reg.register_capability(std::make_unique<SubosCommand>());
    reg.register_capability(std::make_unique<SwitchSubos>());
    reg.register_capability(std::make_unique<RemoveSubos>());
    reg.register_capability(std::make_unique<SubosExec>());
    reg.register_capability(std::make_unique<SubosStart>());
    reg.register_capability(std::make_unique<SubosStop>());
    reg.register_capability(std::make_unique<SubosEvents>());
    reg.register_capability(std::make_unique<SubosEnv>());
    reg.register_capability(std::make_unique<Env>());
    // Index repo management (added 2026-04-26 per interface-api-v1-eval P1)
    reg.register_capability(std::make_unique<ListRepos>());
    reg.register_capability(std::make_unique<AddRepo>());
    reg.register_capability(std::make_unique<RemoveRepo>());
    return reg;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::capabilities {

auto SearchPackages::spec() const -> CapabilitySpec {
    return {
        .name = "search_packages",
        .description = "Search available packages by keyword (fuzzy match). Use plain name without version or namespace (e.g. gc matches gcc, musl-gcc)",
        .inputSchema = R"JSON({"type":"object","properties":{"keyword":{"type":"string","description":"Keyword for fuzzy matching package names, e.g. gc, nod, rust"},"category":{"type":"string","description":"Only packages whose recipe lists this category, e.g. luban-desktop (the keyword may then be empty)"}}})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto SearchPackages::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    auto keyword = json.value("keyword", "");
    return exit_result(xim::cmd_search(keyword, stream, json.value("category", "")));
}

namespace {

// `noDeps` was published in the install_packages / plan_install schemas as
// "Skip dependency installation" and was never read by anything: a caller that
// set it got every dependency installed while believing it had skipped them.
// It is not implemented now either -- a package whose declared dependencies
// are missing is one that does not run -- so asking for it is refused out
// loud, and the field is gone from the schemas. `false` is what always
// happened, so it stays accepted.
bool refuse_no_deps_(const nlohmann::json& json, EventStream& stream) {
    if (!json.is_object() || !json.contains("noDeps")) return false;
    const auto& v = json["noDeps"];
    if (v.is_boolean() && !v.get<bool>()) return false;
    stream.emit(ErrorEvent{
        .code = ErrorCode::InvalidInput,
        .message = "noDeps is not supported: a package's declared dependencies "
                   "are always installed with it, because it does not work "
                   "without them. Nothing was installed.",
        .recoverable = true,
        .hint = "drop \"noDeps\" from the request",
    });
    return true;
}

// `"yes": true` and nothing else. A string "true" or a 1 is not the user
// saying yes to deleting their data; it is a malformed request, read as no.
bool explicit_yes_(const nlohmann::json& json) {
    return json.is_object() && json.contains("yes") && json["yes"].is_boolean()
        && json["yes"].get<bool>();
}

}  // namespace

auto InstallPackages::spec() const -> CapabilitySpec {
    return {
        .name = "install_packages",
        .description = "Install one or more packages",
        .inputSchema = R"({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"},"description":"Format: name, name@version, or namespace:name@version"},"yes":{"type":"boolean","description":"Auto-confirm without prompting"},"global":{"type":"boolean","description":"Install to global scope"},"useAfterInstall":{"type":"boolean","description":"Activate the installed version even if another version is currently active"},"reconfig":{"type":"boolean","description":"Run the configuration step again, even for packages already configured in this scope"},"subos":{"type":"string","description":"Install into this subos; a Luban system installs where it lives"}},"required":["targets"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto InstallPackages::execute(Params params, EventStream& stream) -> Result {
    return execute(std::move(params), stream, nullptr);
}

auto InstallPackages::execute(Params params, EventStream& stream, CancellationToken* cancel) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    std::vector<std::string> targets;
    if (json.contains("targets") && json["targets"].is_array()) {
        for (auto& t : json["targets"]) targets.push_back(t.get<std::string>());
    }
    if (refuse_no_deps_(json, stream)) return exit_result(2);
    bool yes = json.value("yes", false);
    bool global = json.value("global", false);
    bool useAfter = json.value("useAfterInstall", false);
    bool reconfig = json.value("reconfig", false);
    // `"subos"` (1.17, design 2026-10-21 §7.2): into that subos -- a Luban
    // system in a prefix domain installs where it lives, its events relayed
    // as they come, cancelled with this call.
    if (const auto name = json.value("subos", std::string{}); !name.empty()) {
        if (!std::filesystem::is_directory(Config::subos_dir(name))) {
            stream.emit(ErrorEvent{.code = ErrorCode::NotFound, .message = std::format("no subos named '{}'", name),
                                   .recoverable = false, .hint = "xlings subos list"});
            return exit_result(1);
        }
        auto scope = home::domain_producer::read_scope(Config::paths().homeDir, name);
        if (!scope) {
            stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = scope.error(), .recoverable = false});
            return exit_result(1);
        }
        if (*scope) {
            auto rc = subos::relay_capability_((**scope).domain, "install_packages", json, std::nullopt, stream);
            if (!rc) {
                stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = rc.error(), .recoverable = false});
                return exit_result(1);
            }
            return exit_result(*rc);
        }
        platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
        (void)Config::set_active_subos_override(name);
    }
    return exit_result(xim::cmd_install(targets, yes, /*noDeps=*/false, stream, global,
                                         cancel, /*dryRun=*/false, useAfter,
                                         /*allInStore=*/nullptr, reconfig));
}

auto PlanInstall::spec() const -> CapabilitySpec {
    return {
        .name = "plan_install",
        .description = "Resolve targets and report what install_packages WOULD do (no download, no install)",
        .inputSchema = R"({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"}},"global":{"type":"boolean"}},"required":["targets"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto PlanInstall::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    std::vector<std::string> targets;
    if (json.contains("targets") && json["targets"].is_array()) {
        for (auto& t : json["targets"]) targets.push_back(t.get<std::string>());
    }
    if (refuse_no_deps_(json, stream)) return exit_result(2);
    bool global = json.value("global", false);
    return exit_result(xim::cmd_install(targets, /*yes=*/true, /*noDeps=*/false, stream,
                                         global, /*cancel=*/nullptr, /*dryRun=*/true));
}

auto RemovePackage::spec() const -> CapabilitySpec {
    return {
        .name = "remove_package",
        .description = "Remove one package or one specific version; with older_than, every version older than it that nothing uses",
        .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Format: name, name@version, or namespace:name@version"},"yes":{"type":"boolean","description":"Auto-confirm without prompting"},"older_than":{"type":"string","description":"Remove every installed version of target older than this one that nothing uses (target is a name)"},"dry_run":{"type":"boolean","description":"With older_than: report the remove_plan only"}},"required":["target"]})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto RemovePackage::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    bool yes = json.value("yes", false);
    bool force = json.value("force", false);
    if (auto bound = json.value("older_than", std::string{}); !bound.empty()) {
        return exit_result(xim::cmd_remove_older_than(
            json.value("target", ""), bound, yes, json.value("dry_run", false), stream));
    }
    return exit_result(
        xim::cmd_remove(json.value("target", ""), yes, stream, force));
}

auto UpdatePackages::spec() const -> CapabilitySpec {
    return {
        .name = "update_packages",
        .description = "Update package index or upgrade a specific package to the latest declared version",
        .inputSchema = R"({"type":"object","properties":{"target":{"type":"string","description":"Package name to upgrade, or omit to refresh the index only"},"yes":{"type":"boolean","description":"Auto-confirm the upgrade prompt"}}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto UpdatePackages::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    bool yes = json.value("yes", false);
    return exit_result(xim::cmd_update(json.value("target", ""), yes, stream));
}

auto ListPackages::spec() const -> CapabilitySpec {
    return {
        .name = "list_packages",
        .description = "List installed packages. Filter is a plain package name keyword (fuzzy match, not regex)",
        .inputSchema = R"JSON({"type":"object","properties":{"filter":{"type":"string","description":"Plain package name to filter, e.g. gcc, node"}}})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto ListPackages::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(xim::cmd_list(json.value("filter", ""), stream));
}

auto PackageInfo::spec() const -> CapabilitySpec {
    return {
        .name = "package_info",
        .description = "Get detailed package info: versions, install status, metadata. Use namespace:name format (e.g. xim:gcc)",
        .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Package in namespace:name format, e.g. xim:gcc"},"platform":{"type":"string","description":"Whose build is described: linux, macosx, windows (default: this machine's)"}},"required":["target"]})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto PackageInfo::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(xim::cmd_info(json.value("target", ""), stream, false, json.value("platform", "")));
}

auto PayloadStatus::spec() const -> CapabilitySpec {
    return {
        .name = "payload_status",
        .description = "What each target resolves to and what its payload is here -- payload_dir, revision, origin "
                       "(local | upstream | absent), complete -- without installing anything",
        .inputSchema = R"JSON({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"},"description":"Targets, e.g. xim:gcc@16.1.0"}},"required":["targets"]})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto PayloadStatus::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    std::vector<std::string> targets;
    if (json.is_object() && json.contains("targets") && json["targets"].is_array())
        for (const auto& t : json["targets"]) if (t.is_string()) targets.push_back(t.get<std::string>());
    return exit_result(xim::cmd_payload_status(targets, stream));
}

auto SyncIndex::spec() const -> CapabilitySpec {
    return {
        .name = "sync_index",
        .description = "Sync one configured index into a directory (its own when dest is omitted): a caller "
                       "checks a staged index before swapping it into place",
        .inputSchema = R"JSON({"type":"object","properties":{"name":{"type":"string","description":"The index's name"},"dest":{"type":"string","description":"Directory to sync into"}},"required":["name"]})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto SyncIndex::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(xim::cmd_sync_index(json.value("name", ""), json.value("dest", ""), stream));
}

auto NetworkEndpoints::spec() const -> CapabilitySpec {
    return {.name = "network_endpoints",
            .description = "Configured index discovery endpoints, without any network requests or installation",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object"})", .destructive = false};
}

auto NetworkEndpoints::execute(Params, EventStream& stream) -> Result {
    auto rows = nlohmann::json::array();
    std::set<std::string> seen;
    for (const auto& repo : Config::index_repos()) {
        if (Config::is_local_repo_source(repo, false)) continue;
        if (repo.source != "git" && xim::artifact_is_declared_for(repo, false)) {
            auto endpoints = xim::probe_endpoints(repo.artifactBases.empty() ? nullptr : &repo);
            for (const auto& endpoint : endpoints)
                if (seen.insert(endpoint.url).second)
                    rows.push_back({{"resource", repo.name}, {"region", endpoint.region}, {"url", endpoint.url}});
        } else if (repo.url.starts_with("https://")) {
            auto url = repo.url;
            while (url.ends_with('/')) url.pop_back();
            url += "/info/refs?service=git-upload-pack";
            if (seen.insert(url).second)
                rows.push_back({{"resource", repo.name}, {"region", "configured"}, {"url", url}});
        }
    }
    stream.emit(DataEvent{"network_endpoints", nlohmann::json{{"endpoints", std::move(rows)}}.dump()});
    return exit_result(0);
}

auto ProbeUrl::spec() const -> CapabilitySpec {
    return {.name = "probe_url",
            .description = "Small GET through the download transport; caller owns the total process deadline",
            .inputSchema = R"({"type":"object","properties":{"url":{"type":"string"},"socket_timeout_ms":{"type":"integer"}},"required":["url"]})",
            .outputSchema = R"({"type":"object"})", .destructive = false};
}

auto ProbeUrl::execute(Params params, EventStream& stream) -> Result {
    const auto args = nlohmann::json::parse(params, nullptr, false);
    if (!args.is_object() || !args.contains("url") || !args["url"].is_string()) return exit_result(2);
    int socketTimeout = 2000;
    if (args.contains("socket_timeout_ms")) {
        const auto& timeout = args["socket_timeout_ms"];
        if (!timeout.is_number_integer() || timeout < 1 || timeout > 3000) return exit_result(2);
        socketTimeout = timeout.get<int>();
    }
    const auto result = tinyhttps::probe_url(args.value("url", ""), socketTimeout, probe_loopback);
    stream.emit(DataEvent{"network_probe", nlohmann::json{
        {"host", result.host}, {"status", result.status}, {"http_status", result.httpStatus},
        {"elapsed_seconds", result.elapsedSeconds}, {"proxy", result.proxy}, {"reachable", result.reachable}}.dump()});
    return exit_result(0);
}

auto ListVersions::spec() const -> CapabilitySpec {
    return {
        .name = "list_installed_versions",
        .description = "List installed versions for a package and show which is active. Use plain name (e.g. gcc not xim:gcc)",
        .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Plain package name, e.g. gcc, node"}},"required":["target"]})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto ListVersions::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(xvm::cmd_list_versions(json.value("target", ""), stream));
}

auto UseVersion::spec() const -> CapabilitySpec {
    return {
        .name = "use_version",
        .description = "Switch to a specific version of a package. Use plain name (e.g. gcc not xim:gcc)",
        .inputSchema = R"JSON({"type":"object","properties":{"target":{"type":"string","description":"Plain package name, e.g. gcc, node"},"version":{"type":"string","description":"Version to switch to, e.g. 15, 22"}},"required":["target","version"]})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto UseVersion::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(xvm::cmd_use(json.value("target", ""), json.value("version", ""), stream));
}

auto SystemStatus::spec() const -> CapabilitySpec {
    return {
        .name = "system_status",
        .description = "Show xlings system configuration and status",
        .inputSchema = R"({"type":"object","properties":{}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto SystemStatus::execute(Params params, EventStream& stream) -> Result {
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

    // The binding state as `self doctor` reads it, for a client that manages
    // this home and reports on it without running doctor (feature
    // `binding_state`, protocol 1.11; mcpp design 2026-10-17 X3). Read-only,
    // and the same inspectors doctor renders.
    nlohmann::json findings = nlohmann::json::array();
    const auto add = [&](const xvm::BindingFinding& f, std::string_view subos) {
        findings.push_back({{"code", f.code}, {"severity", f.severity == xvm::BindingSeverity::Broken ? "broken" : "notice"},
                            {"subos", subos}, {"target", f.target}, {"version", f.version},
                            {"summary", f.summary}, {"hint", f.hint}});
    };
    const auto db = Config::versions();
    const auto active = Config::effective_workspace();
    for (const auto& f : xvm::inspect_binding_state(db, active)) add(f, p.activeSubos);
    for (const auto& [target, version] :
         xvm::plan_scope_reference_repair(db, active, Config::workspace_installed()).dropInstalled)
        add({.code = "xvm-installed-version-missing", .summary = "an installed[] entry is not registered",
             .target = target, .version = version,
             .hint = "run `xlings self doctor --fix` to drop it",
             .severity = xvm::BindingSeverity::Notice}, p.activeSubos);
    std::error_code cec;
    const auto here = std::filesystem::weakly_canonical(p.subosDir, cec);
    for (const auto& snapshot : profile::load_subos_snapshots(p.homeDir)) {
        std::error_code sec;
        if (std::filesystem::weakly_canonical(snapshot.dir, sec) == here) continue;
        const xvm::SubosRef other{.subos = snapshot.name, .active = snapshot.workspace.active,
                                  .installed = snapshot.workspace.installed};
        for (const auto& f : xvm::inspect_subos_references(db, {other})) add(f, snapshot.name);
    }
    stream.emit(DataEvent{"binding_state", nlohmann::json{{"findings", std::move(findings)}}.dump()});
    return exit_result(0);
}

auto ListIndexVersions::spec() const -> CapabilitySpec {
    return {
        .name = "list_index_versions",
        .description = "List published index snapshots and which one is in use",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto ListIndexVersions::execute(Params params, EventStream& stream) -> Result {
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

auto ListSubos::spec() const -> CapabilitySpec {
    return {
        .name = "list_subos",
        .description = "List all sub-OSs and which one is active",
        .inputSchema = R"({"type":"object","properties":{}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto ListSubos::execute(Params, EventStream& stream) -> Result {
    return exit_result(subos::list(stream));
}

auto ListSubosShims::spec() const -> CapabilitySpec {
    return {
        .name = "list_subos_shims",
        .description = "List installed shim binaries in the active sub-OS bin directory",
        .inputSchema = R"({"type":"object","properties":{}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto ListSubosShims::execute(Params, EventStream& stream) -> Result {
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

auto CreateSubos::spec() const -> CapabilitySpec {
    return {
        .name = "create_subos",
        .description = "Create a new sub-OS. The name must be alphanumeric (plus _ or -); 'current' is reserved",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"dir":{"type":"string","description":"Optional custom directory; defaults to $XLINGS_HOME/subos/<name>"},"yes":{"type":"boolean","description":"The user confirmed adopting a directory that already exists and holds files. Without it such a call changes nothing and reports what is there"}},"required":["name"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto CreateSubos::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    auto name = json.value("name", "");
    auto dir  = json.value("dir", "");
    return exit_result(subos::create(
        name,
        dir.empty() ? std::filesystem::path{} : std::filesystem::path{dir},
        subos::sandbox::StorageMode::Shared, "50G", "", explicit_yes_(json), "yes:true",
        stream));
}

auto SubosCommand::spec() const -> CapabilitySpec {
    return {
        .name = "subos_command",
        .description = "Run one `xlings subos` subcommand (argv after `subos`, e.g. [\"new\",\"box\",\"--from\",\"subos:luban-core\"]); "
                       "its log lines, progress, data and errors are the events. A confirmation it needs is given with \"yes\": true",
        .inputSchema = R"({"type":"object","properties":{"argv":{"type":"array","items":{"type":"string"},"description":"The subcommand and its arguments"},"yes":{"type":"boolean","description":"The user confirmed what the subcommand asks"}},"required":["argv"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto SubosCommand::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    std::vector<std::string> words{"xlings", "subos"};
    if (auto argv = json.find("argv"); argv != json.end() && argv->is_array())
        for (const auto& a : *argv)
            if (a.is_string()) words.push_back(a.get<std::string>());
    if (words.size() < 3) {
        stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = "argv names no subcommand", .recoverable = false});
        return nlohmann::json({{"exitCode", 2}}).dump();
    }
    if (explicit_yes_(json) && std::ranges::find(words, std::string("-y")) == words.end()
        && std::ranges::find(words, std::string("--yes")) == words.end())
        words.push_back("-y");
    std::vector<char*> argv;
    for (auto& w : words) argv.push_back(w.data());
    argv.push_back(nullptr);
    return exit_result(subos::run(static_cast<int>(words.size()), argv.data(), stream));
}

auto SwitchSubos::spec() const -> CapabilitySpec {
    return {
        .name = "switch_subos",
        .description = "Switch the active sub-OS to <name>",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto SwitchSubos::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(subos::use(json.value("name", ""), stream));
}

auto RemoveSubos::spec() const -> CapabilitySpec {
    return {
        .name = "remove_subos",
        .description = "Remove a sub-OS, deleting its home and every file in it. Refuses to remove 'default' or the currently active sub-OS. Deletes only with \"yes\": true -- the user's confirmation; without it nothing is removed and the error says what would be deleted",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"yes":{"type":"boolean","description":"The user confirmed deleting this sub-OS and its data"}},"required":["name"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto RemoveSubos::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(subos::remove(json.value("name", ""), explicit_yes_(json),
                                     "yes:true", stream));
}

auto ListRepos::spec() const -> CapabilitySpec {
    return {
        .name = "list_repos",
        .description = "List configured xim package index repositories (global + project)",
        .inputSchema = R"({"type":"object","properties":{}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto ListRepos::execute(Params, EventStream& stream) -> Result {
    nlohmann::json repos = nlohmann::json::array();
    // #598: the artifact chain is part of what a repo IS -- reporting only the
    // git url describes the fallback and hides the path actually taken.
    auto describe = [](const IndexRepo& r, const char* scope) {
        nlohmann::json bases = nlohmann::json::array();
        for (auto& ab : r.artifactBases)
            bases.push_back({{"region", ab.region}, {"url", ab.url}});
        return nlohmann::json{{"name", r.name}, {"url", r.url}, {"scope", scope},
                              {"artifact_bases", std::move(bases)}};
    };
    for (auto& r : Config::global_index_repos()) repos.push_back(describe(r, "global"));
    if (Config::has_project_config()) {
        for (auto& r : Config::project_index_repos()) repos.push_back(describe(r, "project"));
    }
    nlohmann::json payload;
    payload["repos"] = std::move(repos);
    payload["override"] = (Config::has_project_config() && !Config::project_index_repos().empty())
                          ? "project" : "global";
    stream.emit(DataEvent{"repo_list", payload.dump()});
    return exit_result(0);
}

auto AddRepo::spec() const -> CapabilitySpec {
    return {
        .name = "add_repo",
        .description = "Add or update a global xim index repository. If the name already exists, the URL is updated in place",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"url":{"type":"string","description":"git URL of the index repository"}},"required":["name","url"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto AddRepo::execute(Params params, EventStream& stream) -> Result {
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

    // Same rule as `xlings config --index-repo`, from the same predicate: a
    // declared sub-index's own url configures it; another url under its name
    // would be two repositories answering to one namespace.
    {
        IndexRepo probe;
        probe.name = name;
        probe.url = url;
        switch (xim::classify_index_entry(probe)) {
        case xim::IndexEntryKind::NameCollision:
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = std::format(
                    "'{}' is already a sub-index declared by the default index ({}); "
                    "two repositories cannot share one namespace",
                    name, xim::declared_sub_index_url(name)),
                .recoverable = true,
                .hint = "use another name",
            });
            return exit_result(1);
        case xim::IndexEntryKind::DeclaredSubIndex:
            stream.emit(LogEvent{
                .level = LogLevel::info,
                .message = std::format(
                    "'{}' is a sub-index declared by the default index; this entry "
                    "configures it and it stays a sub-index", name),
            });
            break;
        case xim::IndexEntryKind::Independent:
            break;
        }
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

auto RemoveRepo::spec() const -> CapabilitySpec {
    return {
        .name = "remove_repo",
        .description = "Remove a global xim index repository by name",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto RemoveRepo::execute(Params params, EventStream& stream) -> Result {
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

auto Env::spec() const -> CapabilitySpec {
    return {
        .name = "env",
        .description = "Return effective xlings environment (home, active sub-OS, paths, mirror, lang)",
        .inputSchema = R"({"type":"object","properties":{}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto Env::execute(Params, EventStream& stream) -> Result {
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


// ── sub-OS sessions over the interface ─────────────────────────

namespace xlings::capabilities {

namespace {

std::expected<subos::HomeView, std::string> scope_home_(std::string_view name) {
    const auto owner = subos::home_view();
    auto scope = home::domain_producer::read_scope(owner.home, name);
    if (!scope) return std::unexpected(scope.error());
    return *scope ? subos::HomeView{(**scope).domain.physicalHome} : owner;
}

// `xlings subos ...` as a child of this process, its stdout and stderr
// captured. The interface's own stdout is the NDJSON channel: a command's
// output must never reach it unframed, so it runs in a process of its own and
// comes back as events.
int run_subos_child_(std::vector<std::string> args, EventStream& stream, std::string_view kind,
                      CancellationToken* cancel = nullptr) {
    args.insert(args.begin(), platform::get_executable_path().string());
    const auto base64 = [](std::string_view bytes) {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        unsigned value{}, bits{};
        for (const unsigned char byte : bytes) {
            value = (value << 8) | byte;
            bits += 8;
            while (bits >= 6) { bits -= 6; encoded += alphabet[(value >> bits) & 63]; }
        }
        if (bits) encoded += alphabet[(value << (6 - bits)) & 63];
        while (encoded.size() % 4) encoded += '=';
        return encoded;
    };
    const auto code = platform::stream::run(args, [&](std::string_view channel, std::string_view bytes) {
        nlohmann::json payload{{"stream", channel}, {"data", std::string(bytes)}};
        std::string content;
        try { content = payload.dump(); }
        catch (const nlohmann::json::type_error&) {
            payload["encoding"] = "base64";
            payload["data"] = base64(bytes);
            content = payload.dump();
        }
        stream.emit(DataEvent{std::string(kind), std::move(content)});
    }, [&] { return cancel && cancel->is_cancelled(); });
    if (cancel) cancel->throw_if_cancelled();
    return code;
}

}  // namespace

auto SubosExec::spec() const -> CapabilitySpec {
    return {
        .name = "subos_exec",
        .description = "Run one command in a sub-OS from outside it, as argv (no shell quoting). Joins the sub-OS's running session when there is one. Output arrives as subos_exec_output data events; exitCode is the command's own, 125 when it never started, 126/127 when it cannot run / is not found, 124 on timeout, 128+n for signal n",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string","description":"Sub-OS name; omit with temp"},"argv":{"type":"array","items":{"type":"string"},"description":"The command and its arguments"},"sandbox":{"type":"boolean","description":"Run in the sub-OS's sandbox"},"cwd":{"type":"string"},"env":{"type":"object","additionalProperties":{"type":"string"}},"timeout":{"type":"string","description":"90, 30s, 10m, 2h"},"temp":{"type":"boolean","description":"A throwaway sub-OS, removed afterwards; its audit is kept"},"from":{"type":"string","description":"With temp: fork it from this sub-OS or package"}},"required":["argv"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto SubosExec::execute(Params params, EventStream& stream) -> Result {
    return execute(params, stream, nullptr);
}

auto SubosExec::execute(Params params, EventStream& stream, CancellationToken* cancel) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    std::vector<std::string> args{"subos", "exec"};
    if (json.value("temp", false)) args.push_back("--temp");
    else args.push_back(json.value("name", ""));
    if (json.value("sandbox", false)) args.push_back("--sandbox");
    if (auto v = json.value("cwd", ""); !v.empty()) args.insert(args.end(), {"--cwd", v});
    if (auto v = json.value("timeout", ""); !v.empty()) args.insert(args.end(), {"--timeout", v});
    if (auto v = json.value("from", ""); !v.empty()) args.insert(args.end(), {"--from", v});
    if (auto env = json.value("env", nlohmann::json::object()); env.is_object()) {
        for (auto it = env.begin(); it != env.end(); ++it)
            if (it.value().is_string()) args.insert(args.end(), {"--env", it.key() + "=" + it.value().get<std::string>()});
    }
    args.push_back("--");
    for (auto& a : json.value("argv", nlohmann::json::array()))
        if (a.is_string()) args.push_back(a.get<std::string>());
    return exit_result(run_subos_child_(std::move(args), stream, "subos_exec_output", cancel));
}

auto SubosEnv::spec() const -> CapabilitySpec {
    return {
        .name = "subos_env",
        .description = "The variables a sub-OS declares for the programs run in it (its packages' environment: GL driver and EGL vendor paths and the like), resolved as `subos use` applies them: a subos_env data event with vars [{var, op (set | prepend), value}]. Omit name for the active sub-OS",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string","description":"Sub-OS name; omit for the active one"}}})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto SubosEnv::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    auto name = json.is_object() ? json.value("name", std::string{}) : std::string{};
    if (name.empty()) name = Config::subos_scope().name;
    if (!std::filesystem::is_directory(Config::subos_dir(name))) {
        stream.emit(ErrorEvent{.code = ErrorCode::NotFound, .message = std::format("subos '{}' not found", name),
                               .recoverable = true, .hint = "xlings subos list"});
        return exit_result(1);
    }
    auto vars = nlohmann::json::array();
    for (const auto& v : xlings::subos::declared_variables(name)) {
        if (v.unresolved) continue;
        vars.push_back({{"var", v.var}, {"op", v.op}, {"value", v.value}});
    }
    stream.emit(DataEvent{"subos_env", nlohmann::json{{"name", name}, {"vars", std::move(vars)}}.dump()});
    return exit_result(0);
}

auto SubosStart::spec() const -> CapabilitySpec {
    return {
        .name = "subos_start",
        .description = "Start a sub-OS session that runs without a terminal, for a series of subos_exec calls. ttl ends it after that long idle",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"ttl":{"type":"string","description":"90, 30s, 10m, 2h; omit to run until subos_stop"}},"required":["name"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto SubosStart::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    std::vector<std::string> args{"subos", "start", json.value("name", "")};
    if (auto v = json.value("ttl", ""); !v.empty()) args.insert(args.end(), {"--ttl", v});
    return exit_result(run_subos_child_(std::move(args), stream, "subos_start_output"));
}

auto SubosStop::spec() const -> CapabilitySpec {
    return {
        .name = "subos_stop",
        .description = "End a sub-OS's running session. Succeeds when none is running",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto SubosStop::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    return exit_result(run_subos_child_({"subos", "stop", json.value("name", "")}, stream,
                                        "subos_stop_output"));
}

auto SubosEvents::spec() const -> CapabilitySpec {
    return {
        .name = "subos_events",
        .description = "A sub-OS's audit events (sessions, commands, permission decisions), recorded outside its sandbox. Each event is a subos_event data event; variable values are never recorded, only names",
        .inputSchema = R"({"type":"object","properties":{"name":{"type":"string"},"kind":{"type":"string","description":"ops, lifecycle, perm, exec, net or fs"},"session":{"type":"string"},"limit":{"type":"integer","description":"The last N events; default 200"}},"required":["name"]})",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"},"count":{"type":"integer"}}})",
        .destructive = false,
    };
}

auto SubosEvents::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    const auto name = json.value("name", "");
    const auto kind = json.value("kind", "");
    const auto session = json.value("session", "");
    const auto limit = static_cast<std::size_t>(std::max(1, json.value("limit", 200)));
    const auto home = scope_home_(name);
    if (!home) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = home.error(), .recoverable = false });
        return exit_result(1);
    }
    auto events = observe::read(home->logs_dir(name) / "events.ndjson");
    std::vector<nlohmann::json> shown;
    for (auto& e : events) {
        if (!kind.empty() && e.value("kind", "") != kind) continue;
        if (!session.empty() && e.value("session", "") != session) continue;
        shown.push_back(std::move(e));
    }
    const auto from = shown.size() > limit ? shown.size() - limit : 0;
    for (auto i = from; i < shown.size(); ++i)
        stream.emit(DataEvent{"subos_event", shown[i].dump()});
    return nlohmann::json({{"exitCode", 0}, {"count", shown.size() - from}}).dump();
}

}  // namespace xlings::capabilities
