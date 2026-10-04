module xlings.core.destructive_log;

import std;
import xlings.core.config;
import xlings.observe;

namespace xlings::destructive_log {

void set_command(std::string command) {
    observe::destructive::set_identity(std::string(Info::VERSION), std::move(command));
}

std::filesystem::path log_path() {
    return observe::destructive::log_path(Config::paths().homeDir);
}

void record(const Entry& entry) noexcept {
    try {
        observe::destructive::record(Config::paths().homeDir, entry);
    } catch (...) {
        // See the module comment: a lost line, never a failed operation.
    }
}

}  // namespace xlings::destructive_log
