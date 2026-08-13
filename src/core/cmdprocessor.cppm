export module xlings.core.cmdprocessor;

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

struct CommandInfo {
    std::string name;
    std::string description;
    std::string usage;
    std::function<int(int argc, char* argv[])> func;
};

class CommandProcessor {
public:
    CommandProcessor& add(std::string name, std::string description,
                          std::function<int(int argc, char* argv[])> func,
                          std::string usage = "") {
        if (usage.empty()) usage = std::format("xlings {}", name);
        commands_.push_back({std::move(name), std::move(description),
                            std::move(usage), std::move(func)});
        return *this;
    }

    int run(int argc, char* argv[]) {
        if (argc <= 1) return print_help();

        std::string cmd = argv[1];
        if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd == "--version") {
            return print_help();
        }

        for (const auto& c : commands_) {
            if (c.name == cmd) return c.func(argc, argv);
        }

        log::error("Unknown command: {}", cmd);
        std::println("Use 'xlings help' for usage information");
        return 1;
    }

    int print_help() const {
        std::println("xlings version: {}\n", Info::VERSION);
        std::println("Usage: $ xlings [command] [target] [options]\n");
        std::println("Commands:");
        for (const auto& c : commands_) {
            std::println("\t {:12}\t{}", c.name, c.description);
        }
        return 0;
    }

private:
    std::vector<CommandInfo> commands_;
};

// Resolve the xmake project directory that contains xim task definition.
// The xim task is defined in a xmake.lua that lives next to an xim/ directory
// (release/installed layout).  The source tree keeps xim code under core/xim/
// which is NOT a valid -P target; in that case we fall through to the default
// installed home (~/.xlings) which always has the correct layout.
std::filesystem::path find_xim_project_dir();

int xim_exec(const std::string& flags, int argc, char* argv[], int startIdx = 2);

int xvm_exec(const std::string& subcommand, int argc, char* argv[], int startIdx = 2);

std::filesystem::path find_project_xlings_json();

std::filesystem::path find_project_legacy_xlings_lua();

static int install_targets_from_list(const std::vector<std::string>& targets);

static int install_from_legacy_config_xlings_via_xim(const std::filesystem::path& cfg);

int install_from_project_config();

export CommandProcessor create_processor();

} // namespace xlings::cmdprocessor
