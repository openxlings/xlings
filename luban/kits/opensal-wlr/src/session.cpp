module opensal.wlr;

import std;
import opensal;
import xdesk.os;
import opensal.wlr.wayland;

namespace opensal::wlr {

namespace os = xdesk::os;

namespace {

std::string_view trim_(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::optional<Session> parse_session(std::string_view text, std::string id) {
    Session s;
    s.id = std::move(id);
    bool in_entry = false, hidden = false, shell_less = false;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto line = trim_(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') { in_entry = line == "[Desktop Entry]"; continue; }
        if (!in_entry) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const auto key = trim_(line.substr(0, eq)), value = trim_(line.substr(eq + 1));
        if (key == "Name") s.name = value;
        else if (key == "Exec") s.exec = value;
        else if (key == "X-Opensal-Interfaces") s.interfaces = parse_interfaces(value);
        else if (key == "X-Opensal-Native") s.native = value == "true";
        else if (key == "X-Luban-Shell") shell_less = value == "none";
        else if (key == "X-Luban-App-Exec") s.app_exec = value;
        else if ((key == "Hidden" || key == "NoDisplay") && value == "true") hidden = true;
    }
    if (s.exec.empty() || hidden) return std::nullopt;
    if (shell_less) s.interfaces = s.interfaces | opensal::Interface::Shell;
    if (s.name.empty()) s.name = s.id;
    return s;
}

opensal::Interfaces parse_interfaces(std::string_view names) {
    static constexpr std::array<std::pair<std::string_view, opensal::Interface>, 10> known{{
        {"surface", opensal::Interface::Surface}, {"pointer", opensal::Interface::Pointer},
        {"keyboard", opensal::Interface::Keyboard}, {"mode", opensal::Interface::Mode},
        {"density", opensal::Interface::Density}, {"capture", opensal::Interface::Capture},
        {"cursor", opensal::Interface::Cursor}, {"text", opensal::Interface::Text},
        {"clipboard", opensal::Interface::Clipboard}, {"shell", opensal::Interface::Shell}}};
    opensal::Interfaces out = 0;
    std::size_t at = 0;
    while (at < names.size()) {
        auto end = names.find_first_of(" ;,", at);
        if (end == std::string_view::npos) end = names.size();
        const auto word = names.substr(at, end - at);
        for (const auto& [n, i] : known)
            if (word == n) out |= static_cast<opensal::Interfaces>(i);
        at = end + 1;
    }
    return out;
}

namespace {

// <dir>/<sub> for each of $XDG_DATA_DIRS, then /usr/local/share and /usr/share
// (`always`; else only when it names none, as the base directory specification says).
void data_dirs_(const std::map<std::string, std::string>& env, std::string_view sub, std::vector<fs::path>& out, bool always) {
    const auto before = out.size();
    auto add = [&](fs::path p) {
        p /= sub;
        if (std::ranges::find(out, p) == out.end()) out.push_back(std::move(p));
    };
    if (auto it = env.find("XDG_DATA_DIRS"); it != env.end()) {
        std::string_view dirs = it->second;
        while (!dirs.empty()) {
            const auto c = dirs.find(':');
            if (const auto d = dirs.substr(0, c); !d.empty() && d.front() == '/') add(fs::path(d));
            if (c == std::string_view::npos) break;
            dirs.remove_prefix(c + 1);
        }
    }
    if (!always && out.size() > before) return;
    add("/usr/local/share");
    add("/usr/share");
}

// Each .desktop file in `dirs` once (the first directory's wins), parsed.
template <typename T, typename Parse>
std::vector<T> entries_(const std::vector<fs::path>& dirs, Parse parse) {
    std::vector<T> out;
    std::set<std::string> seen;
    for (const auto& dir : dirs) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".desktop") files.push_back(it->path());
        std::ranges::sort(files);
        for (const auto& f : files) {
            const auto id = f.stem().string();
            if (!seen.insert(id).second) continue;
            std::ifstream in(f);
            std::string text((std::istreambuf_iterator<char>(in)), {});
            if (auto e = parse(text, id, f)) out.push_back(std::move(*e));
        }
    }
    return out;
}

// A [Desktop Entry]'s keys (unlocalized), each as it is written.
std::map<std::string, std::string, std::less<>> entry_keys_(std::string_view text) {
    std::map<std::string, std::string, std::less<>> out;
    bool in_entry = false;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto line = trim_(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') { in_entry = line == "[Desktop Entry]"; continue; }
        const auto eq = line.find('=');
        if (!in_entry || eq == std::string_view::npos) continue;
        out.try_emplace(std::string(trim_(line.substr(0, eq))), trim_(line.substr(eq + 1)));
    }
    return out;
}

}  // namespace

std::vector<fs::path> session_dirs(const std::map<std::string, std::string>& env) {
    std::vector<fs::path> out;
    data_dirs_(env, "wayland-sessions", out, true);
    return out;
}

std::vector<fs::path> application_dirs(const std::map<std::string, std::string>& env) {
    std::vector<fs::path> out;
    if (auto it = env.find("XDG_DATA_HOME"); it != env.end() && it->second.starts_with('/'))
        out.push_back(fs::path(it->second) / "applications");
    else if (auto home = env.find("HOME"); home != env.end() && home->second.starts_with('/'))
        out.push_back(fs::path(home->second) / ".local/share/applications");
    data_dirs_(env, "applications", out, false);
    return out;
}

std::vector<std::string> exec_argv(std::string_view exec) {
    std::vector<std::string> out;
    std::string word;
    bool in_word = false, quoted = false;
    for (std::size_t k = 0; k < exec.size(); ++k) {
        const char c = exec[k];
        if (quoted) {
            if (c == '"') quoted = false;
            else if (c == '\\' && k + 1 < exec.size()) word.push_back(exec[++k]);
            else word.push_back(c);
        } else if (c == '"') {
            quoted = in_word = true;
        } else if (c == ' ' || c == '\t') {
            if (in_word) out.push_back(std::exchange(word, {}));
            in_word = false;
        } else if (c == '%' && k + 1 < exec.size()) {
            // A field code: %% is a percent, any other is dropped (nothing is opened).
            if (exec[++k] == '%') { word.push_back('%'); in_word = true; }
        } else {
            word.push_back(c);
            in_word = true;
        }
    }
    if (in_word) out.push_back(std::move(word));
    return out;
}

std::optional<Application> parse_application(std::string_view text, std::string id) {
    const auto keys = entry_keys_(text);
    auto key = [&](std::string_view k) {
        auto it = keys.find(k);
        return it == keys.end() ? std::string_view{} : std::string_view(it->second);
    };
    if (key("Type") != "Application" || key("NoDisplay") == "true" || key("Hidden") == "true") return std::nullopt;
    Application a{.id = std::move(id), .name = std::string(key("Name")), .argv = exec_argv(key("Exec")),
                  .terminal = key("Terminal") == "true"};
    if (a.argv.empty()) return std::nullopt;
    if (a.name.empty()) a.name = a.id;
    return a;
}

std::vector<Application> applications(const std::vector<fs::path>& dirs) {
    auto out = entries_<Application>(dirs, [](std::string_view text, const std::string& id, const fs::path&) {
        return parse_application(text, id);
    });
    std::ranges::sort(out, {}, &Application::name);
    return out;
}

std::vector<Session> sessions(const std::vector<fs::path>& dirs) {
    return entries_<Session>(dirs, [](std::string_view text, const std::string& id, const fs::path& f) {
        auto s = parse_session(text, id);
        if (s) s->file = f;
        return s;
    });
}

std::optional<Session> find_session(std::string_view id, const std::vector<fs::path>& dirs) {
    for (auto& s : sessions(dirs))
        if (s.id == id) return s;
    return std::nullopt;
}

std::optional<Session> resolve_session(std::string_view id, const std::vector<fs::path>& dirs) {
    if (!id.starts_with(kAppSession)) return find_session(id, dirs);
    const auto command = id.substr(kAppSession.size());
    if (command.empty()) return std::nullopt;
    for (auto s : sessions(dirs)) {
        if (s.app_exec.empty() || s.native) continue;
        std::string quoted = "'";
        for (char c : command) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        quoted += "'";
        s.exec = s.app_exec + " " + quoted;
        s.interfaces &= ~static_cast<opensal::Interfaces>(opensal::Interface::Shell);
        const auto words = exec_argv(command);
        s.name = words.empty() ? std::string(command) : fs::path(words[0]).filename().string();
        s.id = std::string(id);
        return s;
    }
    return std::nullopt;
}

// ── a session ────────────────────────────────────────────────────────

fs::path displays_socket(const fs::path& runtime_root, std::string_view session_id) {
    std::string id(session_id);
    for (auto& c : id)
        if (c == '/' || c == ' ' || c == ':') c = '_';
    return runtime_root / std::format("displays-{}.sock", id);
}

namespace {

// Another window's stream joined to a running desktop's displays socket,
// both ways, until either side ends; nullopt when nothing listens there.
std::optional<int> relay_(const fs::path& socket, int in, int out) {
    const int fd = os::connect_unix(socket);
    if (fd < 0) return std::nullopt;
    std::array<char, 65536> buf{};
    for (bool open = true; open;) {
        std::array<os::PollFd, 2> p{{{.fd = in}, {.fd = fd}}};
        os::poll(p, 1000);
        for (int k = 0; k < 2 && open; ++k) {
            if (!p[static_cast<std::size_t>(k)].readable && !p[static_cast<std::size_t>(k)].closed) continue;
            const auto n = os::read_some(k == 0 ? in : fd, buf);
            open = n && *n > 0 && os::write_all(k == 0 ? fd : out, std::string_view(buf.data(), *n));
        }
    }
    os::close(fd);
    return 0;
}

}  // namespace

int run_session(const Session& s, int in, int out, const SessionOptions& o) {
    std::error_code ec;
    // The desktop already runs: this window is another display of it.
    fs::create_directories(o.runtime_root, ec);
    const auto displays = displays_socket(o.runtime_root, s.id);
    if (auto joined = relay_(displays, in, out)) return *joined;
    const auto runtime = o.runtime_root / std::format("display-{}", os::pid());
    fs::remove_all(runtime, ec);
    fs::create_directories(runtime, ec);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) return 2;
    auto env = o.env;
    env["XDG_RUNTIME_DIR"] = runtime.string();
    env["XDG_SESSION_TYPE"] = "wayland";
    env["WLR_BACKENDS"] = "headless";
    env["WLR_RENDERER"] = "pixman";
    env["WLR_HEADLESS_OUTPUTS"] = "1";
    env["WLR_LIBINPUT_NO_DEVICES"] = "1";
    // The pointer is drawn by the compositor into the frames: it needs a
    // cursor theme. Adwaita when the system has it and nobody chose one.
    if (!env.contains("XCURSOR_THEME") && fs::is_directory("/usr/share/icons/Adwaita/cursors", ec)) env["XCURSOR_THEME"] = "Adwaita";
    if (!env.contains("XCURSOR_SIZE")) env["XCURSOR_SIZE"] = "24";
    env.erase("WAYLAND_DISPLAY");
    env.erase("DISPLAY");
    if (s.native) env["OPENSAL_DISPLAYS"] = displays.string();
    // A session starts at home, as a login does (its terminals open there).
    const int log = o.log.empty() ? -1 : os::open_append(o.log);
    const int pid = os::spawn({.argv = {"/bin/sh", "-c", "exec " + s.exec}, .env = env,
                               .cwd = env.contains("HOME") ? fs::path(env.at("HOME")) : fs::path{},
                               .stdio = {s.native ? in : -1, s.native ? out : log, log}});
    os::close(log);
    if (pid < 0) return 2;
    if (s.native) {
        // It is the desktop and the stream's other end: done when it is --
        // or, when this window's stream ends while it still runs, it serves
        // other displays and goes on without this one.
        for (;;) {
            if (const auto st = os::exited(pid)) {
                fs::remove_all(runtime, ec);
                return *st == 0 ? 0 : 1;
            }
            // Its input is the desktop's to read: only its end is looked for.
            std::array<os::PollFd, 1> p{{{.fd = in}}};
            os::poll(p, 500);
            if (!p[0].closed) {
                if (p[0].readable) std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            for (int k = 0; k < 40; ++k) {
                if (const auto st = os::exited(pid)) {
                    fs::remove_all(runtime, ec);
                    return *st == 0 ? 0 : 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return 0;
        }
    }
    bool ended = false;
    auto alive = [&] {
        if (!ended && os::exited(pid)) ended = true;
        return !ended;
    };
    std::optional<fs::path> socket;
    for (int i = 0; i < 150 && alive() && !socket; ++i) {
        socket = wayland::find_socket(runtime);
        if (!socket) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    int code = 1;
    if (socket) {
        // What it starts as the session's shell joins the compositor.
        auto launched = env;
        launched["WAYLAND_DISPLAY"] = socket->filename().string();
        code = bridge({.in = in, .out = out, .socket = *socket, .alive = alive,
                       .shell = opensal::has(s.interfaces, opensal::Interface::Shell), .env = std::move(launched),
                       .displays = displays});
    }
    if (alive()) {
        os::terminate(pid);
        for (int i = 0; i < 30 && alive(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (alive()) { os::kill(pid); (void)os::wait(pid); }
    }
    fs::remove_all(runtime, ec);
    return code;
}

}  // namespace opensal::wlr
