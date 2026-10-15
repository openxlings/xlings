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
    bool in_entry = false, hidden = false;
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
        else if ((key == "Hidden" || key == "NoDisplay") && value == "true") hidden = true;
    }
    if (s.exec.empty() || hidden) return std::nullopt;
    if (s.name.empty()) s.name = s.id;
    return s;
}

opensal::Interfaces parse_interfaces(std::string_view names) {
    static constexpr std::array<std::pair<std::string_view, opensal::Interface>, 9> known{{
        {"surface", opensal::Interface::Surface}, {"pointer", opensal::Interface::Pointer},
        {"keyboard", opensal::Interface::Keyboard}, {"mode", opensal::Interface::Mode},
        {"density", opensal::Interface::Density}, {"capture", opensal::Interface::Capture},
        {"cursor", opensal::Interface::Cursor}, {"text", opensal::Interface::Text},
        {"clipboard", opensal::Interface::Clipboard}}};
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

std::vector<fs::path> session_dirs(const std::map<std::string, std::string>& env) {
    std::vector<fs::path> out;
    auto add = [&](fs::path p) {
        p /= "wayland-sessions";
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
    add("/usr/local/share");
    add("/usr/share");
    return out;
}

std::vector<Session> sessions(const std::vector<fs::path>& dirs) {
    std::vector<Session> out;
    std::set<std::string> seen;
    for (const auto& dir : dirs) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".desktop") files.push_back(it->path());
        std::ranges::sort(files);
        for (const auto& f : files) {
            const auto id = f.stem().string();
            if (!seen.insert(id).second) continue;   // the more specific directory's wins
            std::ifstream in(f);
            std::string text((std::istreambuf_iterator<char>(in)), {});
            if (auto s = parse_session(text, id)) {
                s->file = f;
                out.push_back(std::move(*s));
            }
        }
    }
    return out;
}

std::optional<Session> find_session(std::string_view id, const std::vector<fs::path>& dirs) {
    for (auto& s : sessions(dirs))
        if (s.id == id) return s;
    return std::nullopt;
}

// ── a session ────────────────────────────────────────────────────────

int run_session(const Session& s, int in, int out, const SessionOptions& o) {
    std::error_code ec;
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
    // A session starts at home, as a login does (its terminals open there).
    const int log = o.log.empty() ? -1 : os::open_append(o.log);
    const int pid = os::spawn({.argv = {"/bin/sh", "-c", "exec " + s.exec}, .env = env,
                               .cwd = env.contains("HOME") ? fs::path(env.at("HOME")) : fs::path{},
                               .stdio = {s.native ? in : -1, s.native ? out : log, log}});
    os::close(log);
    if (pid < 0) return 2;
    if (s.native) {
        // It is the desktop and the stream's other end: done when it is.
        const int status = os::wait(pid);
        fs::remove_all(runtime, ec);
        return status == 0 ? 0 : 1;
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
    if (socket) code = bridge({.in = in, .out = out, .socket = *socket, .alive = alive});
    if (alive()) {
        os::terminate(pid);
        for (int i = 0; i < 30 && alive(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (alive()) { os::kill(pid); (void)os::wait(pid); }
    }
    fs::remove_all(runtime, ec);
    return code;
}

}  // namespace opensal::wlr
