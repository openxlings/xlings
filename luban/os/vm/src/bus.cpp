// The host's openbus router and a machine's place under it (vm.cppm;
// design 2026-10-22 §6.4, §6.5).
module luban.vm;

import std;
import xlings.platform;
import xbus.router;
import telemetry;
import xlings.libs.json;
import xlings.observe;
import luban.service;

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

std::string bus_actor() {
    if (const char* a = std::getenv("XLINGS_ACTOR"); a && *a) return a;
    return "program:luban";
}

bool desktop_control_allowed(const fs::path& home, std::string_view actor, std::string_view path, std::string_view op) {
    // The host's displays (a Luban OS window) are the person's at it.
    const auto host = xbus::host_root() + "/";
    const auto record = [&](std::string_view system, bool ok) {
        // An openas run of its kind (who, on whose behalf, what): the record.
        const auto log = home / "logs" / "openbus" / "desktop-control.ndjson";
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        std::ofstream(log, std::ios::app) << nlohmann::json{{"t", xlings::observe::utc_now()}, {"actor", actor}, {"system", system},
                                                            {"path", path}, {"op", op}, {"allowed", ok}}.dump() << "\n";
        return ok;
    };
    if (actor.starts_with("agent") && (path.starts_with(host + "device/display/") || path.starts_with(host + "device/input/")))
        return record("", false);
    // host/<id>/system/<name>/<what>: what drives the desktop.
    const auto prefix = host + "system/";
    if (!path.starts_with(prefix)) return true;
    const auto rest = path.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return true;
    const auto name = rest.substr(0, slash), what = rest.substr(slash + 1);
    const bool drives = what.starts_with("device/input/") || what.starts_with("device/display/") || what == "service/xdesk/control" ||
                        (what == "service/desktop" && !op.empty() && op != "status");
    if (!drives || !actor.starts_with("agent")) return true;
    return record(name, service::granted(home, name, "desktop-control"));
}

int serve_bus(const fs::path& home) {
    telemetry::identify("luban-bus-router");
    xbus::Router r({.root = xbus::host_root(),
                    .listen = {xbus::Address{"unix", bus_socket(home).string()}},
                    .log = home / "logs" / "openbus" / "router.ndjson",
                    // An agent drives a desktop with the system's grant (design
                    // 2026-10-24 §3.4); the supervisor's own ops it decides itself.
                    .allow = [home](const xbus::PeerInfo& peer, std::string_view op, std::string_view path) {
                        return (op != "call" && op != "open") || desktop_control_allowed(home, peer.actor, path);
                    },
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
