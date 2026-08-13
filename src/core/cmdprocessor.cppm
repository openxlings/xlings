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

    int run(int argc, char* argv[]);

    int print_help() const;

private:
    std::vector<CommandInfo> commands_;
};

export CommandProcessor create_processor();

} // namespace xlings::cmdprocessor
