// The desktop's control socket (server.cppm; design 2026-10-20 §12.3):
// $XDG_RUNTIME_DIR/xdesk-<WAYLAND_DISPLAY>.sock -- the session's runtime
// directory, its user's alone -- one JSON request a line, one answer a line:
//
//   {"keys": "list"}                                  every action: its keys, its group, what it does; the set
//   {"keys": "bind", "action": A, "chord": C}         A answers to C alone (taken from another that had it)
//   {"keys": "unbind", "action": A}                   A back to the set's keys
//   {"keys": "set", "set": "luban"|"luban-no-hjkl"}   the set of keys
//   {"settings": PAGE}                                the settings, at displays|layout|appearance|keys|about
//
// `xdesk msg` is its client; scripts and agents use it as people use the
// settings: the same changes, kept the same way.
module xdesk.compositor;

import std;
import xdesk.os;
import xdesk.config;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;
import :keys;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace config = xdesk::config;
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
                                            if (!os::write_all(c->fd, self->control(line) + "\n")) c->gone = true;
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

std::string Server::control(std::string_view request) {
    auto parsed = Json::parse(request);
    if (!parsed || !parsed->is_object()) return answer_(false, "a request is one JSON object a line");
    const auto& j = *parsed;
    auto str = [&](std::string_view k) { const auto* v = j.get(k); return v ? v->string_or("") : std::string{}; };
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
    return answer_(false, "asks: {\"keys\": \"list\"|\"bind\"|\"unbind\"|\"set\"} or {\"settings\": PAGE}");
}

}  // namespace xdesk::compositor
