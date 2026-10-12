module luban.desktop.wlr;

import std;
import opensal;
import xlings.platform;
import luban.desktop.wayland;

namespace luban::desktop::wlr {

namespace platform = xlings::platform;

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

// ── the keyboard ─────────────────────────────────────────────────────

std::optional<Stroke> evdev_key(std::uint32_t k) {
    // Letters, by their place on the US layout.
    static constexpr std::array<std::uint32_t, 26> letters{30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                                          49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    if (k >= 'a' && k <= 'z') return Stroke{letters[k - 'a'], false};
    if (k >= 'A' && k <= 'Z') return Stroke{letters[k - 'A'], true};
    if (k >= '1' && k <= '9') return Stroke{2 + (k - '1'), false};
    // The rest of the printable keys: unshifted, then shifted.
    static constexpr std::array<std::pair<std::uint32_t, Stroke>, 43> table{{
        {'0', {11, false}}, {' ', {57, false}}, {'-', {12, false}}, {'=', {13, false}}, {'[', {26, false}},
        {']', {27, false}}, {'\\', {43, false}}, {';', {39, false}}, {'\'', {40, false}}, {'`', {41, false}},
        {',', {51, false}}, {'.', {52, false}}, {'/', {53, false}},
        {'!', {2, true}}, {'@', {3, true}}, {'#', {4, true}}, {'$', {5, true}}, {'%', {6, true}}, {'^', {7, true}},
        {'&', {8, true}}, {'*', {9, true}}, {'(', {10, true}}, {')', {11, true}}, {'_', {12, true}}, {'+', {13, true}},
        {'{', {26, true}}, {'}', {27, true}}, {'|', {43, true}}, {':', {39, true}}, {'"', {40, true}}, {'~', {41, true}},
        {'<', {51, true}}, {'>', {52, true}}, {'?', {53, true}},
        // Return, BackSpace, Tab, Escape, Delete, Insert, Home, End, PageUp, PageDown.
        {0xff0d, {28, false}}, {0xff08, {14, false}}, {0xff09, {15, false}}, {0xff1b, {1, false}}, {0xffff, {111, false}},
        {0xff63, {110, false}}, {0xff50, {102, false}}, {0xff57, {107, false}}, {0xff55, {104, false}}}};
    for (const auto& [sym, stroke] : table)
        if (sym == k) return stroke;
    switch (k) {
    case 0xff56: return Stroke{109, false};     // Page_Down
    case 0xff51: return Stroke{105, false};     // Left
    case 0xff52: return Stroke{103, false};     // Up
    case 0xff53: return Stroke{106, false};     // Right
    case 0xff54: return Stroke{108, false};     // Down
    case 0xffe1: return Stroke{42, false};      // Shift_L
    case 0xffe2: return Stroke{54, false};      // Shift_R
    case 0xffe3: return Stroke{29, false};      // Control_L
    case 0xffe4: return Stroke{97, false};      // Control_R
    case 0xffe5: return Stroke{58, false};      // Caps_Lock
    case 0xffe9: return Stroke{56, false};      // Alt_L
    case 0xffea: return Stroke{100, false};     // Alt_R
    case 0xffeb: return Stroke{125, false};     // Super_L
    case 0xffec: return Stroke{126, false};     // Super_R
    default: break;
    }
    if (k >= 0xffbe && k <= 0xffc7) return Stroke{59 + (k - 0xffbe), false};   // F1..F10
    if (k == 0xffc8) return Stroke{87, false};                                   // F11
    if (k == 0xffc9) return Stroke{88, false};                                   // F12
    return std::nullopt;
}

std::uint32_t modifier_of(std::uint32_t k) {
    switch (k) {
    case 0xffe1: case 0xffe2: return opensal::Shift;
    case 0xffe3: case 0xffe4: return opensal::Ctrl;
    case 0xffe9: case 0xffea: return opensal::Alt;
    case 0xffeb: case 0xffec: return opensal::Super;
    default: return 0;
    }
}

std::uint32_t xkb_mods(std::uint32_t m) {
    std::uint32_t out = 0;
    if (m & opensal::Shift) out |= 1u << 0;
    if (m & opensal::Ctrl) out |= 1u << 2;
    if (m & opensal::Alt) out |= 1u << 3;
    if (m & opensal::Super) out |= 1u << 6;
    return out;
}

std::uint32_t evdev_button(opensal::Button b) {
    switch (b) {
    case opensal::Button::Middle: return 0x112;
    case opensal::Button::Right: return 0x111;
    default: return 0x110;
    }
}

// ── a session ────────────────────────────────────────────────────────

int run_session(const Session& s, int in, int out, const SessionOptions& o) {
    std::error_code ec;
    const auto runtime = o.runtime_root / std::format("display-{}", platform::current_pid());
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
    env.erase("WAYLAND_DISPLAY");
    env.erase("DISPLAY");
    const int pid = platform::fork_process();
    if (pid == 0) {
        platform::new_session();
        const int null = platform::open_null();
        const int log = o.log.empty() ? platform::open_null() : platform::open_for_append(o.log);
        const std::array<int, 3> stdio{null, log, log};
        platform::redirect_stdio(stdio);
        platform::exec_path({"/bin/sh", "-c", "exec " + s.exec}, env);
        platform::exit_now(127);
    }
    if (pid < 0) return 2;
    bool ended = false;
    auto alive = [&] {
        if (!ended && platform::wait_process(pid, false)) ended = true;
        return !ended;
    };
    std::optional<fs::path> socket;
    for (int i = 0; i < 150 && alive() && !socket; ++i) {
        socket = desktop::wayland::find_socket(runtime);
        if (!socket) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    int code = 1;
    if (socket) code = bridge({.in = in, .out = out, .socket = *socket, .alive = alive});
    if (alive()) {
        platform::send_signal(pid, platform::sig::terminate);
        for (int i = 0; i < 30 && alive(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (alive()) { platform::send_signal(pid, platform::sig::kill); (void)platform::wait_process(pid, true); }
    }
    fs::remove_all(runtime, ec);
    return code;
}

}  // namespace luban::desktop::wlr
