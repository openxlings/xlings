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
    std::println(std::cerr, "xdesk: not yet");
    return 2;
}
