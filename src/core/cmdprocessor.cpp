module;
#include <cstdio>

module xlings.core.cmdprocessor;

import std;
import xlings.core.log;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.subos;
import xlings.platform;
import xlings.runtime;
import xlings.core.xself;
import mcpplibs.xpkg.executor;

namespace xlings::cmdprocessor {

// Resolve the xmake project directory that contains xim task definition.
// The xim task is defined in a xmake.lua that lives next to an xim/ directory
// (release/installed layout).  The source tree keeps xim code under core/xim/
// which is NOT a valid -P target; in that case we fall through to the default
// installed home (~/.xlings) which always has the correct layout.
std::filesystem::path find_xim_project_dir() {
    namespace fs = std::filesystem;

    auto is_xim_project = [](const fs::path& dir) {
        return fs::exists(dir / "xim") && fs::exists(dir / "xmake.lua");
    };

    // 1. Prefer layout next to the running binary (multi-version / release package)
    auto exePath = platform::get_executable_path();
    if (!exePath.empty()) {
        auto candidate = exePath.parent_path().parent_path();
        if (is_xim_project(candidate)) {
            return candidate;
        }
    }

    // 2. $XLINGS_HOME (installed)
    auto homeDir = Config::paths().homeDir;
    if (is_xim_project(homeDir)) {
        return homeDir;
    }

    // 3. Default fallback (~/.xlings) when homeDir is the source tree
    auto defaultHome = std::filesystem::path(platform::get_home_dir()) / ".xlings";
    if (is_xim_project(defaultHome)) {
        return defaultHome;
    }

    return homeDir;
}

int xim_exec(const std::string& flags, int argc, char* argv[], int startIdx = 2) {
    auto projectDir = find_xim_project_dir();
    std::string cmd = "xmake xim -P \"" + projectDir.string() + "\"";
    if (!flags.empty()) {
        cmd += " " + flags;
    }
    cmd += " --";
    for (int i = startIdx; i < argc; ++i) {
        cmd += " ";
        cmd += argv[i];
    }
    return platform::exec(cmd);
}

int xvm_exec(const std::string& subcommand, int argc, char* argv[], int startIdx = 2) {
    std::string cmd = "xvm " + subcommand;
    for (int i = startIdx; i < argc; ++i) {
        cmd += " ";
        cmd += argv[i];
    }
    return platform::exec(cmd);
}

std::filesystem::path find_project_xlings_json() {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto cwd = fs::current_path(ec);
    if (ec) return {};
    auto homeDir = Config::paths().homeDir;

    fs::path cur = cwd;
    while (!cur.empty()) {
        auto cfg = cur / ".xlings.json";
        if (fs::exists(cfg, ec) && fs::is_regular_file(cfg, ec)) {
            auto curNorm = fs::weakly_canonical(cur, ec);
            if (!ec) {
                auto homeNorm = fs::weakly_canonical(homeDir, ec);
                if (!ec && curNorm == homeNorm) {
                    // Skip global config at $XLINGS_HOME/.xlings.json
                } else {
                    return cfg;
                }
            } else {
                return cfg;
            }
        }

        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

std::filesystem::path find_project_legacy_xlings_lua() {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto cwd = fs::current_path(ec);
    if (ec) return {};
    auto homeDir = Config::paths().homeDir;

    fs::path cur = cwd;
    while (!cur.empty()) {
        auto cfg = cur / "config.xlings";
        if (fs::exists(cfg, ec) && fs::is_regular_file(cfg, ec)) {
            auto curNorm = fs::weakly_canonical(cur, ec);
            if (!ec) {
                auto homeNorm = fs::weakly_canonical(homeDir, ec);
                if (!ec && curNorm == homeNorm) {
                    // Skip global config under $XLINGS_HOME
                } else {
                    return cfg;
                }
            } else {
                return cfg;
            }
        }

        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

int install_targets_from_list(const std::vector<std::string>& targets) {
    auto& p = Config::paths();
    int rc = 0;
    for (const auto& target : targets) {
        if (target.empty()) continue;
        std::string cmd = std::format(
            "xmake xim -P \"{}\" -- {} -y",
            p.homeDir.string(),
            target
        );
        auto dep_rc = platform::exec(cmd);
        if (dep_rc != 0) {
            rc = dep_rc;
            break;
        }
    }
    return rc;
}

int install_from_legacy_config_xlings_via_xim(const std::filesystem::path& cfg) {
    auto projectDir = find_xim_project_dir();
    std::string cmd = "xmake xim -P \"" + projectDir.string() + "\" -- --install-config-xlings \"" + cfg.string() + "\" -y";
    return platform::exec(cmd);
}

int install_from_project_config() {
    auto cfg = find_project_xlings_json();
    if (!cfg.empty()) {
        nlohmann::json json;
        try {
            auto content = platform::read_file_to_string(cfg.string());
            json = nlohmann::json::parse(content, nullptr, false);
        } catch (...) {
            json = nlohmann::json::object();
        }
        if (json.is_discarded() || !json.is_object()) {
            log::error("Invalid JSON in {}", cfg.string());
            return 1;
        }
        if (!json.contains("deps") || !json["deps"].is_array()) {
            log::error("Missing or invalid `deps` array in {}", cfg.string());
            return 1;
        }

        std::vector<std::string> targets;
        for (const auto& dep : json["deps"]) {
            if (!dep.is_string()) continue;
            auto target = dep.get<std::string>();
            if (!target.empty()) targets.push_back(std::move(target));
        }
        return install_targets_from_list(targets);
    }

    // Forward-compat for legacy project config.xlings (Lua):
    // delegate loading/parsing/installing to xim (Lua runtime).
    auto legacyCfg = find_project_legacy_xlings_lua();
    if (!legacyCfg.empty()) {
        return install_from_legacy_config_xlings_via_xim(legacyCfg);
    }

    std::println("Tip: create <project>/.xlings.json with deps (preferred), or use config.xlings xim table, or run `xlings install <package>`");
    return 0;
}

CommandProcessor create_processor() {
    return CommandProcessor{}
        .add("install", "install package/tool",
            [](int argc, char* argv[]) {
                if (argc <= 2) {
                    return install_from_project_config();
                }
                return xim_exec("", argc, argv);
            },
            "xlings install [package[@version]]")
        .add("remove", "remove package/tool",
            [](int argc, char* argv[]) {
                return xim_exec("", argc, argv, 1);
            },
            "xlings remove <package>")
        .add("update", "update package/tool",
            [](int argc, char* argv[]) {
                return xim_exec("", argc, argv, 1);
            },
            "xlings update <package>")
        .add("search", "search for packages",
            [](int argc, char* argv[]) {
                return xim_exec("", argc, argv, 1);
            },
            "xlings search <keyword>")
        .add("use", "switch version of a tool",
            [](int argc, char* argv[]) {
                if (argc < 3) {
                    std::println("Usage: xlings use <target> [version]");
                    return 1;
                }
                if (argc >= 4) {
                    return xvm_exec("use", argc, argv);
                }
                return xvm_exec("list", argc, argv);
            },
            "xlings use <target> [version]")
        .add("info", "show tool info",
            [](int argc, char* argv[]) {
                if (argc < 3) {
                    std::println("Usage: xlings info <target>");
                    return 1;
                }
                return xvm_exec("info", argc, argv);
            },
            "xlings info <target>")
        .add("config", "show xlings configuration",
            [](int, char**) {
                Config::print_paths();
                return 0;
            })
        .add("subos", "manage sub-os environments",
            [](int argc, char* argv[]) {
                EventStream stream;
                return subos::run(argc, argv, stream);
            },
            "xlings subos <new|use|list|ls|remove|rm|info|i> [name]")
        .add("script", "run xpkg script",
            [](int argc, char* argv[]) {
                if (argc < 3) {
                    std::println("Usage: xlings script <script-file> [args...]");
                    return 1;
                }
                namespace fs = std::filesystem;
                fs::path scriptFile = argv[2];
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
                for (int i = 3; i < argc; ++i) {
                    ctx.args.emplace_back(argv[i]);
                }
                auto result = execResult->run_script(ctx);
                if (!result.success) {
                    log::error("script error: {}", result.error);
                    return 1;
                }
                return 0;
            },
            "xlings script <script-file> [args...]")
        .add("self", "self management (init/update/config/clean/migrate)",
            [](int argc, char* argv[]) {
                EventStream stream;
                return xself::run(argc, argv, stream);
            },
            "xlings self [init|update|config|clean [--dry-run]|migrate|help]");
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::cmdprocessor {

CommandProcessor& CommandProcessor::add(std::string name, std::string description, std::function<int(int argc, char* argv[])> func, std::string usage) {
    if (usage.empty()) usage = std::format("xlings {}", name);
    commands_.push_back({std::move(name), std::move(description),
                        std::move(usage), std::move(func)});
    return *this;
}

int CommandProcessor::run(int argc, char* argv[]) {
    if (argc <= 1) return print_help();

    std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd == "--version") {
        return print_help();
    }

    for (const auto& c : commands_) {
        if (c.name == cmd) return c.func(argc, argv);
    }

    log::error("Unknown command: {}", cmd);
    std::println(stdout, "Use 'xlings help' for usage information");
    return 1;
}

int CommandProcessor::print_help() const {
    std::println(stdout, "xlings version: {}\n", Info::VERSION);
    std::println(stdout, "Usage: $ xlings [command] [target] [options]\n");
    std::println(stdout, "Commands:");
    for (const auto& c : commands_) {
        std::println(stdout, "\t {:12}\t{}", c.name, c.description);
    }
    return 0;
}

} // namespace xlings::cmdprocessor
