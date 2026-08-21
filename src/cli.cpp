module;

#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"

#include <cstdio>

module xlings.cli;

import std;
import mcpplibs.cmdline;
import mcpplibs.capi.lua;
import mcpplibs.xpkg.executor;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.core.diag;
import xlings.core.uimode;
import xlings.theme;
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
import xlings.core.profile;
import xlings.core.utf8;
import xlings.cli.spec;
import xlings.core.xim.index_cmd;

namespace xlings::cli {

// ─── EventStream consumer: dispatch DataEvent to ui:: functions ───
// Kinds that reach the wire but deliberately have no terminal renderer.
//
// Two different reasons, both legitimate, and the difference has to be written
// down or the check below cannot tell either of them from "somebody forgot":
//
//   * capability-only -- emitted from `capabilities.cpp`, which the CLI does
//     not route through. They exist for `xlings interface` / MCP, where the
//     NDJSON writer serialises any kind generically.
//   * wire duplicate -- emitted on a CLI path, but the terminal output for
//     that path already comes from `log::` right next to the emit. The event
//     carries the same facts in structured form for programmatic consumers.
//     These are the ones that should eventually become `View::Document` and
//     lose their `log::` twin; until then they are duplicated on purpose, not
//     dropped.
//
// Anything NOT on this list and NOT rendered above is a screen the user never
// sees. `subos new --from` shipped in exactly that state.
bool kind_is_interface_only_(std::string_view kind) {
    static constexpr std::string_view kCapabilityOnly[] = {
        "system_info", "index_versions", "subos_shims", "repo_list", "env",
    };
    static constexpr std::string_view kWireDuplicate[] = {
        "remove_blocked", "update_plan", "update_summary",
    };
    return std::ranges::contains(kCapabilityOnly, kind)
        || std::ranges::contains(kWireDuplicate, kind);
}

void dispatch_data_event(const DataEvent& e) {
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
                    f.value("highlight", false),
                    f.value("alert", false)
                });
            }
        }
        std::vector<ui::InfoField> extra;
        if (json.contains("extra_fields") && json["extra_fields"].is_array()) {
            for (auto& f : json["extra_fields"]) {
                extra.push_back({
                    f.value("label", ""),
                    f.value("value", ""),
                    f.value("highlight", false),
                    f.value("alert", false)
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
    else if (e.kind == "tip") {
        ui::print_tip(json.value("message", ""));
    }
    else if (e.kind == "styled_list") {
        std::string title = json.value("title", "");
        bool numbered = json.value("numbered", false);
        std::vector<ui::ListRow> items;
        if (json.contains("items") && json["items"].is_array()) {
            for (auto& item : json["items"]) {
                if (item.is_array() && item.size() >= 2) {
                    items.push_back({
                        item[0].get<std::string>(),
                        item[1].get<std::string>(),
                        item.size() >= 3 ? item[2].get<std::string>()
                                         : std::string{},
                    });
                } else if (item.is_string()) {
                    items.push_back({item.get<std::string>(), "", ""});
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
        std::vector<std::string> pinnedBy;
        if (json.contains("pinned_by") && json["pinned_by"].is_array()) {
            for (auto& n : json["pinned_by"]) {
                if (n.is_string()) pinnedBy.push_back(n.get<std::string>());
            }
        }
        ui::print_remove_summary(
            json.value("subos", ""),
            name,
            json.value("version", ""),
            json.value("detached", false),
            pinnedBy);
    }
    else if (e.kind == "subos_candidates") {
        std::vector<std::tuple<std::string, std::string, int, bool>> entries;
        if (json.contains("candidates") && json["candidates"].is_array()) {
            for (auto& candidate : json["candidates"]) {
                entries.emplace_back(
                    candidate.value("name", ""),
                    candidate.value("dir", ""),
                    candidate.value("pkgCount", 0),
                    candidate.value("active", false));
            }
        }
        if (json.value("auto_selected", false)) {
            ui::print_subos_resolved(
                json.value("query", ""), json.value("selected", ""));
        } else {
            ui::print_subos_list(entries);
            auto hint = json.value("hint", "");
            if (!hint.empty()) ui::print_tip(hint);
        }
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
    else if (e.kind == "subos_forked") {
        ui::print_subos_forked(json.value("name", ""), json.value("from", ""),
                               json.value("base", ""));
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
    else if (!kind_is_interface_only_(e.kind)) {
        // A screen with no renderer is a screen the user never sees, and at
        // `debug` nobody finds out. `subos new --from` shipped in exactly this
        // state: one emit, zero consumers, no other output on the success
        // path -- it simply printed nothing and exited 0.
        //
        // Warn rather than error: an unrendered event is a gap in this
        // frontend, not a failed command, and failing the command would be a
        // worse bug than the one being reported.
        log::warn("no renderer for '{}' -- this screen was dropped; "
                  "please report it", e.kind);
    }
}

// ─── EventStream consumer: handle PromptEvent via ui:: interactive functions ───
// Told once, ever.
//
// Interactive prompts are ON by default for terminals, so the first one a user
// meets is unannounced -- they typed `xlings use gcc` and got a widget. One
// line the first time says what the keys are and how to turn it off for good;
// after that it would be nagging.
//
// Persisted in the home config rather than a process-local flag: the
// equivalent helper in `xself::repair` is a `static bool`, which means "once
// per run" and would print this on every single invocation.
void show_interactive_hint_once_() {
    constexpr std::string_view kId = "tui.interactive.first-run";
    if (Config::hint_seen(kId)) return;
    Config::mark_hint_seen(kId);
    diag::emit({
        .level   = diag::Level::Note,
        .code    = "ui.interactive_first_run",
        .summary = "xlings can ask instead of printing a list",
        .facts   = { { "keys", "up/down to move, enter to pick, esc to skip" } },
        .actions = { { "turn it off", "xlings config --interactive false" } },
    });
}

void handle_prompt(EventStream& stream, const PromptEvent& p) {
    // Binary yes/no → confirm dialog
    if (p.options.size() == 2 && p.options[0] == "y" && p.options[1] == "n") {
        bool defaultYes = (p.defaultValue == "y");
        bool result = ui::confirm(p.question, defaultYes);
        stream.respond(p.id, result ? "y" : "n");
        return;
    }

    // Multiple options → inline picker
    if (!p.options.empty()) {
        std::vector<std::pair<std::string, std::string>> items;
        int preselect = 0;
        for (auto& opt : p.options) {
            // Mark the current one. Without it the list is a set of equally
            // plausible strings and the user has to remember which they are
            // already on -- which is the question that sent them here.
            if (opt == p.defaultValue) preselect = static_cast<int>(items.size());
            items.emplace_back(opt, opt == p.defaultValue ? "(current)" : "");
        }
        show_interactive_hint_once_();
        // The question, not a fixed "Select a package:" -- the caller knows
        // what it is asking and the widget should not overwrite it.
        auto idx = ui::select_option(p.question, items, "", preselect);
        stream.respond(p.id, idx ? p.options[static_cast<std::size_t>(*idx)]
                                 : std::string{});
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

    // --ui-mode / --theme / --interactive
    //
    // Deliberately the same mechanism as --mirror: option -> collected edit ->
    // one locked read-modify-write of ~/.xlings.json -> read back by Config,
    // project layer overriding global. `lang` already proved that pipeline
    // works end to end (it even has an e2e); these keys just use it.
    if (auto mode = args.value("ui-mode")) {
        std::string value(*mode);
        if (value != "auto" && !ui::parse_mode(value)) {
            diag::emit({
                .code    = "cli.bad_ui_mode",
                .summary = std::format("'{}' is not a UI mode", value),
                .facts   = { { "valid", "cli, tui, auto" } },
                .actions = { { "set one", "xlings config --ui-mode tui" } },
            });
            return 2;
        }
        edits.push_back([value](nlohmann::json& j) { j["uiMode"] = value; });
        log::println("uiMode = {}", value);
    }
    if (auto themeArg = args.value("theme")) {
        std::string value(*themeArg);
        // `list` is a query, not an edit; handled after commit_edits below so
        // it cannot swallow edits collected alongside it.
        if (value != "list") {
            edits.push_back([value](nlohmann::json& j) { j["theme"] = value; });
            log::println("theme = {}", value);
        }
    }

    if (auto inter = args.value("interactive")) {
        std::string value(*inter);
        if (value != "true" && value != "false") {
            diag::emit({
                .code    = "cli.bad_interactive",
                .summary = std::format("'{}' is not true or false", value),
                .actions = { { "set one", "xlings config --interactive false" } },
            });
            return 2;
        }
        const bool on = (value == "true");
        edits.push_back([on](nlohmann::json& j) { j["tui"]["interactive"] = on; });
        log::println("tui.interactive = {}", value);
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

    // --theme list
    if (args.value("theme") == "list") {
        // Edits collected alongside it land first: `xlings config --mirror CN
        // --theme list` must not print the list and drop the mirror.
        if (!commit_edits()) return 1;
        log::println("themes:");
        log::println("  default            (built in)");
        const auto dir = Config::paths().homeDir / "config" / "themes";
        std::error_code lec;
        if (std::filesystem::is_directory(dir, lec)) {
            for (const auto& e : platform::dir_entries(dir)) {
                if (e.path().extension() != ".json") continue;
                log::println("  {:<18} {}", e.path().stem().string(),
                             Config::display_path(e.path()));
            }
        }
        log::println("");
        log::println("  active: {}", theme::current().name);
        // Said explicitly because the shipped files are overwritten on
        // upgrade -- editing one in place looks like it works right up until
        // the next release.
        log::println("  to customise: copy a file, then "
                     "`xlings config --theme ./my-theme.json`");
        return 0;
    }

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
    fields.push_back({"XLINGS_DATA", Config::display_path(p.dataDir)});
    fields.push_back({"XLINGS_SUBOS", Config::display_path(p.subosDir)});
    fields.push_back({"active subos", p.activeSubos, true});
    fields.push_back({"bin", Config::display_path(p.binDir)});

    auto mirror = Config::mirror();
    if (!mirror.empty()) fields.push_back({"mirror", mirror});
    auto lang = Config::lang();
    if (!lang.empty()) fields.push_back({"lang", lang});
    // Shown unconditionally, unlike mirror/lang: the resolved frontend is the
    // answer to "why does my output look like this", and hiding it when it was
    // auto-detected is exactly when the question gets asked.
    fields.push_back({"ui mode", std::string(ui::to_string(ui::current_mode()))});
    if (auto theme = Config::theme(); !theme.empty())
        fields.push_back({"theme", theme});
    if (auto inter = Config::tui_interactive())
        fields.push_back({"tui.interactive", *inter ? "true" : "false"});

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

// `xlings profile list|commit|rollback` — generations of the active subos.
//
// The module had commit / list_generations / rollback and no way to reach
// any of them: four exported functions, zero callers, no subcommand. So the
// feature read as working and was not, and `rollback` in particular wrote a
// YAML file nothing consulted.
//
// Commit is explicit rather than automatic. Recording a generation on every
// install would change the hot path for everyone, and which mutations
// deserve a generation is a product decision, not one to make on the way
// past. Explicit commits make the feature usable now without deciding it.
int run_profile_(int argc, char* argv[], EventStream& stream) {
    const std::string action = (argc >= 3) ? argv[2] : "list";
    auto& p = Config::paths();
    const auto envDir = p.subosDir;

    if (action == "list") {
        auto gens = profile::list_generations(envDir);
        auto current = profile::load_current(envDir);
        if (gens.empty()) {
            std::println("no generations yet — record one with "
                         "`xlings profile commit [reason]`");
            return 0;
        }
        for (const auto& gen : gens) {
            std::println("{}{:>4}  {}  {} package(s)  {}",
                         gen.number == current.number ? "*" : " ",
                         gen.number, gen.created, gen.packages.size(),
                         gen.reason);
        }
        return 0;
    }

    if (action == "commit") {
        const std::string reason = (argc >= 4) ? argv[3] : "manual";
        std::map<std::string, std::string> packages;
        for (const auto& [target, version] : Config::effective_workspace()) {
            packages[target] = version;
        }
        profile::commit(envDir, packages, reason);
        auto current = profile::load_current(envDir);
        std::println("[xlings:profile] recorded generation {} ({} package(s))",
                     current.number, packages.size());
        return 0;
    }

    if (action == "rollback") {
        if (argc < 4) {
            ui::print_usage("xlings profile rollback <generation>");
            return 1;
        }
        int target = 0;
        try { target = std::stoi(argv[3]); }
        catch (...) {
            log::error("not a generation number: {}", argv[3]);
            return 1;
        }
        auto packages = profile::rollback(envDir, target);
        if (!packages) {
            log::error("[xlings:profile] {}", packages.error());
            return 1;
        }
        // Apply through the same path `xlings use` takes. Recording the
        // selection is not rolling back: the previous implementation stopped
        // at the record and left every version where it was.
        int failed = 0;
        for (const auto& [name, version] : *packages) {
            if (xvm::cmd_use(name, version, stream) != 0) ++failed;
        }
        std::println("[xlings:profile] rolled back to generation {}", target);
        return failed == 0 ? 0 : 1;
    }

    ui::print_usage("xlings profile [list | commit [reason] | rollback <n>]");
    return action == "-h" || action == "--help" ? 0 : 1;
}

// Install the configured colour theme, saying so when it cannot be used.
//
// Every failure here is REPORTED. Falling back to the default in silence is
// the shape this whole round is about: "I configured a theme" and "my theme
// file has a typo in it" would produce identical output, and the user would
// conclude the setting does nothing.
void load_configured_theme_() {
    const auto path = Config::resolve_theme_path();
    if (path.empty()) return;              // built-in default, nothing to do

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        diag::emit({
            .level   = diag::Level::Warn,
            .code    = "theme.not_found",
            .summary = "the configured theme file is not there",
            .source  = Config::display_path(path),
            .facts   = { { "using instead", "the built-in default" } },
            .actions = { { "list what is available", "xlings config --theme list" },
                         { "or go back to default", "xlings config --theme default" } },
        });
        return;
    }

    std::string content;
    try {
        content = platform::read_file_to_string(path.string());
    } catch (...) {
        diag::emit({
            .level   = diag::Level::Warn,
            .code    = "theme.unreadable",
            .summary = "the configured theme file could not be read",
            .source  = Config::display_path(path),
            .facts   = { { "using instead", "the built-in default" } },
            .actions = { { "check permissions", "ls -l " + path.string() } },
        });
        return;
    }

    auto loaded = theme::load_from_json(content, theme::builtin_default());
    for (const auto& issue : loaded.issues) {
        diag::Diagnostic d {
            .level  = diag::Level::Warn,
            .source = Config::display_path(path),
        };
        switch (issue.kind) {
            case theme::LoadIssue::Kind::BadJson:
                d.code = "theme.bad_json";
                d.summary = "the theme file is not valid JSON";
                d.actions = { { "using instead", "the built-in default" } };
                break;
            case theme::LoadIssue::Kind::UnknownSlot:
                d.code = "theme.unknown_slot";
                d.summary = std::format("'{}' is not a colour slot", issue.detail);
                if (!issue.suggestion.empty()) {
                    d.actions = { { "did you mean", issue.suggestion } };
                } else {
                    d.actions = { { "valid slots",
                        "accent, alt, success, warn, error, text, muted, "
                        "border, surface" } };
                }
                break;
            default:
                d.code = "theme.bad_color";
                d.summary = std::format("'{}' is not a colour", issue.detail);
                d.actions = { { "expected", "#RRGGBB or #RGB" } };
                break;
        }
        diag::emit(d);
    }
    // Applied even with issues: the parts that parsed are still what the user
    // asked for, and dropping all of them because one line was wrong would be
    // a worse answer than reporting the line.
    theme::set_current(std::move(loaded.theme));
}

int run(int argc, char* argv[]) {
    if (argc == 2
        && std::string_view{argv[1]} == "--command-reference-json") {
        std::println("{}", spec::reference_json().dump());
        return 0;
    }
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
            // Wrapped here, not at the call sites. An ErrorEvent's message is
            // composed by whichever command raised it, and some of them carry
            // a list of candidates or a full command line -- the width
            // contract has to be a property of the renderer, or every command
            // re-derives it and one of them gets it wrong. log:: itself
            // cannot do this: core does not depend on ui.
            // Wrapped into ONE message with embedded newlines, not one
            // `log::error` per wrapped line.
            //
            // The per-line form turned a single sentence into several
            // independent-looking errors: a message wrapping at 100 columns
            // produced `[error] package 'x' not found in the synced index
            // (xim@...,` followed by `[error] dsh@..., +3 more)` -- the second
            // half of a clause, wearing its own severity marker. `log::` already
            // indents continuation lines to the tag width; it just had no
            // callers using that.
            const auto wrapped = [](std::string_view text,
                                    std::string_view indent) {
                const int prefix = 8  // "[error] "
                    + ui::layout::display_width(indent);
                const int width = std::max(1,
                    ui::layout::fit_width(
                        prefix + ui::layout::display_width(text)) - prefix);
                std::string out;
                for (auto& line : ui::layout::wrap_to_width(text, width)) {
                    if (!out.empty()) out += '\n';
                    out += indent;
                    out += line;
                }
                return out;
            };
            auto block = wrapped(er->message, "");
            if (!er->hint.empty()) {
                block += '\n';
                block += wrapped(er->hint, "  ");
            }
            log::error("{}", block);
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

    // `lang` had a complete pipeline -- CLI option, locked write into
    // ~/.xlings.json, project override, `xlings config` echo, a documented
    // schema field, an e2e assertion, and a bilingual table -- and NOTHING
    // called this. It was a setting users could set, xlings would echo, and
    // that changed no output at all.
    i18n::set_language(Config::lang());
    load_configured_theme_();

    // Build Capability Registry (for Agent/MCP use — CLI keeps direct dispatch)
    auto registry = capabilities::build_registry();

    // Scan for global flags (--verbose, -v, --quiet, -q, --agent) anywhere
    // in argv so they work regardless of position.
    bool agent_mode = false;
    std::string uiModeFlag;
    for (int i = 1; i < argc; ++i) {
        std::string_view a { argv[i] };
        if (a == "--verbose" || a == "-v") log::set_level(log::Level::Debug);
        else if (a == "--quiet" || a == "-q") log::set_level(log::Level::Error);
        else if (a == "--agent") agent_mode = true;
        else if (a.starts_with("--ui-mode=")) {
            uiModeFlag = std::string(a.substr(std::string_view{"--ui-mode="}.size()));
        }
        else if (a == "--ui-mode" && i + 1 < argc) {
            uiModeFlag = std::string(argv[i + 1]);
        }
    }

    // Resolve the frontend ONCE, here, from flag > config > auto -- the same
    // precedence `mirror` uses. Before this the answer was assembled
    // independently by `--agent` and by `interface`, so a third caller had to
    // assemble a third.
    {
        std::optional<ui::UiMode> preferred;
        auto origin = ui::PreferenceOrigin::Config;
        if (!uiModeFlag.empty() && uiModeFlag != "auto") {
            origin = ui::PreferenceOrigin::Flag;
            preferred = ui::parse_mode(uiModeFlag);
            if (!preferred) {
                diag::emit({
                    .code    = "cli.bad_ui_mode",
                    .summary = std::format("'{}' is not a UI mode", uiModeFlag),
                    .facts   = { { "valid", "cli, tui, auto" } },
                    .actions = { { "try", "xlings --ui-mode tui list" } },
                });
                return 2;
            }
        } else if (uiModeFlag.empty()) {
            preferred = ui::parse_mode(Config::ui_mode());
        }

        const auto env = ui::detect();
        const auto resolved = ui::resolve(preferred, origin, agent_mode, env);
        // Degrading in silence is how a user concludes a setting does nothing.
        if (!resolved.degradedReason.empty()) {
            diag::emit({
                .level   = diag::Level::Note,
                .code    = "ui.mode_degraded",
                .summary = std::format("using the {} frontend instead of {}",
                                       ui::to_string(resolved.mode),
                                       preferred ? ui::to_string(*preferred)
                                                 : "tui"),
                .facts   = { { "why", resolved.degradedReason } },
            });
        }
        // Interactive prompting is OPT-IN, not on by default.
        //
        // The obvious default is "on for terminals", and it is wrong. E2E-48
        // (non_interactive_contract_test.sh, N3) locks down the reason: a pty
        // is not evidence that a human is there. Agents and automation
        // routinely allocate one, so "is this a terminal" selects the
        // BLOCKING branch for exactly the callers that cannot answer. That
        // picker was removed on purpose in 2026.7.30 and `--pick`, its
        // explicit door, was removed with it.
        //
        // A stored preference is different in kind: `xlings config
        // --interactive true` is a promise that somebody will be at the
        // keyboard, made once, by the person who would be. That is the signal
        // a tty never was.
        ui::set_current(resolved.mode,
                        ui::capabilities_of(resolved.mode,
                                            Config::tui_interactive().value_or(false),
                                            agent_mode, env));
    }

    // Is there anyone to answer a question?
    //
    // One answer, resolved above: an explicit preference, gated by whether a
    // terminal is even attached and whether the caller announced itself as a
    // machine. Asking `ui::` rather than re-deriving it here is the point --
    // the version of this that tested `!agent_mode && stdout_is_terminal()`
    // locally was a second opinion, and second opinions are what this whole
    // change is about.
    stream.set_interactive(ui::current_capabilities().interactive);

    // --agent: replace TUI listener with plain-text renderer, disable colors.
    // Unlike interface mode, we do NOT set tui_mode(true) — log output should
    // still reach the terminal, just without ANSI decoration.
    if (agent_mode) {
        stream.set_enabled(tui_listener, false);
        log::enable_color(false);
        // One switch for every writer, not just log's own prefixes.
        ui::layout::set_plain(true);
        stream.on_event([&stream](const Event& e) {
            if (auto* d = std::get_if<DataEvent>(&e)) {
                agent::render_data_event(*d);
            }
            else if (auto* p = std::get_if<PromptEvent>(&e)) {
                // Reached only when something registered an auto-responder;
                // an unanswerable prompt is refused inside EventStream and
                // never emitted. Kept so a registered responder still works.
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
            || a == "--agent" || a.starts_with("--ui-mode")) continue;
        // `--ui-mode tui` (space form): drop the value too.
        if (i > 1 && std::string_view{argv[i - 1]} == "--ui-mode") continue;
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

        // agent — dispatched before the generic help interception so it keeps
        // handling its own -h as plain text. `xlings agent -h` has to print
        // the skill overview (the list of what is installed and readable),
        // and the boxed subcommand help rendered from the spec neither is
        // plain text nor names a single skill. The whole point of this command
        // family is that a machine reads its output.
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

            std::vector<std::string_view> commandPath;
            commandPath.push_back(cmd);
            if (fargc > 2) {
                const std::string_view nested{fargv[2]};
                if (!nested.starts_with('-')) {
                    auto candidate = commandPath;
                    candidate.push_back(nested);
                    if (spec::find(candidate)) commandPath = std::move(candidate);
                }
            }
            if (const auto* command = spec::find(commandPath)) {
                std::vector<A> arguments;
                std::vector<O> options;
                std::vector<O> children;
                for (const auto& argument : command->arguments) {
                    arguments.push_back({argument.name, argument.description,
                                         argument.required});
                }
                for (const auto& option : command->options) {
                    options.push_back({option.syntax, option.description});
                }
                for (const auto& child : command->children) {
                    children.push_back({child.name, child.description});
                }
                ui::print_subcommand_help(command->name, command->description,
                                          arguments, options, children);
                return 0;
            }

        }

        // index — positional dispatch like subos/self, so it validates the
        // same way and never reaches the cmdline builder's package semantics.
        if (cmd == "index") {
            const std::array<std::string_view, 1> path{cmd};
            if (const auto* command = spec::find(path)) {
                std::vector<std::string_view> manualArgs;
                for (int i = 2; i < fargc; ++i) manualArgs.emplace_back(fargv[i]);
                if (auto validated = spec::validate_manual_argv(*command, manualArgs);
                    !validated) {
                    log::error("{}", validated.error().message);
                    return 2;
                }
            }
            std::string sub = fargc > 2 ? fargv[2] : "list";
            if (sub == "ls") sub = "list";
            if (sub == "list") {
                std::string filter;
                bool asJson = false;
                for (int i = 3; i < fargc; ++i) {
                    std::string_view a{fargv[i]};
                    if (a == "--json") asJson = true;
                    else if (!a.starts_with('-')) filter = std::string(a);
                }
                return xim::cmd_index_list(filter, asJson, stream);
            }
            if (sub == "use") {
                return xim::cmd_index_use(fargc > 3 ? fargv[3] : "",
                                          fargc > 4 ? fargv[4] : "", stream);
            }
            log::error("unknown subcommand for `xlings index`: {}", sub);
            return 2;
        }

        if (cmd == "subos" || cmd == "self" || cmd == "profile") {
            const std::array<std::string_view, 1> path{cmd};
            if (const auto* command = spec::find(path)) {
                std::vector<std::string_view> manualArgs;
                for (int i = 2; i < fargc; ++i) manualArgs.emplace_back(fargv[i]);
                if (auto validated = spec::validate_manual_argv(
                        *command, manualArgs); !validated) {
                    log::error("{}", validated.error().message);
                    return 2;
                }
            }
        }

        // Detect unknown commands early — show TUI error + help
        {
            const bool known = std::ranges::any_of(spec::root().children,
                [&](const auto& command) {
                    return command.name == cmd
                        || std::ranges::find(command.aliases, cmd)
                            != command.aliases.end();
                });
            if (!known && !cmd.starts_with("-")) {
                log::error("unknown command: {}", cmd);
                return 1;
            }
        }

        if (cmd == "subos") return subos::run(fargc, fargv.data(), stream);
        if (cmd == "self") return xself::run(fargc, fargv.data(), stream);
        if (cmd == "profile") return run_profile_(fargc, fargv.data(), stream);
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
        // Declared here as well as scanned by hand above: the scan runs before
        // the parser exists (the frontend has to be resolved before anything
        // prints), and the parser rejects options it has never heard of.
        .option("ui-mode").takes_value().value_name("MODE")
            .help("Frontend for this run (cli/tui/auto)").global()

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
        // `-g` mirrors install's. Without it, a package installed with
        // `install -g` from inside a project could not be removed from inside
        // that project at all: remove resolved the project subos, found the
        // package registered in the home's, and there was no way to say
        // otherwise. Scope has to be expressible on both halves of a
        // reversible pair, or the pair is not reversible.
        .subcommand("remove")
            .description("Remove a package")
            .option(cmdline::Option("global").short_name('g').help("Act on the global scope (not the project-local subos)"))
            .option(cmdline::Option("force").help("Remove even if installed packages depend on it"))
            .arg("package").required().help("Package to remove (name or name@ver)")
            .arg("version").help("Optional version (alternative to name@ver form)")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                if (args.is_flag_set("global")) {
                    Config::set_force_global_scope(true);
                }
                std::string target;
                if (!parse_target_spec_(args, target)) return 1;
                bool yes = args.is_flag_set("yes");
                // Separate from `yes`, deliberately. `-y` says "do not ask me",
                // which is a statement about prompting; breaking a dependency
                // link is a different decision and a scripted `remove -y`
                // should still be stopped by it.
                bool force = args.is_flag_set("force");
                return xim::cmd_remove(target, yes, stream, force);
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
            .option(cmdline::Option("all-versions").help("Show every available version"))
            .arg("package").required().help("Package name (or name@ver)")
            .arg("version").help("Optional version (alternative to name@ver form)")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                std::string target;
                if (!parse_target_spec_(args, target)) return 1;
                return xim::cmd_info(target, stream,
                                     args.is_flag_set("all-versions"));
            }))

        // why — the resolution record, read back
        .subcommand("why")
            .description("Show why a dependency resolved to the version it did")
            .arg("package").required().help("Installed package name")
            .arg("dep").help("Optional dependency name to filter by")
            .action(wrap_rc([&stream](const cmdline::ParsedArgs& args) -> int {
                apply_global_opts_(args);
                return xim::cmd_why(args.value("package").value_or(""),
                                    args.value("dep").value_or(""), stream);
            }))

        // use — accepts both `<name> <ver>` (legacy form) and `<name>@<ver>`
        // (one-shot form, matching install/remove). Bare `<name>` switches
        // when exactly one version is installed and lists them (exit 0) when
        // several are, rather than blocking on a picker — see
        // xvm::cmd_use_by_name. `--all` widens the candidate set to the
        // global view (every version every subos has ever installed).
        .subcommand("use")
            .description("Switch tool version")
            .option(cmdline::Option("all").short_name('a').help("Show versions across all subos (default: current subos only)"))
            .option(cmdline::Option("strict").help("Refuse the switch if any program of the current release has no version in the new one"))
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
                bool strict = args.is_flag_set("strict");
                auto first = std::string(args.positional(0));
                if (n == 1) {
                    auto at = first.find('@');
                    if (at == std::string::npos)
                        return xvm::cmd_use_by_name(first, stream, show_all,
                                                    strict);
                    return xvm::cmd_use(first.substr(0, at),
                                        first.substr(at + 1), stream, strict);
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
                return xvm::cmd_use(first, std::string(args.positional(1)),
                                    stream, strict);
            }))

        // config
        .subcommand("config")
            .description("Show or modify xlings configuration")
            .option(cmdline::Option("lang").takes_value().value_name("LANG").help("Set language (en/zh)"))
            .option(cmdline::Option("mirror").takes_value().value_name("MIRROR").help("Set mirror (GLOBAL/CN)"))
            .option(cmdline::Option("ui-mode").takes_value().value_name("MODE").help("Set UI mode (cli/tui/auto)"))
            .option(cmdline::Option("theme").takes_value().value_name("THEME").help("Set colour theme (name or path)"))
            .option(cmdline::Option("interactive").takes_value().value_name("BOOL").help("Inline prompts in tui mode (true/false)"))
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

}
