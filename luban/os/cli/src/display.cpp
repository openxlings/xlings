// `luban display <name> [action]` (design 2026-10-24 §4.3): the displays of a
// running desktop, through its supervisor and the displays' own controls on
// the bus -- the same paths an agent or a test uses (xbus call ...).
module luban.cli;

import std;
import xlings.libs.json;
import xlings.platform;
import luban.service;
import luban.vm;
import openbus;
import xbus.node;
import openhal.display;

namespace luban::cli {

namespace {

namespace svc = luban::service;
namespace vm = luban::vm;
using Json = nlohmann::json;

std::expected<std::shared_ptr<xbus::Node>, std::string> node_(const std::filesystem::path& home) {
    auto fd = vm::connect_bus(home);
    if (!fd) return std::unexpected(fd.error());
    std::vector<int> close{*fd};
    xlings::platform::close_fds(close);
    return xbus::Node::connect({.name = "luban", .actor = vm::bus_actor(),
                                .router = xbus::Address{"unix", vm::bus_socket(home).string()}, .telemetry = false});
}

int fail_(const svc::Context& c, std::string why, std::string next = {}) {
    if (c.sink) c.sink(svc::error(std::move(why), std::move(next)));
    return 1;
}

// A call the router refused: an agent without the system's grant (design 2026-10-24 §3.4).
int refused_(const svc::Context& c, std::string_view name, const xbus::CallError& e) {
    if (e.code == openbus::ErrorCode::denied)
        return fail_(c, std::format("an agent drives {}'s desktop with its grant", name), std::format("luban config {} --allow desktop-control", name));
    return fail_(c, e.message);
}

}  // namespace

int display(const svc::Context& c, std::string_view name, std::span<const std::string> words,
            std::optional<std::string> which, std::optional<std::string> as, bool json) {
    const std::string action = words.empty() ? "list" : words[0];
    auto st = vm::desktop(c.home, name, {{"op", "status"}});
    if (!st) return fail_(c, st.error(), std::format("luban enter {} --display virtual", name));
    const auto& displays = (*st)["displays"];
    // The display meant: --display N (its number in the list), else the virtual one.
    std::string path = st->value("virtual", std::string{});
    if (which) {
        std::size_t n = 0;
        if (std::from_chars(which->data(), which->data() + which->size(), n).ec != std::errc{} || n >= displays.size())
            return fail_(c, std::format("no display {} (luban display {})", *which, name));
        path = displays[n].value("path", std::string{});
    }
    if (action == "list") {
        if (json) { std::println("{}", st->dump(2)); return 0; }
        std::println("{}  {}  since {}", name, st->value("running", false) ? "running" : "ended", st->value("since", std::string{}));
        for (std::size_t k = 0; k < displays.size(); ++k)
            std::println("  {}  {:<8} {:<10} {}", k, displays[k].value("kind", std::string{}), displays[k].value("role", std::string{}),
                         displays[k].value("path", std::string{}));
        return 0;
    }
    if (action == "end") {
        auto ended = vm::desktop(c.home, name, {{"op", "end"}, {"by", "luban display end"}});
        if (!ended) return fail_(c, ended.error());
        std::println("✓ {}'s desktop is ending", name);
        return 0;
    }
    if (action == "detach") {
        if (words.size() < 2) return fail_(c, "detach takes the display's path (luban display " + std::string(name) + ")");
        auto done = vm::desktop(c.home, name, {{"op", "detach"}, {"path", words[1]}});
        if (!done) return fail_(c, done.error());
        std::println("✓ {} detached", words[1]);
        return 0;
    }
    if (action == "attach") {
        // Another virtual display (design 2026-10-24 §4.4): an output of the
        // desktop's own beside the others by default (--role extend), or a mirror.
        if (words.size() >= 2 && (words[1] == "virtual" || words[1].starts_with("virtual="))) {
            Json req{{"op", "attach"}, {"kind", "virtual"}, {"role", as.value_or("extend")}};
            if (const auto eq = words[1].find('='); eq != std::string::npos) {
                if (!openhal::display::parse_mode(words[1].substr(eq + 1)))
                    return fail_(c, "virtual=<width>x<height>[@<scale>], as virtual=1920x1080@1.25");
                req["mode"] = words[1].substr(eq + 1);
            }
            auto done = vm::desktop(c.home, name, req);
            if (!done) return fail_(c, done.error());
            if (json) { std::println("{}", done->dump(2)); return 0; }
            const auto& all = (*done)["displays"];
            std::println("✓ display {} attached ({}): {}", all.size() - 1, as.value_or("extend"),
                         all.empty() ? std::string{} : all.back().value("path", std::string{}));
            return 0;
        }
        if (words.size() < 2 || words[1] != "window") return fail_(c, "attach takes window or virtual[=WxH]: the Luban OS window shows it");
        const auto app = svc::window_app(c, name);
        if (!app) { if (c.sink) c.sink(app.error()); return 1; }
        // The window comes as the primary: the desktop takes its size (opensal 0.7).
        if (as && *as != "primary") return fail_(c, "the Luban OS window comes as the primary (--role primary)");
        return xlings::platform::run_argv({app->string(), "--attach", std::string(name)});
    }
    auto node = node_(c.home);
    if (!node) return fail_(c, node.error());
    const auto control = openhal::display::control_path(path);
    if (action == "capture") {
        if (words.size() < 2) return fail_(c, "capture takes a file: luban display " + std::string(name) + " capture shot.png");
        auto png = (*node)->call(control, Json{{"op", "capture"}}.dump(), std::chrono::seconds(20));
        if (!png) return refused_(c, name, png.error());
        if (png->empty()) return fail_(c, "no picture came");
        std::string bytes;
        for (const auto& part : *png) bytes += part;
        xlings::platform::write_file_atomic(words[1], bytes);
        std::println("✓ {} ({} bytes, display {})", words[1], bytes.size(), path);
        return 0;
    }
    if (action == "record") {
        if (words.size() < 3) return fail_(c, "record takes seconds and a directory: record 10 out/");
        int seconds = 10;
        std::from_chars(words[1].data(), words[1].data() + words[1].size(), seconds);
        std::error_code ec;
        std::filesystem::create_directories(words[2], ec);
        // Each frame: its header, then its PNG in pieces.
        int n = 0;
        std::string frame;
        std::size_t want = 0;
        const auto flush = [&] {
            if (frame.empty()) return;
            xlings::platform::write_file_atomic((std::filesystem::path(words[2]) / std::format("frame-{:04}.png", n++)).string(), frame);
            frame.clear();
        };
        auto done = (*node)->call_each(control, Json{{"op", "record"}, {"seconds", seconds}, {"frames", 600}}.dump(), [&](std::string_view part) {
            if (frame.size() >= want) {
                if (const auto h = Json::parse(part, nullptr, false); h.is_object() && h.contains("frame")) {
                    flush();
                    want = h.value("bytes", std::size_t{0});
                    return;
                }
            }
            frame += part;
        }, std::chrono::seconds(seconds + 20));
        flush();
        if (!done) return refused_(c, name, done.error());
        std::println("✓ {} frames in {}", n, words[2]);
        return 0;
    }
    if (action == "mode" || action == "input") {
        if (words.size() < 2) return fail_(c, action == "mode" ? "mode takes <w>x<h>[@<scale>]" : "input takes the steps (JSON)");
        Json req{{"op", action}};
        if (action == "mode") req["mode"] = words[1];
        else req["events"] = Json::parse(words[1], nullptr, false);
        auto answer = (*node)->call(control, req.dump(), std::chrono::seconds(10));
        if (!answer) return refused_(c, name, answer.error());
        std::println("{}", answer->empty() ? std::string("{}") : answer->front());
        return 0;
    }
    return fail_(c, std::format("no action '{}' (list, capture, record, attach, detach, mode, input, end)", action));
}

}  // namespace luban::cli
