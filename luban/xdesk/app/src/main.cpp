import std;
import opensal;
import xdesk.compositor;
import xdesk.apps;
import xdesk.config;
import xdesk.os;

int main(int argc, char* argv[]) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (!args.empty() && args[0] == "--probe") {
        auto f = xdesk::compositor::probe({640, 400});
        if (!f) { std::println(std::cerr, "xdesk: {}", f.error()); return 1; }
        std::println("{}x{} stride {} first pixel {:02x}{:02x}{:02x}", f->size.width, f->size.height, f->stride,
                     f->pixels[2], f->pixels[1], f->pixels[0]);
        return 0;
    }
    // `xdesk msg ...`: a request to the running desktop's control socket
    // (design 2026-10-20 §12.3), its answer printed (JSON).
    if (!args.empty() && args[0] == "msg") {
        const auto env = xdesk::os::environment();
        if (!env.contains("XDG_RUNTIME_DIR") || !env.contains("WAYLAND_DISPLAY")) {
            std::println(std::cerr, "xdesk msg: no desktop here (XDG_RUNTIME_DIR, WAYLAND_DISPLAY)");
            return 2;
        }
        auto quoted = [](std::string_view v) {
            std::string out = "\"";
            for (char c : v) { if (c == '"' || c == '\\') out += '\\'; out += c; }
            return out + "\"";
        };
        std::string request;
        if (args.size() >= 2 && args[1] == "settings") request = std::format("{{\"settings\": {}}}", quoted(args.size() > 2 ? args[2] : "displays"));
        else if (args.size() == 3 && args[1] == "keys" && args[2] == "list") request = R"({"keys": "list"})";
        else if (args.size() == 5 && args[1] == "keys" && args[2] == "bind")
            request = std::format("{{\"keys\": \"bind\", \"action\": {}, \"chord\": {}}}", quoted(args[3]), quoted(args[4]));
        else if (args.size() == 4 && args[1] == "keys" && args[2] == "unbind") request = std::format("{{\"keys\": \"unbind\", \"action\": {}}}", quoted(args[3]));
        else if (args.size() == 4 && args[1] == "keys" && args[2] == "set") request = std::format("{{\"keys\": \"set\", \"set\": {}}}", quoted(args[3]));
        else {
            std::println(std::cerr, "usage: xdesk msg keys list | keys bind ACTION CHORD | keys unbind ACTION | keys set luban|luban-no-hjkl\n"
                                    "       xdesk msg settings [displays|layout|appearance|keys|about]");
            return 2;
        }
        const auto path = std::filesystem::path(env.at("XDG_RUNTIME_DIR")) / std::format("xdesk-{}.sock", env.at("WAYLAND_DISPLAY"));
        const int fd = xdesk::os::connect_unix(path);
        if (fd < 0) { std::println(std::cerr, "xdesk msg: no xdesk answers at {}", path.string()); return 1; }
        if (!xdesk::os::write_all(fd, request + "\n")) { std::println(std::cerr, "xdesk msg: the desktop went away"); return 1; }
        std::string reply;
        std::array<char, 65536> buf{};
        while (reply.find('\n') == std::string::npos) {
            const auto n = xdesk::os::read_some(fd, buf);
            if (!n || *n == 0) break;
            reply.append(buf.data(), *n);
        }
        xdesk::os::close(fd);
        if (const auto nl = reply.find('\n'); nl != std::string::npos) reply.resize(nl);
        std::println("{}", reply);
        return reply.find("\"ok\": true") != std::string::npos || reply.find("\"ok\":true") != std::string::npos ? 0 : 1;
    }
    if (args.size() == 2 && args[0] == "--keys-markdown") {
        std::print("{}", xdesk::compositor::keys_markdown(args[1] == "zh"));
        return 0;
    }
    if (args.size() == 2 && args[0] == "--keymap") {
        auto k = xdesk::compositor::keymap_text(args[1]);
        if (!k) { std::println(std::cerr, "xdesk: {}", k.error()); return 1; }
        std::print("{}", *k);
        return 0;
    }
    // The desktop: an opensal consumer on stdin/stdout -- the window that
    // shows it (Luban OS, a machine's display) is the provider.
    xdesk::compositor::Options o;
    bool terminal_given = false;
    o.log = [](std::string_view line) { std::println(std::cerr, "xdesk: {}", line); };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size()) return std::nullopt;
            return std::string(args[++i]);
        };
        const auto a = args[i];
        std::optional<std::string> v;
        if (a == "--profile" && (v = value())) o.profile = *v;
        else if (a == "--terminal" && (v = value())) { o.terminal = xdesk::apps::argv(*v); terminal_given = true; }   // as an Exec= line
        else if (a == "--autostart" && (v = value())) o.autostart.push_back({"/bin/sh", "-c", "exec " + *v});
        else if (a == "--size" && (v = value())) {
            unsigned w = 0, h = 0;
            if (std::sscanf(v->c_str(), "%ux%u", &w, &h) != 2 || !w || !h) {
                std::println(std::cerr, "xdesk: --size takes WIDTHxHEIGHT, as 1280x800");
                return 2;
            }
            o.size = {w, h};
        } else {
            std::println(std::cerr, "usage: xdesk [--profile xdesk|classic|canvas] [--size WxH] [--terminal CMD] [--autostart CMD]...\n"
                                    "       xdesk --probe | --keymap LAYOUT");
            return 2;
        }
    }
    // xdesk: each workspace as it was left (tile the first time); classic and
    // canvas: what a workspace not yet set starts as (design 2026-10-19 §17.12).
    if (o.profile != "xdesk" && o.profile != "classic" && o.profile != "canvas") {
        std::println(std::cerr, "xdesk: no profile '{}' (xdesk, classic, canvas)", o.profile);
        return 2;
    }
    // The profile's settings (a config: package's, then the person's own);
    // what the command line said stays.
    // One set of settings for every session (design 2026-10-20 WuXian §12.2):
    // xdesk.ini, whatever the session was called.
    const auto settings = xdesk::config::load(o.config_profile, xdesk::os::environment());
    for (const auto& p : settings.problems) o.log("settings: " + p);
    if (!terminal_given) {
        if (auto t = settings.get("desktop", "terminal"); !t.empty()) o.terminal = xdesk::apps::argv(t);
    }
    if (o.autostart.empty())
        for (const auto& a : settings.all("desktop", "autostart")) o.autostart.push_back({"/bin/sh", "-c", "exec " + a});
    // `gap` of before is the gap between windows.
    o.inner_gap = settings.get_int("desktop", "inner_gap", settings.get_int("desktop", "gap", o.inner_gap));
    o.outer_gap = settings.get_int("desktop", "outer_gap", o.outer_gap);
    o.min_column_width = settings.get_int("desktop", "min_column_width", o.min_column_width);
    o.flow = settings.get("desktop", "flow", o.flow);
    // [wuxian] (design 2026-10-20 WuXian §3.16); [canvas] of before is read too.
    o.minimap = settings.get("wuxian", "minimap", settings.get("canvas", "minimap", "on")) != "off";
    o.snap = settings.get("wuxian", "snap", settings.get("canvas", "snap", "on")) != "off";
    o.reduce_motion = settings.get("appearance", "reduce_motion", "off") == "on";
    o.rule = settings.get("desktop", "rule", o.rule);
    o.status_others = settings.get("desktop", "status_others", "on") != "off";
    o.switcher_hold_ms = std::clamp(settings.get_int("switchers", "hold_ms", o.switcher_hold_ms), 100, 3000);
    o.highlight_ms = std::clamp(settings.get_int("switchers", "highlight_ms", o.highlight_ms), 0, 5000);
    if (const auto d = settings.get("switchers", "dim", ""); !d.empty()) o.dim = std::clamp(std::atof(d.c_str()), 0.1, 1.0);
    if (const auto s = settings.get("scroll", "speed", "medium"); s == "slow") o.wheel_speed = 0.6;
    else if (s == "fast") o.wheel_speed = 1.6;
    o.snap_columns = settings.get("scroll", "snap", "on") != "off";
    o.renderer = settings.get("appearance", "renderer", o.renderer);
    if (const auto c = settings.colour("theme", "accent", 0x1000000); c != 0x1000000) o.accent = c;
    if (const auto c = settings.colour("theme", "background", 0x1000000); c != 0x1000000) o.background = c;
    // Other windows of this desktop connect here (opensal-wlr's run_session says where).
    if (const auto env = xdesk::os::environment(); env.contains("OPENSAL_DISPLAYS")) o.displays = env.at("OPENSAL_DISPLAYS");
    o.keyboard_layout = settings.get("keyboard", "layout", o.keyboard_layout);
    o.repeat_rate = settings.get_int("keyboard", "repeat_rate", o.repeat_rate);
    o.repeat_delay = settings.get_int("keyboard", "repeat_delay", o.repeat_delay);
    return xdesk::compositor::run(o);
}
