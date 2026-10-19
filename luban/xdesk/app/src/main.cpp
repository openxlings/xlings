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
    const auto settings = xdesk::config::load(o.profile, xdesk::os::environment());
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
    o.minimap = settings.get("canvas", "minimap", "on") != "off";
    o.snap = settings.get("canvas", "snap", "on") != "off";
    o.workspaces = settings.get_int("desktop", "workspaces", o.workspaces);
    if (const auto c = settings.colour("theme", "accent", 0x1000000); c != 0x1000000) o.accent = c;
    if (const auto c = settings.colour("theme", "background", 0x1000000); c != 0x1000000) o.background = c;
    o.keyboard_layout = settings.get("keyboard", "layout", o.keyboard_layout);
    o.repeat_rate = settings.get_int("keyboard", "repeat_rate", o.repeat_rate);
    o.repeat_delay = settings.get_int("keyboard", "repeat_delay", o.repeat_delay);
    return xdesk::compositor::run(o);
}
