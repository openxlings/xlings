// The tray process, `luban-os __tray` (core.cppm; design 2026-10-19 §3): the
// status-area icon of a home, its menu and its notifications, with no
// display of its own. Windows connect to it; what is chosen at the icon goes
// to a window that suits it (`raise`), or starts one. It reads which systems
// run each second, and what the windows are doing as they say it. It ends
// when nothing needs it: no window, nothing running, the icon not to be kept.
module luban.os;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.observe;
import luban.service;
import luban.vm;
import luban.desktop.host;
import tray;
import :core;

namespace luban::os {

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

struct Client {
    platform::LocalStream stream;
    std::string buffer;
    Json hello = Json::object();
    Json reg = Json::object();          // attach, view, hidden
    Json job;                           // null: none
    bool said_hidden { false };
};

struct Tray {
    fs::path home = home_dir_();
    service::Context ctx{.home = home, .port = service::Port(service::xlings_path())};
    fs::path dir = tray_dir(home);
    std::unique_ptr<tray::Icon> icon;
    std::string error, backend;
    std::vector<std::unique_ptr<Client>> clients;
    std::string said;                   // what the icon shows (told only changes)
    bool told_background { false };     // "still here": once
    bool quitting { false };
    std::chrono::steady_clock::time_point last_read {}, idle_since = std::chrono::steady_clock::now();
    // Once a second when reading is cheap (a few files); where it asks a
    // carrier's guest (WSL 2), five times what the read took.
    // Windows say when something changed; this is the fallback for a system
    // started from the command line.
    std::chrono::milliseconds read_every { 5000 };
    std::vector<std::pair<std::string, bool>> running;   // name, a machine
    bool keep { false };                // the icon kept with nothing running (keep_in_tray)
    Client* asking { nullptr };         // the window a choice came from

    void log(std::string_view what) const {
        std::ofstream(home / "logs" / "luban-os-tray.log", std::ios::app)
            << std::format("{} {}\n", xlings::observe::utc_now(), what);
    }

    void tell(Client& c, const Json& m) { (void)c.stream.send_all(m.dump() + "\n", 500ms); }

    mutable std::string written;        // tray.json as it is on disk (written when it changes)
    void write_state() const {
        Json windows = Json::array();
        for (const auto& c : clients) windows.push_back({{"pid", c->hello.value("pid", 0)}, {"register", c->reg}});
        const Json j{{"pid", platform::current_pid()}, {"version", std::string(service::kVersion)}, {"error", error},
                     {"backend", backend}, {"said", said}, {"windows", windows}};
        auto text = j.dump(2);
        if (text == written) return;
        platform::write_file_atomic((dir / "tray.json").string(), text);
        written = std::move(text);
    }

    // Which systems run: read at most once a second (a few files).
    void read_running() {
        last_read = std::chrono::steady_clock::now();
        const auto took = [this] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_read);
        };
        keep = read_app_settings(home).keep_in_tray;
        running.clear();
        auto l = service::list(ctx);
        if (!l) return;
        for (const auto& e : *l) {
            const bool machine = vm::running(home, e.name).has_value();
            if (e.running || machine) running.emplace_back(e.name, machine);
        }
        read_every = std::max(std::chrono::milliseconds(5000), took() * 5);
    }

    // The icon: the systems that run, what the windows are doing, a menu of both.
    void show() {
        if (!icon) { write_state(); return; }
        const Json* busy = nullptr;
        bool failed = false;
        for (const auto& c : clients) {
            if (!c->job.is_object()) continue;
            if (!c->job.value("done", false) && !busy) busy = &c->job;
            if (c->job.value("done", false) && c->job.value("code", 0) != 0) failed = true;
        }
        const auto n = running.size();
        std::string body = n == 0 ? std::string(tr("luban.os.app.nothing_running")) : std::vformat(tr("luban.os.app.running"), std::make_format_args(n));
        if (busy) {
            const double p = busy->value("percent", -1.0);
            body += " · " + busy->value("title", std::string{}) + (p >= 0 ? std::format(" {:.0f} %", p * 100) : std::string{});
        }
        tray::Status s{.title = "Luban OS", .body = body, .icons = art::tray_images(static_cast<int>(n), busy != nullptr, failed),
                       .attention = failed};
        for (const auto& [name, machine] : running) {
            tray::MenuItem item{.id = "sys:" + name, .label = std::format("{}  ({})", name, machine ? tr("luban.os.app.machine") : tr("luban.os.app.sandbox"))};
            item.children.push_back({.id = "sys:" + name + ":open", .label = std::string(tr("luban.os.app.open"))});
            if (machine) {
                item.children.push_back({.id = "sys:" + name + ":console", .label = std::string(tr("luban.os.app.open_console"))});
                item.children.push_back({.id = "sys:" + name + ":off", .label = std::string(tr("luban.os.app.power_off"))});
            }
            item.children.push_back({.id = "sys:" + name + ":manage", .label = std::string(tr("luban.os.app.manage"))});
            s.menu.push_back(std::move(item));
        }
        if (!running.empty()) s.menu.push_back({.separator = true});
        s.menu.push_back({.id = "new", .label = std::string(tr("luban.os.app.new_system"))});
        s.menu.push_back({.id = "open", .label = std::string(tr("luban.os.app.open_luban_os"))});
        s.menu.push_back({.id = "settings", .label = std::string(tr("luban.os.app.settings"))});
        s.menu.push_back({.separator = true});
        s.menu.push_back({.id = "quit", .label = std::string(tr("luban.os.app.quit"))});
        std::string now = body + (failed ? "!" : "");
        for (const auto& m : s.menu) now += "|" + m.id;
        if (now != said) {
            said = now;
            icon->update(s);
        }
        write_state();
    }

    // A window for it: one that shows `system` (when given), else any, a
    // shown one before a hidden one.
    Client* window_for(std::string_view system = {}) {
        // A window that said it (a script's choice) is the one, when it suits.
        if (asking && (system.empty() || asking->reg.value("attach", std::string{}) == system)) return asking;
        Client* best = nullptr;
        for (auto& c : clients) {
            if (!system.empty() && c->reg.value("attach", std::string{}) != system) continue;
            if (!best || (best->reg.value("hidden", false) && !c->reg.value("hidden", false))) best = c.get();
        }
        return best;
    }

    void start_window(std::vector<std::string> args) {
        const auto app = service::luban_os_app(home);
        args.insert(args.begin(), (app.empty() ? platform::get_executable_path() : app).string());
        if (!platform::spawn_detached(args, home / "logs" / "luban-os.log")) log("could not start a window");
    }

    // What was chosen at the icon or a notification goes where the design's
    // table says (§3.3): the window that suits it, else a new one.
    void route(const std::string& what) {
        auto raise = [&](Client* c, Json view, std::vector<std::string> otherwise) {
            if (c) tell(*c, {{"raise", std::move(view)}});
            else start_window(std::move(otherwise));
        };
        if (what == "activate" || what == "open" || what == "notification:open")
            raise(window_for(), {{"view", "launcher"}, {"activate", what == "activate"}}, {});
        else if (what == "new") raise(window_for(), {{"view", "quick-start"}}, {"--view", "quick-start"});
        else if (what == "settings") raise(window_for(), {{"view", "settings"}}, {"--view", "settings"});
        else if (what == "quit") {
            // A window asks the person; with none, one is started to.
            if (auto* c = window_for()) tell(*c, {{"confirm-quit", Json::object()}});
            else start_window({"--view", "quit"});
        } else if (what.starts_with("notification:made:")) {
            const auto name = what.substr(18);
            auto* c = window_for(name);
            if (!c) c = window_for();
            raise(c, {{"view", "enter"}, {"system", name}}, {"--attach", name});
        } else if (what.starts_with("notification:")) {
            raise(window_for(), {{"view", "launcher"}}, {});
        } else if (what.starts_with("sys:")) {
            const auto rest = what.substr(4);
            const auto colon = rest.rfind(':');
            const auto name = rest.substr(0, colon), act = rest.substr(colon + 1);
            if (act == "open") raise(window_for(name), {{"view", "enter"}, {"system", name}}, {"--attach", name});
            else if (act == "console") raise(window_for(name), {{"view", "console"}, {"system", name}}, {"--attach", name, "--view", "console"});
            else if (act == "manage") raise(window_for(), {{"view", "manage"}, {"system", name}}, {"--view", "manage", "--system", name});
            else if (act == "off") std::thread([ctx = ctx, name] { (void)vm::stop(ctx, name); }).detach();
        }
    }

    void handle(Client& c, const Json& m) {
        if (m.contains("hello")) {
            c.hello = m["hello"];
            tell(c, {{"tray", {{"pid", platform::current_pid()}, {"error", error}, {"backend", backend},
                               {"version", std::string(service::kVersion)},
                               {"provides", Json::array({std::format("tray-protocol {}", kTrayProtocol)})}}}});
        } else if (m.contains("register")) {
            c.reg = m["register"];
            // The first window gone to the background: said once that the systems keep running.
            if (c.reg.value("hidden", false) && !std::exchange(told_background, true) && icon)
                icon->notify("Luban OS", std::string(tr("luban.os.app.still_here_your_systems_keep")), "open");
        } else if (m.contains("job")) {
            c.job = m["job"];
        } else if (m.contains("notify") && icon) {
            const auto& n = m["notify"];
            icon->notify(n.value("title", std::string{}), n.value("body", std::string{}), n.value("action", std::string{"open"}));
        } else if (m.contains("changed")) {
            read_running();
        } else if (m.contains("choose")) {
            // As if chosen at the icon (a window's script, tests).
            asking = &c;
            route(m.value("choose", std::string{}));
            asking = nullptr;
        } else if (m.contains("quit-all")) {
            for (auto& other : clients) tell(*other, {{"close", Json::object()}});
            quitting = true;
        }
        show();
    }

    // What a new install of the app is: a different program, or the same one
    // changed -- the tray becomes it (design 2026-10-19 §3.5).
    std::pair<fs::path, fs::file_time_type> app_now() const {
        std::error_code ec;
        const auto app = service::luban_os_app(home);
        return {app, app.empty() ? fs::file_time_type{} : fs::last_write_time(app, ec)};
    }
};

}  // namespace

int run_tray(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    Tray t;
    std::error_code ec;
    fs::create_directories(t.dir, ec);
    fs::create_directories(t.home / "logs", ec);
    // One per home: whoever holds the lock is the tray; the rest leave now.
    platform::FileLock lock;
    std::string why;
    if (!lock.try_acquire(t.dir / "tray.lock", why)) return 0;
    auto listener = platform::LocalListener::listen(t.dir / "tray.sock");
    if (!listener) { t.log("cannot listen: " + listener.error()); return 1; }
    host::open_events_only();
    set_language(read_app_settings(t.home).language);
    // The icon's events wake the loop through a line of its own to the
    // listener (G21: it woke ten times a second to look).
    auto waker = platform::LocalStream::connect(t.dir / "tray.sock", 1s);
    platform::LocalStream woken;
    if (waker)
        if (auto s = listener->accept(1s); s && *s) woken = std::move(**s);
    std::mutex waker_m;
    if (auto icon = tray::Icon::create("luban-os", "Luban OS")) {
        t.icon = std::move(*icon);
        t.backend = std::string(t.icon->backend());
        if (waker && woken.is_open())
            t.icon->on_wake([&] {
                std::lock_guard l(waker_m);
                (void)waker->send_all("w", 100ms);
            });
    } else {
        // No status area here: said, and the tray stays for the windows to agree among.
        t.error = icon.error();
    }
    t.log(std::format("tray {} started{}", service::kVersion, t.error.empty() ? std::string{} : " without an icon: " + t.error));
    const auto started_app = t.app_now();
    auto last_app_check = std::chrono::steady_clock::now();
    t.read_running();
    t.show();
    while (true) {
        std::vector<std::intptr_t> handles{listener->native(), woken.is_open() ? woken.native() : listener->native()};
        for (const auto& c : t.clients) handles.push_back(c->stream.native());
        // macOS's menu bar is this thread's to pump; elsewhere the icon wakes the loop.
        const bool pumped = platform::is_macos || !woken.is_open();
        const auto until_read = std::chrono::duration_cast<std::chrono::milliseconds>(t.read_every - (std::chrono::steady_clock::now() - t.last_read));
        const auto ready = platform::wait_readable(handles, pumped ? 100ms : std::clamp(until_read, 10ms, 1000ms));
        for (const auto i : ready) {
            if (i == 1) {
                std::array<char, 256> drain{};
                (void)woken.poll_receive(drain, 0ms);
                continue;
            }
            if (i == 0) {
                if (auto s = listener->accept(0ms); s && *s) t.clients.push_back(std::make_unique<Client>(Client{.stream = std::move(**s)}));
                continue;
            }
            auto& c = *t.clients[i - 2];
            std::array<char, 4096> chunk{};
            auto n = c.stream.poll_receive(chunk, 0ms);
            if (!n || (*n && **n == 0)) { c.stream.close(); continue; }
            if (!*n) continue;
            c.buffer.append(chunk.data(), **n);
            for (auto nl = c.buffer.find('\n'); nl != std::string::npos; nl = c.buffer.find('\n')) {
                auto j = Json::parse(c.buffer.substr(0, nl), nullptr, false);
                c.buffer.erase(0, nl + 1);
                if (j.is_object()) t.handle(c, j);
            }
        }
        const auto before = t.clients.size();
        std::erase_if(t.clients, [](const auto& c) { return !c->stream.is_open(); });
        if (t.clients.size() != before) t.show();
        host::wait_events(0);
        if (t.icon) {
            t.icon->pump();
            for (const auto& chosen : t.icon->take()) t.route(chosen);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - t.last_read >= t.read_every) {
            t.read_running();
            t.show();
        }
        // The home gone (a test's, removed): nothing left to show.
        if (!fs::exists(t.dir, ec)) break;
        if (t.quitting && t.clients.empty()) break;
        // Nothing needs it: no window, nothing running, the icon not to be kept.
        const bool needed = !t.clients.empty() || !t.running.empty() || t.keep;
        if (needed) t.idle_since = now;
        else if (now - t.idle_since > 3s) break;
        // A newer app installed: the tray becomes it; the windows reconnect.
        if (now - last_app_check > 2s) {
            last_app_check = now;
            if (const auto app = t.app_now(); !app.first.empty() && app != started_app) {
                t.log("a new Luban OS: the tray restarts as it");
                listener->close();
                lock.release();
                (void)platform::spawn_detached({app.first.string(), "__tray"}, t.home / "logs" / "luban-os-tray.log");
                break;
            }
        }
    }
    if (t.icon) t.icon->on_wake({});
    listener->close();
    host::close_events_only();
    return 0;
}

}  // namespace luban::os
