// The desktop's control socket (server.cppm; design 2026-10-20 §12.3):
// $XDG_RUNTIME_DIR/xdesk-<WAYLAND_DISPLAY>.sock -- the session's runtime
// directory, its user's alone -- one JSON request a line, one answer a line:
//
//   {"keys": "list"}                                  every action: its keys, its group, what it does; the set
//   {"keys": "bind", "action": A, "chord": C}         A answers to C alone (taken from another that had it)
//   {"keys": "unbind", "action": A}                   A back to the set's keys
//   {"keys": "set", "set": "luban"|"luban-no-hjkl"}   the set of keys
//   {"settings": PAGE}                                the settings, at displays|layout|appearance|keys|about
//   {"state": true}                                   what the desktop is now (design 2026-10-21 §8.11)
//   {"debug": "hud", "on": true|false}                the frames' parts drawn in the top left
//   {"subscribe": true}                               what happens from now on, a line each
//   {"bookmark": N}                                   bookmark N (state's "bookmarks"), as clicked: an agent's way to it
//   {"telemetry": REQUEST}                            the process's telemetry: dump, metrics, recent [n], trace,
//                                                     profile on|off, level L, reset (the telemetry kit)
//
// `xdesk msg` is its client; scripts and agents use it as people use the
// settings: the same changes, kept the same way.
module xdesk.compositor;

import std;
import xdesk.os;
import xdesk.config;
import xdesk.apps;
import xdesk.layout;
import telemetry;
import xdesk.shell;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;
import :keys;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace config = xdesk::config;
namespace apps = xdesk::apps;
namespace layout = xdesk::layout;
namespace shell = xdesk::shell;
using Json = config::Json;

namespace {

std::string answer_(bool ok, std::string_view error = {}) {
    Json j{Json::Object{}};
    j.set("ok", Json{ok});
    if (!error.empty()) j.set("error", Json{std::string(error)});
    return j.dump();
}

}  // namespace

void Server::control_start() {
    const auto env = os::environment();
    if (!env.contains("XDG_RUNTIME_DIR") || socket.empty()) return;
    control_path = fs::path(env.at("XDG_RUNTIME_DIR")) / std::format("xdesk-{}.sock", socket);
    control_listener = os::listen_unix(control_path);
    if (control_listener < 0) { log("no control socket at " + control_path.string()); return; }
    control_source = wl_event_loop_add_fd(loop, control_listener, static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                          [](int, std::uint32_t, void* data) -> int {
                                              static_cast<Server*>(data)->control_accept();
                                              return 0;
                                          }, this);
}

void Server::control_accept() {
    const int fd = os::accept(control_listener);
    if (fd < 0) return;
    auto& c = controls.emplace_back();
    c.server = this;
    c.fd = fd;
    c.source = wl_event_loop_add_fd(loop, fd, static_cast<std::uint32_t>(WL_EVENT_READABLE_),
                                    [](int, std::uint32_t, void* data) -> int {
                                        auto* c = static_cast<Control*>(data);
                                        auto* self = c->server;
                                        std::array<char, 4096> buf{};
                                        const auto n = os::read_some(c->fd, buf);
                                        if (!n || *n == 0) c->gone = true;
                                        else c->buffer.append(buf.data(), *n);
                                        for (auto nl = c->buffer.find('\n'); !c->gone && nl != std::string::npos; nl = c->buffer.find('\n')) {
                                            const auto line = c->buffer.substr(0, nl);
                                            c->buffer.erase(0, nl + 1);
                                            self->asking = c;
                                            const auto answer = self->control(line);
                                            self->asking = nullptr;
                                            if (!os::write_all(c->fd, answer + "\n")) c->gone = true;
                                        }
                                        if (c->buffer.size() > 65536) c->gone = true;
                                        if (c->gone) {
                                            wl_event_source_remove(c->source);
                                            os::close(c->fd);
                                            self->controls.remove_if([c](const Control& o) { return &o == c; });
                                        }
                                        return 0;
                                    },
                                    &c);
}

namespace {

Json num_(double v) { return Json{v}; }
Json rect_(const layout::Rect& r) { return Json{Json::Array{num_(r.x), num_(r.y), num_(r.width), num_(r.height)}}; }

std::string_view overlay_name_(int o) {
    static constexpr std::array<std::string_view, 10> kNames{"none", "chooser", "switcher", "spread", "sheet", "menu",
                                                            "layouts", "settings", "dialog", "naming"};
    return o >= 0 && o < static_cast<int>(kNames.size()) ? kNames[static_cast<std::size_t>(o)] : "?";
}

}  // namespace

std::string Server::state_json() {
    Json out{Json::Object{}};
    out.set("ok", Json{true});
    // The output, its renderer and why, the view and its camera.
    Json output{Json::Object{}};
    output.set("size", Json{Json::Array{num_(size.width), num_(size.height)}});
    output.set("scale", num_(scale));
    output.set("renderer", Json{gpu ? std::string("gpu") : std::string("software")});
    output.set("renderer_text", Json{renderer_text});
    out.set("output", output);
    out.set("view", Json{std::string(wuxian() ? "wuxian" : "workspace")});
    const auto cam = lay.camera();
    Json camera{Json::Object{}};
    camera.set("at", Json{Json::Array{num_(cam.at.x), num_(cam.at.y)}});
    camera.set("zoom", num_(cam.zoom));
    out.set("camera", camera);
    // The workspaces and the windows: where each is, focused, its workspace (0: free).
    Json::Array workspaces;
    for (const auto& c : chips()) {
        Json w{Json::Object{}};
        w.set("id", num_(c.id));
        w.set("name", Json{c.name});
        w.set("rule", Json{std::string(c.stack ? "stack" : "tile")});
        w.set("windows", num_(c.windows));
        w.set("current", Json{c.current});
        w.set("collapsed", Json{lay.workspace_collapsed(c.id)});
        if (const auto f = std::ranges::find(lay.frames(), c.id, &layout::Frame::id); f != lay.frames().end()) w.set("frame", rect_(f->screen));
        workspaces.push_back(std::move(w));
    }
    out.set("workspaces", Json{workspaces});
    Json::Array windows;
    for (auto& v : views) {
        if (!v.mapped) continue;
        Json w{Json::Object{}};
        w.set("id", num_(static_cast<double>(v.id)));
        w.set("app_id", Json{std::string(v.toplevel->app_id ? v.toplevel->app_id : "")});
        w.set("title", Json{std::string(v.toplevel->title ? v.toplevel->title : "")});
        if (const auto p = std::ranges::find(placements, v.id, &layout::Placement::id); p != placements.end()) {
            w.set("rect", rect_(p->rect));
            w.set("workspace", num_(p->workspace));
            w.set("focused", Json{p->focused});
            w.set("visible", Json{p->visible});
            w.set("floating", Json{p->floating});
            w.set("collapsed", Json{p->collapsed});
        }
        windows.push_back(std::move(w));
    }
    out.set("windows", Json{windows});
    // The overlays: which is open, and the hint (where it is drawn).
    Json overlays{Json::Object{}};
    overlays.set("open", Json{std::string(overlay_name_(static_cast<int>(overlay)))});
    Json hint{Json::Object{}};
    hint.set("shown", Json{hint_node && hint_node->node.enabled});
    if (hint_node && hint_node->node.enabled) {
        hint.set("text", Json{hint_text});
        int w = 0, h = 0;
        if (hint_node->buffer) { w = static_cast<int>(std::lround(hint_node->buffer->width / scale)); h = static_cast<int>(std::lround(hint_node->buffer->height / scale)); }
        hint.set("rect", Json{Json::Array{num_(hint_node->node.x), num_(hint_node->node.y), num_(w), num_(h)}});
    }
    overlays.set("hint", hint);
    out.set("overlays", overlays);
    // What the launcher offers: the applications its directories have.
    {
        auto env = os::environment();
        std::string lang;
        for (const char* k : {"LC_ALL", "LC_MESSAGES", "LANG"})
            if (env.contains(k) && !env.at(k).empty()) { lang = env.at(k); break; }
        Json::Array listed;
        for (const auto& a : apps::scan(apps::dirs(env), lang)) {
            Json one{Json::Object{}};
            one.set("id", Json{a.id});
            one.set("name", Json{a.name});
            listed.push_back(std::move(one));
        }
        out.set("apps", Json{listed});
    }
    // The channels to the windows that show it, and the frames' parts (the last ten seconds).
    Json::Array channels;
    for (const auto& l : links) {
        if (l.gone) continue;
        Json c{Json::Object{}};
        c.set("display", num_(l.number));
        c.set("paced", Json{l.paced});
        c.set("hidden", Json{l.hidden});
        c.set("transport", Json{std::string("stream")});
        channels.push_back(std::move(c));
    }
    out.set("channels", Json{channels});
    Json frames{Json::Object{}};
    double compose = 0, readback = 0, send = 0, bytes = 0, whole = 0;
    for (const auto& f : frame_stats) { compose += f.compose_ms; readback += f.readback_ms; send += f.send_ms; bytes += static_cast<double>(f.bytes); whole += f.whole; }
    const double n = static_cast<double>(frame_stats.size());
    const double span = frame_stats.size() > 1
        ? std::max(0.001, std::chrono::duration<double>(frame_stats.back().at - frame_stats.front().at).count()) : 10.0;
    frames.set("count", num_(n));
    frames.set("fps", num_(frame_stats.size() > 1 ? (n - 1) / span : 0));
    frames.set("compose_ms", num_(n > 0 ? compose / n : 0));
    frames.set("readback_ms", num_(n > 0 ? readback / n : 0));
    frames.set("send_ms", num_(n > 0 ? send / n : 0));
    frames.set("bytes_per_s", num_(bytes / span));
    frames.set("whole", num_(whole));
    out.set("frames", frames);
    out.set("hud", Json{hud});
    // Covered items' bookmarks (design 2026-10-21 §8.4): what each is for, where it is.
    Json::Array marks;
    for (const auto& b : bookmarks) {
        Json m{Json::Object{}};
        if (b.window) m.set("window", num_(static_cast<double>(b.window)));
        if (b.workspace) m.set("workspace", num_(b.workspace));
        if (!b.more.empty()) m.set("more", num_(static_cast<double>(b.more.size())));
        m.set("label", Json{b.label});
        m.set("tab", rect_(b.tab));
        marks.push_back(std::move(m));
    }
    out.set("bookmarks", Json{marks});
    return out.dump();
}

void Server::draw_hud() {
    hud_drawn = std::chrono::steady_clock::now();
    if (!hud_node) return;
    wlr_scene_node_set_enabled(&hud_node->node, hud);
    if (!hud) return;
    const auto st = Json::parse(state_json());
    const auto* f = st ? st->get("frames") : nullptr;
    auto v = [&](std::string_view k) { const auto* x = f ? f->get(k) : nullptr; return x ? x->number_or(0) : 0.0; };
    std::vector<std::string> lines{
        std::format("{:.0f} frames/s · {} · {}", v("fps"), gpu ? "GPU" : "software", wuxian() ? "WuXian" : "workspace"),
        std::format("compose {:.2f} ms  readback {:.2f} ms  send {:.2f} ms", v("compose_ms"), v("readback_ms"), v("send_ms")),
        std::format("{:.1f} MB/s  whole frames {:.0f} of {:.0f} (10 s)", v("bytes_per_s") / 1e6, v("whole"), v("count"))};
    auto pic = shell::debug_hud(ui(), theme, lines, scale);
    wlr_scene_node_set_position(&hud_node->node, static_cast<int>(pic.x), static_cast<int>(pic.y));
    show(hud_node, std::move(pic.image), scale);
}

void Server::publish(std::string_view event, std::vector<std::pair<std::string, std::string>> fields) {
    const bool on_bus = bus && bus->connected();
    if (!on_bus && std::ranges::none_of(controls, &Control::subscribed)) return;
    Json j{Json::Object{}};
    j.set("event", Json{std::string(event)});
    for (auto& [k, v] : fields) j.set(k, Json{std::move(v)});
    const auto line = j.dump() + "\n";
    if (on_bus) bus->publish("topic/xdesk/events", std::string_view(line).substr(0, line.size() - 1));
    for (auto& c : controls)
        if (c.subscribed && !c.gone && !os::write_all(c.fd, line)) c.gone = true;
}

std::string Server::control(std::string_view request) {
    auto parsed = Json::parse(request);
    if (!parsed || !parsed->is_object()) return answer_(false, "a request is one JSON object a line");
    const auto& j = *parsed;
    auto str = [&](std::string_view k) { const auto* v = j.get(k); return v ? v->string_or("") : std::string{}; };
    if (const auto* st = j.get("state"); st && st->bool_or(false)) return state_json();
    if (const auto* bm = j.get("bookmark")) {
        const auto n = static_cast<std::size_t>(std::max(0.0, bm->number_or(-1)));
        if (bm->number_or(-1) < 0 || n >= bookmarks.size()) return answer_(false, std::format("no bookmark {} ({} shown)", n, bookmarks.size()));
        const auto b = bookmarks[n];
        if (!b.more.empty()) bookmark_choose(b.more.front().first, b.more.front().second);
        else bookmark_choose(b.window, b.workspace);
        return answer_(true);
    }
    if (const auto* t = j.get("telemetry")) {
        auto answer = telemetry::control(t->string_or("dump"));
        if (answer.is_object() && !answer.contains("error")) answer["ok"] = true;
        else if (answer.is_object()) answer["ok"] = false;
        return answer.dump(-1, ' ', false, telemetry::Json::error_handler_t::replace);
    }
    if (str("debug") == "hud") {
        const auto* on = j.get("on");
        hud = on ? on->bool_or(!hud) : !hud;
        draw_hud();
        return answer_(true);
    }
    if (const auto* sub = j.get("subscribe"); sub && sub->bool_or(false)) {
        // The asker's connection: the answer, then a line for each thing that happens.
        if (!asking) return answer_(false, "subscribe on the control socket");
        asking->subscribed = true;
        return answer_(true);
    }
    if (const auto page = str("settings"); !page.empty()) {
        static constexpr std::array<std::pair<std::string_view, SettingsPage>, 5> kPages{{
            {"displays", SettingsPage::displays}, {"layout", SettingsPage::layout}, {"appearance", SettingsPage::appearance},
            {"keys", SettingsPage::keys}, {"about", SettingsPage::about}}};
        const auto it = std::ranges::find(kPages, page, &std::pair<std::string_view, SettingsPage>::first);
        if (it == kPages.end()) return answer_(false, std::format("no settings page '{}' (displays, layout, appearance, keys, about)", page));
        open_settings(it->second);
        return answer_(true);
    }
    const auto what = str("keys");
    if (what == "list") {
        Json out{Json::Object{}};
        out.set("ok", Json{true});
        out.set("set", Json{keys.set});
        Json::Array list;
        for (const auto& a : actions()) {
            Json one{Json::Object{}};
            one.set("action", Json{std::string(a.name)});
            one.set("group", Json{std::string(group_label(a.group, false))});
            one.set("what", Json{std::string(a.en)});
            Json::Array chords;
            for (const auto& c : keys.chords(a.action)) chords.push_back(Json{chord_text(c)});
            one.set("keys", Json{chords});
            one.set("changed", Json{key_overrides.contains(std::string(a.name))});
            list.push_back(std::move(one));
        }
        out.set("actions", Json{list});
        return out.dump();
    }
    const auto action = str("action");
    auto known = [&] { return std::ranges::any_of(actions(), [&](const ActionInfo& a) { return a.name == action; }); };
    if (what == "bind") {
        if (!known()) return answer_(false, std::format("no action '{}' (xdesk msg keys list)", action));
        const auto chord = parse_chord(str("chord"));
        if (!chord) return answer_(false, std::format("'{}' is not a chord (as Super+Shift+f)", str("chord")));
        if (!chord->super && !chord->alt && !chord->ctrl) return answer_(false, "a desktop's key is held with Super, Ctrl or Alt");
        const auto taken = rebind(action, *chord);
        Json out{Json::Object{}};
        out.set("ok", Json{true});
        if (!taken.empty()) out.set("taken_from", Json{taken});
        return out.dump();
    }
    if (what == "unbind") {
        if (!known()) return answer_(false, std::format("no action '{}' (xdesk msg keys list)", action));
        key_overrides.erase(action);
        auto all = key_overrides;
        all["set"] = {keys.set};
        keys.load(all);
        save_keys();
        return answer_(true);
    }
    if (what == "set") {
        const auto set = str("set");
        if (std::ranges::find(key_sets(), set) == key_sets().end()) return answer_(false, std::format("no set of keys '{}' (luban, luban-no-hjkl)", set));
        auto all = key_overrides;
        all["set"] = {set};
        keys.load(all);
        save_keys();
        return answer_(true);
    }
    return answer_(false, "asks: {\"keys\": \"list\"|\"bind\"|\"unbind\"|\"set\"}, {\"settings\": PAGE}, {\"state\": true}, "
                          "{\"debug\": \"hud\"}, {\"subscribe\": true}, {\"telemetry\": \"dump\"}");
}

}  // namespace xdesk::compositor
