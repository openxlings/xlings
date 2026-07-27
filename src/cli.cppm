module;

#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"

export module xlings.cli;

import std;

import mcpplibs.cmdline;
import mcpplibs.capi.lua;
import mcpplibs.xpkg.executor;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.runtime;
import xlings.ui;
import xlings.core.i18n;
import xlings.platform;
import xlings.capabilities;
import xlings.agent;
import xlings.agent.text_renderer;
import xlings.interface;
import xlings.core.subos;
import xlings.core.xself;
import xlings.core.xim.commands;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.commands;
import xlings.core.utf8;

namespace lua = mcpplibs::capi::lua;

namespace xlings::cli {

// ─── EventStream consumer: dispatch DataEvent to ui:: functions ───

static void dispatch_data_event(const DataEvent& e) {
    auto json = nlohmann::json::parse(e.json, nullptr, false);
    if (json.is_discarded()) {
        log::warn("invalid JSON in DataEvent kind={}", e.kind);
        return;
    }

    if (e.kind == "info_panel") {
        std::string title = json.value("title", "");
        std::vector<ui::InfoField> fields;
        if (json.contains("fields") && json["fields"].is_array()) {
            for (auto& f : json["fields"]) {
                fields.push_back({
                    f.value("label", ""),
                    f.value("value", ""),
                    f.value("highlight", false)
                });
            }
        }
        std::vector<ui::InfoField> extra;
        if (json.contains("extra_fields") && json["extra_fields"].is_array()) {
            for (auto& f : json["extra_fields"]) {
                extra.push_back({
                    f.value("label", ""),
                    f.value("value", ""),
                    f.value("highlight", false)
                });
            }
        }
        ui::print_info_panel(title, fields, extra);
    }
    else if (e.kind == "help") {
        std::string name = json.value("name", "");
        std::string desc = json.value("description", "");
        std::vector<ui::HelpArg> args;
        if (json.contains("args") && json["args"].is_array()) {
            for (auto& a : json["args"]) {
                args.push_back({
                    a.value("name", ""),
                    a.value("desc", ""),
                    a.value("required", false)
                });
            }
        }
        std::vector<ui::HelpOpt> opts;
        if (json.contains("opts") && json["opts"].is_array()) {
            for (auto& o : json["opts"]) {
                opts.push_back({
                    o.value("name", ""),
                    o.value("desc", "")
                });
            }
        }
        ui::print_subcommand_help(name, desc, args, opts);
    }
    else if (e.kind == "styled_list") {
        std::string title = json.value("title", "");
        bool numbered = json.value("numbered", false);
        std::vector<std::pair<std::string, std::string>> items;
        if (json.contains("items") && json["items"].is_array()) {
            for (auto& item : json["items"]) {
                if (item.is_array() && item.size() >= 2) {
                    items.emplace_back(item[0].get<std::string>(), item[1].get<std::string>());
                } else if (item.is_string()) {
                    items.emplace_back(item.get<std::string>(), "");
                }
            }
        }
        ui::print_styled_list(title, items, numbered);
    }
    else if (e.kind == "install_plan") {
        std::vector<std::pair<std::string, std::string>> packages;
        if (json.contains("packages") && json["packages"].is_array()) {
            for (auto& p : json["packages"]) {
                if (p.is_array() && p.size() >= 2) {
                    packages.emplace_back(p[0].get<std::string>(), p[1].get<std::string>());
                }
            }
        }
        ui::print_install_plan(packages);
    }
    else if (e.kind == "install_summary") {
        ui::print_install_summary(json.value("success", 0), json.value("failed", 0));
    }
    else if (e.kind == "remove_plan") {
        ui::print_remove_plan(
            json.value("subos", ""),
            json.value("name", ""),
            json.value("version", ""));
    }
    else if (e.kind == "remove_summary") {
        // Tolerate the legacy "target" key (older interface clients) by
        // falling back when name/version were not provided.
        auto name = json.value("name", json.value("target", ""));
        ui::print_remove_summary(
            json.value("subos", ""),
            name,
            json.value("version", ""));
    }
    else if (e.kind == "subos_list") {
        std::vector<std::tuple<std::string, std::string, int, bool>> entries;
        if (json.contains("entries") && json["entries"].is_array()) {
            for (auto& e : json["entries"]) {
                entries.emplace_back(
                    e.value("name", ""),
                    e.value("dir", ""),
                    e.value("pkgCount", 0),
                    e.value("active", false)
                );
            }
        }
        ui::print_subos_list(entries);
    }
    else if (e.kind == "subos_created") {
        ui::print_subos_created(
            json.value("name", ""), json.value("dir", ""));
    }
    else if (e.kind == "subos_switched") {
        ui::print_subos_switched(
            json.value("name", ""), json.value("dir", ""));
    }
    else if (e.kind == "subos_entering") {
        ui::print_subos_entering(json.value("name", ""));
    }
    else if (e.kind == "subos_already_in") {
        ui::print_subos_already_in(json.value("name", ""));
    }
    else if (e.kind == "subos_nesting") {
        ui::print_subos_nesting(
            json.value("from", ""), json.value("to", ""));
    }
    else if (e.kind == "subos_removed") {
        ui::print_subos_removed(json.value("name", ""));
    }
    else if (e.kind == "search_results") {
        std::vector<std::pair<std::string, std::string>> results;
        if (json.contains("results") && json["results"].is_array()) {
            for (auto& r : json["results"]) {
                if (r.is_array() && r.size() >= 2) {
                    results.emplace_back(r[0].get<std::string>(), r[1].get<std::string>());
                }
            }
        }
        ui::print_search_results(results);
    }
    else if (e.kind == "table") {
        std::vector<std::string> headers;
        if (json.contains("headers") && json["headers"].is_array()) {
            for (auto& h : json["headers"]) {
                headers.push_back(h.get<std::string>());
            }
        }
        std::vector<std::vector<std::string>> rows;
        if (json.contains("rows") && json["rows"].is_array()) {
            for (auto& r : json["rows"]) {
                std::vector<std::string> row;
                if (r.is_array()) {
                    for (auto& c : r) row.push_back(c.get<std::string>());
                }
                rows.push_back(std::move(row));
            }
        }
        ui::print_table(headers, rows);
    }
    else if (e.kind == "download_progress") {
        // Skip CLI rendering in TUI mode (agent TUI handles progress separately)
        if (platform::is_tui_mode()) return;
        auto nameWidth = json.value("nameWidth", std::size_t{20});
        auto elapsedSec = json.value("elapsedSec", 0.0);
        auto sizesReady = json.value("sizesReady", false);
        std::vector<ui::DownloadProgressEntry> entries;
        if (json.contains("files") && json["files"].is_array()) {
            for (auto& f : json["files"]) {
                entries.push_back({
                    f.value("name", std::string{}),
                    f.value("totalBytes", 0.0),
                    f.value("downloadedBytes", 0.0),
                    f.value("started", false),
                    f.value("finished", false),
                    f.value("success", false)
                });
            }
        }
        auto prevLines = json.value("prevLines", 0);
        ui::render_download_progress(entries, nameWidth, elapsedSec, sizesReady, prevLines);
    }
    else {
        log::debug("unhandled DataEvent kind: {}", e.kind);
    }
}

// ─── EventStream consumer: handle PromptEvent via ui:: interactive functions ───

static void handle_prompt(EventStream& stream, const PromptEvent& p) {
    // Binary yes/no → confirm dialog
    if (p.options.size() == 2 && p.options[0] == "y" && p.options[1] == "n") {
        bool defaultYes = (p.defaultValue == "y");
        bool result = ui::confirm(p.question, defaultYes);
        stream.respond(p.id, result ? "y" : "n");
        return;
    }

    // Multiple options → package/version selector
    if (!p.options.empty()) {
        std::vector<std::pair<std::string, std::string>> items;
        for (auto& opt : p.options) {
            items.emplace_back(opt, "");
        }
        auto result = ui::select_package(items);
        stream.respond(p.id, result.value_or(""));
        return;
    }

    // Free input — use default
    stream.respond(p.id, p.defaultValue);
}

// Parse legacy config.xlings (Lua format) and extract workspace from the xim table.
// Returns empty workspace if file doesn't exist or has no xim/xlings_deps.
xvm::Workspace parse_legacy_config_(const std::filesystem::path& configFile) {
    namespace fs = std::filesystem;
    xvm::Workspace workspace;

    if (!fs::exists(configFile)) return workspace;

    auto* L = lua::L_newstate();
    if (!L) return workspace;
    lua::L_openlibs(L);

    // Provide a no-op is_host() so Lua files with conditionals don't error
    lua::L_dostring(L, "function is_host() return false end");

    if (lua::L_dofile(L, configFile.string().c_str()) != lua::OK) {
        log::warn("failed to parse legacy config: {}", lua::tostring(L, -1));
        lua::close(L);
        return workspace;
    }

    // Read xim table: { pkg_name = "version", ... }
    lua::getglobal(L, "xim");
    if (lua::type(L, -1) == lua::TTABLE) {
        lua::pushnil(L);
        while (lua::next(L, -2)) {
            if (lua::type(L, -2) == lua::TSTRING) {
                std::string key = lua::tostring(L, -2);
                // Skip non-package entries
                if (key != "xppcmds") {
                    std::string version;
                    if (lua::type(L, -1) == lua::TSTRING) {
                        version = lua::tostring(L, -1);
                    }
                    workspace[key] = version;
                }
            }
            lua::pop(L, 1); // pop value, keep key for next iteration
        }
    }
    lua::pop(L, 1); // pop xim

    // Fallback: older format uses xlings_deps = "cpp, vscode, mdbook"
    if (workspace.empty()) {
        lua::getglobal(L, "xlings_deps");
        if (lua::type(L, -1) == lua::TSTRING) {
            std::string deps = lua::tostring(L, -1);
            std::istringstream ss(deps);
            std::string token;
            while (std::getline(ss, token, ',')) {
                auto start = token.find_first_not_of(" \t");
                auto end = token.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    workspace[token.substr(start, end - start + 1)] = "";
                }
            }
        }
        lua::pop(L, 1);
    }

    lua::close(L);
    return workspace;
}

// Generate .xlings.json from a workspace map
void generate_xlings_json_(const std::filesystem::path& dir, const xvm::Workspace& workspace) {
    nlohmann::json ws;
    for (auto& [name, version] : workspace) {
        ws[name] = version;
    }
    nlohmann::json root;
    root["workspace"] = ws;
    auto outPath = dir / ".xlings.json";
    platform::write_string_to_file(outPath.string(), root.dump(2));
}

// Normalize a target-spec from positional args for the single-target
// commands (remove/update/info — `use` has its own list-versions
// semantic and parses inline).
//
// Accepted forms (equivalent):
//   1 positional, contains '@'   →  passed as-is  (e.g. "node@22.17.1")
//   1 positional, no '@'         →  passed as-is  (bare name; the cmd
//                                    decides what to do — typically
//                                    "use the active version")
//   2 positionals                →  folded into "<arg0>@<arg1>"
//
// Rejected:
//   3+ positionals
//   2 positionals where arg0 already contains '@' (ambiguous)
//
// Returns false (with a logged error) on bad input. Caller should
// `return 1` on false.
bool parse_target_spec_(const mcpplibs::cmdline::ParsedArgs& args,
                        std::string& out) {
    auto n = args.positional_count();
    if (n == 0) {
        log::error("missing target argument");
        return false;
    }
    if (n > 2) {
        log::error("too many positional arguments (expected 1 or 2, got {})", n);
        log::error("  hint: use `<name>@<version>` or `<name> <version>`");
        return false;
    }
    auto first = std::string(args.positional(0));
    if (n == 1) {
        out = std::move(first);
        return true;
    }
    // n == 2: "<name> <version>" form. The first arg must be a bare
    // name — if it already includes '@', the user gave us two version
    // hints and we can't reconcile.
    if (first.find('@') != std::string::npos) {
        auto bare = first.substr(0, first.find('@'));
        log::error("ambiguous target: '{}' already includes @<version>, "
                   "but a separate version '{}' was also given",
                   first, std::string(args.positional(1)));
        log::error("  hint: pick one of `{}` or `{} {}`",
                   first, bare, std::string(args.positional(1)));
        return false;
    }
    out = first + "@" + std::string(args.positional(1));
    return true;
}

// Install packages from project .xlings.json workspace
int install_from_project_config_(EventStream& stream) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return 1;
    auto homeDir = Config::paths().homeDir;

    // Walk up from cwd looking for .xlings.json with workspace
    fs::path cur = cwd;
    while (!cur.empty()) {
        auto curNorm = fs::weakly_canonical(cur, ec);
        auto homeNorm = fs::weakly_canonical(homeDir, ec);
        if (curNorm == homeNorm) {
            auto parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
            continue;
        }

        // Try .xlings.json first
        auto cfg = cur / ".xlings.json";
        if (fs::exists(cfg, ec) && fs::is_regular_file(cfg, ec)) {
            try {
                auto content = platform::read_file_to_string(cfg.string());
                auto json = nlohmann::json::parse(content, nullptr, false);
                if (!json.is_discarded() && json.contains("workspace") && json["workspace"].is_object()) {
                    auto workspace = xvm::workspace_from_json(json["workspace"]);
                    auto targets = Config::workspace_install_targets(workspace);
                    if (!targets.empty()) {
                        // Project-config install: the workspace file declares
                        // which versions should be active in this subos, so
                        // force-activate to keep workspace state in sync with
                        // the config.
                        return xim::cmd_install(targets, true, false, stream,
                                                /*forceGlobal=*/false,
                                                /*cancel=*/nullptr,
                                                /*dryRun=*/false,
                                                /*useAfterInstall=*/true);
                    }
                }
            } catch (...) {
                log::error("failed to parse {}", cfg.string());
                return 1;
            }
        }

        // Fallback: try legacy config.xlings (Lua format)
        auto legacyCfg = cur / "config.xlings";
        if (fs::exists(legacyCfg, ec) && fs::is_regular_file(legacyCfg, ec)) {
            auto workspace = parse_legacy_config_(legacyCfg);
            if (!workspace.empty()) {
                log::println("detected legacy config: {}", legacyCfg.string());
                log::println("generating .xlings.json from config.xlings ...");
                generate_xlings_json_(cur, workspace);
                log::println("generated: {}", (cur / ".xlings.json").string());
                auto targets = Config::workspace_install_targets(workspace);
                return xim::cmd_install(targets, true, false, stream,
                                        /*forceGlobal=*/false,
                                        /*cancel=*/nullptr,
                                        /*dryRun=*/false,
                                        /*useAfterInstall=*/true);
            }
        }

        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }

    ui::print_tip("create <project>/.xlings.json with workspace, or run `xlings install <package>`");
    return 0;
}

void apply_global_opts_(const mcpplibs::cmdline::ParsedArgs& args) {
    if (args.is_flag_set("verbose")) log::set_level(log::Level::Debug);
    if (args.is_flag_set("quiet")) log::set_level(log::Level::Error);
}


// config subcommand handler
int cmd_config_(const mcpplibs::cmdline::ParsedArgs& args, EventStream& stream) {
    // Edits are collected rather than applied, then replayed against the
    // document `update_home_config` re-reads under the state lock. Applying
    // them to a copy read here instead would write back every *other* key as
    // it looked before an install that finished in the meantime -- see
    // core/home_config.cppm. Validation and the user-facing logging stay out
    // here so a rejected argument never reaches the lock.
    std::vector<std::function<void(nlohmann::json&)>> edits;

    // --lang
    if (auto lang = args.value("lang")) {
        std::string value(*lang);
        edits.push_back([value](nlohmann::json& j) { j["lang"] = value; });
        log::println("lang = {}", value);
    }

    // --mirror
    if (auto mirror = args.value("mirror")) {
        std::string value(*mirror);
        edits.push_back([value](nlohmann::json& j) { j["mirror"] = value; });
        log::println("mirror = {}", value);
    }

    auto commit_edits = [&]() -> bool {
        if (edits.empty()) return true;
        auto committed = update_home_config(
            Config::paths().homeDir, [&](nlohmann::json& j) {
                for (const auto& edit : edits) edit(j);
                return true;
            });
        if (!committed) {
            log::error("{}", committed.error());
            return false;
        }
        return true;
    };

    // --add-xpkg
    if (auto xpkg = args.value("add-xpkg")) {
        if (!commit_edits()) return 1;
        return xim::cmd_add_xpkg(std::string(*xpkg), stream);
    }

    // --index-repo  namespace:https://....git
    if (auto repo = args.value("index-repo")) {
        std::string val(*repo);
        auto colonPos = val.find(':');
        // Find the colon that separates name from URL (skip scheme "https://")
        // Format: name:url  e.g. myrepo:https://github.com/user/repo.git
        if (colonPos == std::string::npos || colonPos == 0) {
            log::error("invalid format, expected: <namespace>:<url>");
            log::error("  e.g. myrepo:https://github.com/user/repo.git");
            return 1;
        }
        auto name = val.substr(0, colonPos);
        auto url = val.substr(colonPos + 1);
        if (url.empty()) {
            log::error("invalid format, expected: <namespace>:<url>");
            return 1;
        }

        // Upsert against the freshly-read document: a repo another process
        // added since we started must be updated in place, not duplicated.
        edits.push_back([name, url](nlohmann::json& json) {
            if (!json.contains("index_repos")
                || !json["index_repos"].is_array()) {
                json["index_repos"] = nlohmann::json::array();
            }
            for (auto& entry : json["index_repos"]) {
                if (entry.is_object() && entry.contains("name") &&
                    entry["name"].get<std::string>() == name) {
                    entry["url"] = url;
                    return;
                }
            }
            nlohmann::json entry;
            entry["name"] = name;
            entry["url"] = url;
            json["index_repos"].push_back(entry);
        });
        log::println("index-repo {} = {}", name, url);
    }

    if (!edits.empty()) {
        return commit_edits() ? 0 : 1;
    }

    // No options: show current config via TUI info panel
    auto& p = Config::paths();
    std::vector<ui::InfoField> fields;
    fields.push_back({"XLINGS_HOME", p.homeDir.string()});
    fields.push_back({"XLINGS_DATA", p.dataDir.string()});
    fields.push_back({"XLINGS_SUBOS", p.subosDir.string()});
    fields.push_back({"active subos", p.activeSubos, true});
    fields.push_back({"bin", p.binDir.string()});

    auto mirror = Config::mirror();
    if (!mirror.empty()) fields.push_back({"mirror", mirror});
    auto lang = Config::lang();
    if (!lang.empty()) fields.push_back({"lang", lang});

    auto& repos = Config::global_index_repos();
    for (auto& repo : repos) {
        fields.push_back({"index-repo", repo.name + " : " + repo.url});
    }

    if (Config::has_project_config()) {
        fields.push_back({"project data", Config::project_data_dir().string()});
        auto& projectRepos = Config::project_index_repos();
        for (auto& repo : projectRepos) {
            fields.push_back({"project repo", repo.name + " : " + repo.url});
        }
    }

    ui::print_info_panel("xlings config", fields);
    return 0;
}

export int run(int argc, char* argv[]) {
    using namespace mcpplibs;

    // Bare `xlings` (no args) used to silently exit 0 because the
    // help branch below requires fargc >= 2 (i.e. at least one arg).
    // Treat it as `xlings --help` instead — much friendlier for first
    // contact and matches what most modern package managers do.
    if (argc <= 1) {
        ui::print_help(Info::VERSION);
        return 0;
    }

    // Create EventStream for core→UI decoupling
    EventStream stream;
    // Default TUI consumer for CLI mode (will be toggled in agent mode).
    // ErrorEvent/LogEvent surfacing keeps capability-emitted errors visible
    // to CLI users — without it, anything that emits an ErrorEvent (subos
    // operations, repo helpers) would silently fail in CLI mode.
    int tui_listener = stream.on_event([&stream](const Event& e) {
        if (auto* d = std::get_if<DataEvent>(&e)) {
            dispatch_data_event(*d);
        }
        else if (auto* p = std::get_if<PromptEvent>(&e)) {
            handle_prompt(stream, *p);
        }
        else if (auto* er = std::get_if<ErrorEvent>(&e)) {
            log::error("{}", er->message);
            if (!er->hint.empty()) log::error("  {}", er->hint);
        }
        else if (auto* l = std::get_if<LogEvent>(&e)) {
            switch (l->level) {
                case LogLevel::debug: log::debug("{}", l->message); break;
                case LogLevel::info:  log::info("{}",  l->message); break;
                case LogLevel::warn:  log::warn("{}",  l->message); break;
                case LogLevel::error: log::error("{}", l->message); break;
            }
        }
    });

    // Build Capability Registry (for Agent/MCP use — CLI keeps direct dispatch)
    auto registry = capabilities::build_registry();

    // Scan for global flags (--verbose, -v, --quiet, -q, --agent) anywhere
    // in argv so they work regardless of position.
    bool agent_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a { argv[i] };
        if (a == "--verbose" || a == "-v") log::set_level(log::Level::Debug);
        else if (a == "--quiet" || a == "-q") log::set_level(log::Level::Error);
        else if (a == "--agent") agent_mode = true;
    }

    // --agent: replace TUI listener with plain-text renderer, disable colors.
    // Unlike interface mode, we do NOT set tui_mode(true) — log output should
    // still reach the terminal, just without ANSI decoration.
    if (agent_mode) {
        stream.set_enabled(tui_listener, false);
        log::enable_color(false);
        stream.on_event([&stream](const Event& e) {
            if (auto* d = std::get_if<DataEvent>(&e)) {
                agent::render_data_event(*d);
            }
            else if (auto* p = std::get_if<PromptEvent>(&e)) {
                // In agent mode, auto-respond with default (use --yes for confirms)
                stream.respond(p->id, p->defaultValue);
            }
            else if (auto* er = std::get_if<ErrorEvent>(&e)) {
                std::println(stderr, "Error: {}", er->message);
                if (!er->hint.empty()) std::println(stderr, "Hint: {}", er->hint);
            }
            else if (auto* l = std::get_if<LogEvent>(&e)) {
                if (l->level == LogLevel::error)
                    std::println(stderr, "{}", l->message);
                else if (l->level == LogLevel::warn)
                    std::println(stderr, "{}", l->message);
                else
                    std::println("{}", l->message);
            }
        });
    }

    // Build filtered argv without global flags for positional-arg handlers
    // (subos, self, script parse argv[2]/argv[3] positionally)
    std::vector<char*> fargv;
    fargv.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string_view a { argv[i] };
        if (a == "--verbose" || a == "-v" || a == "--quiet" || a == "-q"
            || a == "--agent") continue;
        fargv.push_back(argv[i]);
    }
    int fargc = static_cast<int>(fargv.size());

    // Find the first non-flag argument as the command name
    std::string cmd;
    for (int i = 1; i < fargc; ++i) {
        std::string_view a { fargv[i] };
        if (!a.starts_with("-")) { cmd = std::string(a); break; }
    }

    // Special: subos, self, script need raw argc/argv
    if (fargc >= 2) {
        // Handle -h/--help/--version before cmdline library to avoid
        // std::format width-specifier crash in GCC 15 C++23 modules.
        if (cmd == "-h" || cmd == "--help" || cmd.empty()) {
            if (cmd.empty()) {
                // Only flags, no command — check if -h was requested
                for (int i = 1; i < fargc; ++i) {
                    std::string_view a { fargv[i] };
                    if (a == "-h" || a == "--help") { ui::print_help(Info::VERSION); return 0; }
                    if (a == "--version") { std::println("xlings {}", Info::VERSION); return 0; }
                }
            }
            ui::print_help(Info::VERSION);
            return 0;
        }
        if (cmd == "--version") {
            std::println("xlings {}", Info::VERSION);
            return 0;
        }

        // agent — dispatched early so it handles its own -h as plain text
        if (cmd == "agent") return agent::run(fargc, fargv.data());

        // Intercept subcommand help: xlings <cmd> -h/--help
        bool wantsHelp = false;
        for (int i = 2; i < fargc; ++i) {
            std::string_view a { fargv[i] };
            if (a == "-h" || a == "--help") { wantsHelp = true; break; }
        }
        if (wantsHelp) {
            using A = ui::HelpArg;
            using O = ui::HelpOpt;

            struct SubHelp {
                std::string_view name;
                std::string_view desc;
                std::vector<A> args;
                std::vector<O> opts;
                std::vector<O> subcmds;
            };

            auto match = [&](std::string_view n) { return cmd == n; };

            std::optional<SubHelp> h;
            if (match("install")) h = SubHelp{
                "install", "Install packages (e.g. xlings install gcc@15 node)",
                { {"packages", "Package names with optional version"} },
                {
                    {"-g, --global", "Install to global scope (not project-local subos)"},
                    {"-u, --use",    "Activate the installed version even if another version is currently active"},
                },
            };
            else if (match("remove")) h = SubHelp{
                "remove", "Remove a package",
                {
                    {"package", "Package name (or name@ver)", true},
                    {"version", "Optional version (alternative to name@ver form)"},
                }, {},
            };
            else if (match("update")) h = SubHelp{
                "update", "Update package index or a specific package",
                {
                    {"package", "Package to update (omit for index only)"},
                    {"version", "Optional version (alternative to name@ver form)"},
                }, {},
            };
            else if (match("search")) h = SubHelp{
                "search", "Search for packages",
                { {"keyword", "Search keyword", true} }, {},
            };
            else if (match("list")) h = SubHelp{
                "list", "List installed packages",
                { {"filter", "Filter pattern"} }, {},
            };
            else if (match("info")) h = SubHelp{
                "info", "Show package information",
                {
                    {"package", "Package name (or name@ver)", true},
                    {"version", "Optional version (alternative to name@ver form)"},
                }, {},
            };
            else if (match("use")) h = SubHelp{
                "use", "Switch tool version",
                {
                    {"target",  "Tool name (or name@ver one-shot)", true},
                    {"version", "Version to switch to (omit to list)"},
                }, {},
            };
            else if (match("config")) h = SubHelp{
                "config", "Show or modify xlings configuration", {},
                {
                    {"--lang <LANG>",       "Set language (en/zh)"},
                    {"--mirror <MIRROR>",   "Set mirror (GLOBAL/CN)"},
                    {"--add-xpkg <FILE>",   "Add xpkg file to package index"},
                    {"--index-repo <NS:URL>", "Add/update index repo (e.g. myns:https://...git)"},
                },
            };
            else if (match("self")) h = SubHelp{
                "self", "Manage xlings itself", {},
                {
                    {"install",  "Install xlings from release package"},
                    {"init",     "Create home/data/subos dirs"},
                    {"update",   "Update index + install latest xlings"},
                    {"config",   "Show configuration details"},
                    {"clean",    "Remove cache + gc orphaned packages (--dry-run)"},
                    {"migrate",  "Migrate old layout to subos/default"},
                    {"doctor",   "Verify workspace/shim consistency (--fix to repair, --reset-metadata to discard unreadable release metadata)"},
                },
            };
            else if (match("subos")) h = SubHelp{
                "subos", "Manage sub-OS environments", {},
                {
                    {"new <name>",    "Create a new sub-OS"},
                    {"use <name>",    "Switch active sub-OS"},
                    {"list",          "List all sub-OS environments"},
                    {"remove <name>", "Remove a sub-OS"},
                    {"info [name]",   "Show sub-OS details"},
                },
            };
            else if (match("script")) h = SubHelp{
                "script", "Run xlings scripts",
                { {"script-file", "Path to script file", true}, {"args", "Script arguments"} }, {},
            };

            if (h) {
                ui::print_subcommand_help(h->name, h->desc, h->args, h->opts, h->subcmds);
                return 0;
            }
        }

        // Detect unknown commands early — show TUI error + help
        {
            static constexpr std::string_view known_cmds[] = {
                "install", "remove", "update", "search", "list",
                "info", "use", "config", "subos", "self", "script",
                "interface", "agent",
            };
            bool known = false;
            for (auto& k : known_cmds) {
                if (cmd == k) { known = true; break; }
            }
            if (!known && !cmd.starts_with("-")) {
                log::error("unknown command: {}", cmd);
                ui::print_help(Info::VERSION);
                return 1;
            }
        }

        if (cmd == "subos") return subos::run(fargc, fargv.data(), stream);
        if (cmd == "self") return xself::run(fargc, fargv.data(), stream);
        if (cmd == "script") {
            if (fargc < 3) {
                ui::print_usage("xlings script <script-file> [args...]");
                return 1;
            }
            namespace fs = std::filesystem;
            fs::path scriptFile = fargv[2];
            auto execResult = mcpplibs::xpkg::create_executor(scriptFile);
            if (!execResult) {
                log::error("failed to load script: {}", execResult.error());
                return 1;
            }
            mcpplibs::xpkg::ExecutionContext ctx;
            ctx.platform = std::string(platform::OS_NAME);
            ctx.bin_dir = Config::paths().binDir;
            ctx.subos_sysrootdir = Config::paths().subosDir.string();
            ctx.run_dir = fs::current_path();
            ctx.xpkg_dir = Config::paths().dataDir / "xpkgs";
            for (int i = 3; i < fargc; ++i) {
                ctx.args.emplace_back(fargv[i]);
            }
            auto result = execResult->run_script(ctx);
            if (!result.success) {
                log::error("script error: {}", result.error);
                return 1;
            }
            return 0;
        }
    }

    // cmdline::App stores actions as std::function<void(ParsedArgs&)>, so the
    // int returns from each action lambda below are silently discarded by the
    // library on its way through std::function. Capture the lambda's intended
    // exit code into this shared int and have run() return it after app.run()
    // completes. Without this every cmd_* return value (1 for failures, 2 for
    // refused-by-policy, ...) is squashed to 0 — so e.g. CI scripts that branch
    // on `xlings install` exit code never see install failures.
    int action_rc = 0;
    auto wrap_rc = [&action_rc](auto&& fn) {
        return [fn = std::forward<decltype(fn)>(fn), &action_rc]
               (const cmdline::ParsedArgs& args) {
            action_rc = fn(args);
        };
    };

    auto app = cmdline::App("xlings")
        .version(std::string(Info::VERSION))
        .author("d2learn community")
        .description("A modern package manager and development environment tool")

        // Global options
        .option("yes").short_name('y').help("Skip confirmation prompts").global()
        .option("verbose").short_name('v').help("Enable verbose output").global()
        .option("quiet").short_name('q').help("Suppress non-essential output").global()
        .option("agent").help("Plain-text output without TUI formatting (for LLM agents)").global()

        // install
        .subcommand("install")
            .description("Install packages (e.g. xlings install gcc@15 node)")
            .option(cmdline::Option("global").short_name('g').help("Install to global scope (not project-local subos)"))
            .option(cmdline::Option("use").short_name('u').help("Activate the installed version even if another version is currently active"))
            .arg("packages").help("Package names with optional version")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                std::vector<std::string> targets;
                for (std::size_t i = 0; i < args.positional_count(); ++i) {
                    auto t = args.positional(i);
                    if (!t.empty()) targets.emplace_back(t);
                }
                // Friendly hint: `install` treats every positional as an
                // independent target (multi-package semantic). If the user
                // wrote two args where the first looks like a bare package
                // name and the second looks version-like, they probably
                // meant the combined `name@version` form (single package),
                // which is the convention the rest of the CLI follows.
                if (targets.size() == 2
                    && targets[0].find('@') == std::string::npos
                    && !targets[1].empty()
                    && (std::isdigit(static_cast<unsigned char>(targets[1][0]))
                        || targets[1] == "latest"))
                {
                    log::warn("install: each positional is a separate package; "
                              "did you mean `{}@{}` (single package) instead "
                              "of two packages '{}' and '{}'?",
                              targets[0], targets[1], targets[0], targets[1]);
                }

                if (targets.empty()) return install_from_project_config_(stream);

                bool yes = args.is_flag_set("yes");
                bool global = args.is_flag_set("global");
                bool useAfter = args.is_flag_set("use");
                return xim::cmd_install(targets, yes, false, stream, global,
                                        /*cancel=*/nullptr, /*dryRun=*/false,
                                        useAfter);
            }))

        // remove
        .subcommand("remove")
            .description("Remove a package")
            .arg("package").required().help("Package to remove (name or name@ver)")
            .arg("version").help("Optional version (alternative to name@ver form)")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                std::string target;
                if (!parse_target_spec_(args, target)) return 1;
                bool yes = args.is_flag_set("yes");
                return xim::cmd_remove(target, yes, stream);
            }))

        // update
        .subcommand("update")
            .description("Update package index or a specific package")
            .arg("package").help("Package to update (omit for index only)")
            .arg("version").help("Optional version (alternative to name@ver form)")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                std::string target;
                if (args.positional_count() > 0) {
                    if (!parse_target_spec_(args, target)) return 1;
                }
                bool yes = args.is_flag_set("yes");
                return xim::cmd_update(target, yes, stream);
            }))

        // search
        .subcommand("search")
            .description("Search for packages")
            .arg("keyword").required().help("Search keyword")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                return xim::cmd_search(std::string(args.positional(0)), stream);
            }))

        // list
        .subcommand("list")
            .description("List installed packages")
            .option(cmdline::Option("all").short_name('a').help("Show packages across all subos (default: current subos only)"))
            .arg("filter").help("Filter pattern")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                std::string filter;
                if (args.positional_count() > 0)
                    filter = std::string(args.positional(0));
                bool show_all = args.is_flag_set("all");
                return xim::cmd_list(filter, stream, show_all);
            }))

        // info
        .subcommand("info")
            .description("Show package information")
            .arg("package").required().help("Package name (or name@ver)")
            .arg("version").help("Optional version (alternative to name@ver form)")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                std::string target;
                if (!parse_target_spec_(args, target)) return 1;
                return xim::cmd_info(target, stream);
            }))

        // use — accepts both `<name> <ver>` (legacy form) and `<name>@<ver>`
        // (one-shot form, matching install/remove). Bare `<name>` lists
        // installed versions in the current subos; pass `--all` to widen
        // to the global view (every version every subos has ever installed).
        .subcommand("use")
            .description("Switch tool version")
            .option(cmdline::Option("all").short_name('a').help("Show versions across all subos (default: current subos only)"))
            .arg("target").required().help("Tool name (or name@ver one-shot)")
            .arg("version").help("Version to switch to (omit to list installed versions)")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                auto n = args.positional_count();
                if (n == 0) {
                    log::error("missing target argument");
                    return 1;
                }
                if (n > 2) {
                    log::error("too many positional arguments (expected 1 or 2, got {})", n);
                    log::error("  hint: use `<name>@<version>` or `<name> <version>`");
                    return 1;
                }
                bool show_all = args.is_flag_set("all");
                auto first = std::string(args.positional(0));
                if (n == 1) {
                    auto at = first.find('@');
                    if (at == std::string::npos)
                        return xvm::cmd_list_versions(first, stream, show_all);
                    return xvm::cmd_use(first.substr(0, at),
                                        first.substr(at + 1), stream);
                }
                // n == 2: must be `<name> <version>`
                if (first.find('@') != std::string::npos) {
                    auto bare = first.substr(0, first.find('@'));
                    log::error("ambiguous target: '{}' already includes "
                               "@<version>, but a separate version '{}' was "
                               "also given", first,
                               std::string(args.positional(1)));
                    log::error("  hint: pick one of `{}` or `{} {}`",
                               first, bare, std::string(args.positional(1)));
                    return 1;
                }
                return xvm::cmd_use(first,
                                    std::string(args.positional(1)), stream);
            }))

        // config
        .subcommand("config")
            .description("Show or modify xlings configuration")
            .option(cmdline::Option("lang").takes_value().value_name("LANG").help("Set language (en/zh)"))
            .option(cmdline::Option("mirror").takes_value().value_name("MIRROR").help("Set mirror (GLOBAL/CN)"))
            .option(cmdline::Option("add-xpkg").takes_value().value_name("FILE").help("Add xpkg file to package index"))
            .option(cmdline::Option("index-repo").takes_value().value_name("NS:URL").help("Add/update index repo (e.g. myns:https://...git)"))
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                return cmd_config_(args, stream);
            }))

        // interface — programmatic JSON API (NDJSON over stdio).
        // See docs/plans/2026-04-25-interface-api-v1.md for full protocol spec.
        .subcommand("interface")
            .description("Programmatic JSON API for external tools (NDJSON over stdio)")
            .option(cmdline::Option("args").takes_value().value_name("JSON")
                .help("Capability arguments as JSON string"))
            .option(cmdline::Option("args-file").takes_value().value_name("PATH")
                .help("Read capability arguments from a file (avoids cmd.exe quoting on Windows)"))
            .option(cmdline::Option("list").help("List all available capabilities with schemas"))
            .option(cmdline::Option("version").help("Print protocol version and exit"))
            .arg("capability").help("Capability name to invoke")
            .action(wrap_rc([&stream, tui_listener, &registry](const cmdline::ParsedArgs& args) -> int {
                return interface::run(args, stream, tui_listener, registry);
            }));

    // Top-level catch: any uncaught std::exception (most commonly
    // std::filesystem::filesystem_error from a missing error_code
    // overload, but also out-of-memory, invalid_argument from JSON parsing,
    // etc.) would otherwise propagate out of main() and trigger
    // std::terminate(). On Windows, terminate() does not flush stdio
    // buffers, so any log::error already queued is lost — CI sees a
    // silent non-zero exit. Convert to a logged error + non-zero return
    // so the user always sees what went wrong.
    try {
        // app.run returns its own status (1 for parse errors, 0 on
        // successful dispatch). On successful dispatch, the action
        // lambda's intended exit code lives in `action_rc` (cmdline lib
        // discards it on its own). Surface the action's rc when app.run
        // succeeded so e.g. `xlings install <bad-pkg>` returns non-zero
        // to scripts and CI.
        auto app_rc = app.run(argc, argv);
        if (app_rc != 0) return app_rc;
        return action_rc;
    } catch (const std::filesystem::filesystem_error& e) {
        log::error("filesystem error: {}", e.what());
        if (!e.path1().empty()) log::error("  path: {}", e.path1().string());
        log::error("  hint: this is likely a bug; please report at "
                   "https://github.com/openxlings/xlings/issues");
        return 1;
    } catch (const std::exception& e) {
        log::error("internal error: {}", e.what());
        log::error("  hint: this is likely a bug; please report at "
                   "https://github.com/openxlings/xlings/issues");
        return 1;
    } catch (...) {
        log::error("internal error: unknown exception");
        return 1;
    }
}

} // namespace xlings::cli
