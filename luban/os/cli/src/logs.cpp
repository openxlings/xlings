// `luban logs <name> [--app ID] [--since 10m] [--bundle FILE.tar] [--with-screenshot]`
// (design 2026-10-24 §6.3): everything a system's desktop and machine said,
// gathered without anybody copying it out by hand -- its desktop's session
// and supervisor, the programs it started (through the bus: their last
// mebibyte, ended or not), the machine's console and supervisor, the Luban OS
// app's log. Bundled with its status (and, asked, a picture of its desktop).
module luban.cli;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.observe;
import luban.service;
import luban.service.archive;
import luban.vm;
import openbus;
import xbus.node;
import openhal.display;

namespace luban::cli {

namespace {

namespace svc = luban::service;
namespace vm = luban::vm;
namespace fs = std::filesystem;
using Json = nlohmann::json;

struct Piece {
    std::string name;      // in the bundle
    std::string what;      // for a person
    std::string bytes;
};

std::string tail_lines_(std::string_view text, int n) {
    std::size_t at = text.size();
    while (n-- > 0 && at > 0) {
        const auto nl = text.rfind('\n', at > 1 ? at - 2 : 0);
        if (nl == std::string_view::npos) { at = 0; break; }
        at = nl + 1;
    }
    return std::string(text.substr(at));
}

// "10m", "2h", "30s", "1d": how far back a file's last change may be.
std::optional<std::chrono::seconds> since_(std::string_view s) {
    if (s.empty()) return std::nullopt;
    long long n = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || n <= 0) return std::nullopt;
    const std::string_view unit(end, s.data() + s.size());
    const long long k = unit == "s" ? 1 : unit == "m" || unit.empty() ? 60 : unit == "h" ? 3600 : unit == "d" ? 86400 : 0;
    if (!k) return std::nullopt;
    return std::chrono::seconds(n * k);
}

}  // namespace

int logs(const svc::Context& c, std::string_view name, std::optional<std::string> app, std::optional<std::string> since_text,
         std::optional<std::string> bundle, bool screenshot, bool json) {
    const auto since = since_text ? since_(*since_text) : std::nullopt;
    if (since_text && !since) {
        if (c.sink) c.sink(svc::error(std::format("--since {}: a time back, as 30s, 10m, 2h, 1d", *since_text)));
        return 2;
    }
    std::vector<Piece> pieces;
    const auto recent = [&](const fs::path& f) {
        std::error_code ec;
        if (!fs::is_regular_file(f, ec)) return false;
        if (!since) return true;
        const auto t = fs::last_write_time(f, ec);
        return !ec && fs::file_time_type::clock::now() - t <= *since;
    };
    const auto file_piece = [&](const fs::path& f, std::string bundle_name, std::string what) {
        if (!recent(f)) return;
        pieces.push_back({std::move(bundle_name), std::move(what), xlings::platform::read_file_to_string(f.string())});
    };
    if (!app) {
        // The desktop's session and supervisor; the machine's; the window's.
        const auto desk = c.home / "logs" / "desktops" / std::string(name);
        file_piece(desk / "supervisor.log", "desktop/supervisor.log", "its desktop's supervisor");
        file_piece(desk / "session.log", "desktop/session.log", "its desktop's session (the compositor's own words)");
        const auto machine = vm::log_dir(c.home, name);
        file_piece(machine / "console.log", "machine/console.log", "its machine's console");
        file_piece(machine / "supervisor.log", "machine/supervisor.log", "its machine's supervisor");
        file_piece(c.home / "logs" / "luban-os.log", "luban-os.log", "the Luban OS app");
    }
    // The programs its desktop started, through the bus (a machine's too).
    Json listed = Json::array();
    std::shared_ptr<xbus::Node> node;
    if (auto fd = vm::connect_bus(c.home)) {
        std::vector<int> close{*fd};
        xlings::platform::close_fds(close);
        if (auto n = xbus::Node::connect({.name = "luban", .actor = "program:luban",
                                          .router = xbus::Address{"unix", vm::bus_socket(c.home).string()}, .telemetry = false}))
            node = *n;
    }
    const auto apps_path = vm::system_bus_root(name) + "/service/xdesk/apps";
    if (node)
        if (auto got = node->call(apps_path, {}, std::chrono::seconds(5)); got && !got->empty()) {
            listed = Json::parse(got->front(), nullptr, false);
            if (!listed.is_array()) listed = Json::array();
            for (const auto& a : listed) {
                const auto id = a.value("id", std::string{});
                if (app && id != *app) continue;
                const int pid = a.value("pid", -1);
                auto tail = node->call(apps_path, Json{{"op", "tail"}, {"pid", pid}}.dump(), std::chrono::seconds(10));
                if (!tail) continue;
                std::string bytes;
                for (const auto& part : *tail) bytes += part;
                const auto state = a.value("running", false) ? std::string("running")
                                                             : std::format("ended, {}", a.contains("code") && !a["code"].is_null()
                                                                                            ? std::to_string(a["code"].get<int>()) : std::string("?"));
                pieces.push_back({std::format("apps/{}-{}.log", id, pid), std::format("{} (pid {}, {})", a.value("name", id), pid, state), bytes});
            }
        }
    if (app && pieces.empty()) {
        if (c.sink) c.sink(svc::error(std::format("no program '{}' in {}'s desktop (what it started: luban logs {})", *app, name, name)));
        return 1;
    }
    if (bundle) {
        std::vector<std::pair<std::string, std::string>> files;
        for (const auto& p : pieces) files.emplace_back(p.name, p.bytes);
        // Its status, as luban status --json says it; and its desktop's, when it runs.
        Json status{{"name", name}, {"gathered", xlings::observe::utc_now()}, {"apps", listed}};
        if (auto d = vm::desktop(c.home, name, {{"op", "status"}})) status["desktop"] = *d;
        files.emplace_back("status.json", status.dump(2));
        if (screenshot && node)
            if (auto d = vm::desktop(c.home, name, {{"op", "status"}}))
                if (auto png = node->call(openhal::display::control_path(d->value("virtual", std::string{})), R"({"op": "capture"})",
                                          std::chrono::seconds(20))) {
                    std::string bytes;
                    for (const auto& part : *png) bytes += part;
                    files.emplace_back("screenshot.png", bytes);
                }
        auto written = svc::archive::write_files(*bundle, files);
        if (!written) {
            if (c.sink) c.sink(svc::error(written.error()));
            return 1;
        }
        std::println("✓ {} ({} files, {} bytes)", *bundle, files.size(), *written);
        std::println("  it may hold what was on the screen and the pages that were open: look before you share it");
        return 0;
    }
    if (json) {
        Json out = Json::array();
        for (const auto& p : pieces) out.push_back({{"name", p.name}, {"what", p.what}, {"bytes", p.bytes.size()}, {"tail", tail_lines_(p.bytes, 40)}});
        std::println("{}", out.dump(2));
        return 0;
    }
    if (pieces.empty()) {
        std::println("{} has said nothing{}", name, since ? " in that time" : "");
        return 0;
    }
    for (const auto& p : pieces) {
        std::println("── {}  ({}, {} bytes)", p.what, p.name, p.bytes.size());
        const auto t = tail_lines_(p.bytes, 40);
        std::print("{}", t);
        if (!t.empty() && !t.ends_with('\n')) std::println("");
    }
    std::println("── all of it: luban logs {} --bundle {}-logs.tar", name, name);
    return 0;
}

}  // namespace luban::cli
