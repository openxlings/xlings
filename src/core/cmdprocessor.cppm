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
                          std::string usage = "");

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

export CommandProcessor create_processor();

} // namespace xlings::cmdprocessor
