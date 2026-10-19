module luban.compat;

import std;
import xlings.platform;

namespace luban::compat {

OldWindows unregistered_luban_os(std::span<const int> registered) {
    namespace platform = xlings::platform;
    OldWindows out;
    out.looked = platform::can_list_processes();
    const int self = platform::current_pid();
    for (const auto& p : platform::own_processes()) {
        if (p.pid == self || std::ranges::find(registered, p.pid) != registered.end()) continue;
        const auto exe = p.exe.filename().string();
        const auto arg0 = p.argv.empty() ? std::string{} : std::filesystem::path(p.argv[0]).filename().string();
        // `__tray` and `__capabilities` are this version's own processes.
        const bool plumbing = p.argv.size() > 1 && p.argv[1].starts_with("__");
        if ((exe == "luban-os" || arg0 == "luban-os") && !plumbing) out.found.push_back({p.pid, p.exe});
    }
    return out;
}

}  // namespace luban::compat
