import std;
import opensal;
import xdesk.compositor;

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
    o.log = [](std::string_view line) { std::println(std::cerr, "xdesk: {}", line); };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size()) return std::nullopt;
            return std::string(args[++i]);
        };
        const auto a = args[i];
        std::optional<std::string> v;
        if (a == "--profile" && (v = value())) o.profile = *v;
        else if (a == "--terminal" && (v = value())) o.terminal = {"/bin/sh", "-c", "exec " + *v};
        else if (a == "--autostart" && (v = value())) o.autostart.push_back({"/bin/sh", "-c", "exec " + *v});
        else if (a == "--size" && (v = value())) {
            unsigned w = 0, h = 0;
            if (std::sscanf(v->c_str(), "%ux%u", &w, &h) != 2 || !w || !h) {
                std::println(std::cerr, "xdesk: --size takes WIDTHxHEIGHT, as 1280x800");
                return 2;
            }
            o.size = {w, h};
        } else {
            std::println(std::cerr, "usage: xdesk [--profile classic|canvas] [--size WxH] [--terminal CMD] [--autostart CMD]...\n"
                                    "       xdesk --probe | --keymap LAYOUT");
            return 2;
        }
    }
    if (o.profile != "classic" && o.profile != "canvas") {
        std::println(std::cerr, "xdesk: no profile '{}' (classic, canvas)", o.profile);
        return 2;
    }
    return xdesk::compositor::run(o);
}
