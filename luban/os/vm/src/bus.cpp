// The host's openbus router and a machine's place under it (vm.cppm;
// design 2026-10-22 §6.4, §6.5).
module luban.vm;

import std;
import xlings.platform;
import xbus.router;
import telemetry;

namespace luban::vm {

namespace {
namespace platform = xlings::platform;
}  // namespace

fs::path luban_binary(const fs::path& home) {
    if (const char* forced = std::getenv("LUBAN_BIN"); forced && *forced) return forced;
    const auto self = platform::get_executable_path();
    std::error_code ec;
    if (self.filename().string().starts_with("luban") && !self.filename().string().starts_with("luban-os")) return self;
    const auto exe = std::string(platform::exe_suffix);
    for (const auto& candidate : {self.parent_path() / ("luban" + exe), home / "bin" / ("luban" + exe)})
        if (fs::is_regular_file(candidate, ec)) return candidate;
    return self;
}

fs::path bus_socket(const fs::path& home) { return home / "run" / "openbus" / "router.sock"; }

std::expected<int, std::string> connect_bus(const fs::path& home) {
    const auto at = bus_socket(home);
    if (const int fd = platform::unix_connect_stream(at); fd >= 0) return fd;
    const auto log = home / "logs" / "openbus" / "router.out";
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    if (!platform::spawn_detached({luban_binary(home).string(), "__bus-router", home.string()}, log))
        return std::unexpected("the host's openbus router could not be started (" + log.string() + ")");
    for (int k = 0; k < 40; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (const int fd = platform::unix_connect_stream(at); fd >= 0) return fd;
    }
    return std::unexpected("the host's openbus router did not answer at " + at.string() + " (" + log.string() + ")");
}

int serve_bus(const fs::path& home) {
    telemetry::identify("luban-bus-router");
    xbus::Router r({.root = xbus::host_root(),
                    .listen = {xbus::Address{"unix", bus_socket(home).string()}},
                    .log = home / "logs" / "openbus" / "router.ndjson",
                    .idle_exit = std::chrono::seconds(600),
                    // A sandbox's programs reach it through their system's own
                    // socket (design 2026-10-24 §3.3: system_bus_socket).
                    .systems_dir = bus_socket(home).parent_path() / "systems"});
    // Another answers there already (two machines started at once): it is the one.
    if (auto l = r.listen(); !l) {
        std::println(std::cerr, "luban __bus-router: {}", l.error());
        return l.error().starts_with("a router already answers") ? 0 : 1;
    }
    std::atomic<bool> stop{false};
    r.run(stop);
    return 0;
}

std::string system_bus_root(std::string_view name) {
    std::string seg;
    for (const char c : name) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        seg += (std::isalnum(static_cast<unsigned char>(l)) || l == '-' || l == '_' || l == '.') ? l : '-';
    }
    if (seg.empty()) seg = "system";
    return xbus::host_root() + "/system/" + seg.substr(0, 64);
}

}  // namespace luban::vm
