// The desktop's tour (server.cppm; design 2026-10-21 §8.1): ten steps, each
// done by doing it -- the compositor sees what happens (the same events
// `xdesk msg subscribe` tells) and goes on; each may be skipped, the whole
// ended. Given once, the first session (state.json's `toured`); again from
// the launcher ("Tour of the desktop") or `xdesk msg tour start`.
module xdesk.compositor;

import std;
import xdesk.words;
import xdesk.shell;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;

namespace xdesk::compositor {

namespace shell = xdesk::shell;

namespace {

struct Step {
    std::string_view title, text;
    // What finishes it: an event and, when it says, the field that must match (a prefix).
    std::vector<std::tuple<std::string_view, std::string_view, std::string_view>> when;
};

const std::vector<Step>& steps_() {
    static const std::vector<Step> s{
        {"Your applications", "Press Super alone: the applications, and everything the desktop does, by name",
         {{"launcher", "", ""}}},
        {"A terminal", "Super+Enter opens one", {{"action", "name", "terminal"}, {"open", "", ""}}},
        {"Switching", "Super+Tab goes by application, Alt+Tab by window",
         {{"action", "name", "switch-apps"}, {"action", "name", "windows"}}},
        {"Workspaces", "Super+2 goes to workspace 2 (made when there is none), Super+1 back", {{"action", "name", "workspace-"}}},
        {"The whole of it: WuXian", "Super+O shows every workspace on one plane, and back", {{"view", "to", "wuxian"}}},
        {"Moving between them", "In the WuXian view drag a window by its title into another workspace, or onto the plane",
         {{"drop", "", ""}}},
        {"Nothing gets lost", "Something entirely covered has a bookmark on what covers it: click one (skip it when nothing is)",
         {{"bookmark", "", ""}}},
        {"Collapsing", "Super+S folds a free window to its title bar, a workspace to its label", {{"collapse", "", ""}}},
        {"Every key", "Super+/ lists every key, and lets you change them", {{"action", "name", "keys"}}},
        {"Settings", "Super+I: the desktop's settings. That is all: you know your way", {{"action", "name", "settings"}}},
    };
    return s;
}

}  // namespace

int Server::tour_steps() const { return static_cast<int>(steps_().size()); }

void Server::start_tour() {
    tour_at = 0;
    publish("tour", {{"step", "0"}});
    draw_tour();
}

void Server::end_tour(bool done) {
    if (tour_at < 0) return;
    tour_at = -1;
    if (tour_node) wlr_scene_node_set_enabled(&tour_node->node, false);
    state.toured = true;
    state.hinted = true;
    save_state();
    publish("tour", {{"ended", done ? "done" : "ended"}});
    // Ended before its end: the keys in one line, as before there was a tour.
    if (!done)
        show_hint(i18n::tr("Super: apps  ·  Super+Tab: switch  ·  Super+Enter: a terminal  ·  Super+M: arrangements  ·  Super+/: every key, and changing them"), 10000);
}

void Server::tour_next() {
    if (tour_at < 0) return;
    if (++tour_at >= tour_steps()) { end_tour(true); return; }
    publish("tour", {{"step", std::to_string(tour_at)}});
    draw_tour();
}

void Server::tour_event(std::string_view event, const std::vector<std::pair<std::string, std::string>>& fields) {
    if (tour_at < 0 || tour_at >= tour_steps() || event == "tour") return;
    for (const auto& [what, key, value] : steps_()[static_cast<std::size_t>(tour_at)].when) {
        if (what != event) continue;
        if (!key.empty()) {
            const auto it = std::ranges::find(fields, std::string(key), &std::pair<std::string, std::string>::first);
            if (it == fields.end() || !it->second.starts_with(value)) continue;
        }
        tour_next();
        return;
    }
}

void Server::draw_tour() {
    if (tour_at < 0) return;
    if (!tour_node) tour_node = wlr_scene_buffer_create(overlay_layer, nullptr);
    const auto& s = steps_()[static_cast<std::size_t>(tour_at)];
    // Above the minimap where it is shown.
    const int room = logical_h() - (minimap_node && minimap_node->node.enabled ? 140 : 0);
    tour_picture = shell::tour(ui(), big_font ? &*big_font : nullptr, theme, tour_at, tour_steps(), i18n::tr(s.title), i18n::tr(s.text),
                               i18n::tr("Skip this step"), i18n::tr("End the tour"), logical_w(), room, scale);
    wlr_scene_node_set_position(&tour_node->node, static_cast<int>(tour_picture.x), static_cast<int>(tour_picture.y));
    show(tour_node, std::move(tour_picture.image), scale);
    tour_picture.image = {};
    wlr_scene_node_set_enabled(&tour_node->node, true);
    wlr_scene_node_raise_to_top(&tour_node->node);
}

bool Server::tour_click(double x, double y) {
    if (tour_at < 0 || !tour_node || !tour_node->node.enabled) return false;
    for (const auto& a : tour_picture.areas)
        if (x >= a.rect.x && y >= a.rect.y && x < a.rect.x + a.rect.width && y < a.rect.y + a.rect.height) {
            if (a.index == 0) tour_next();
            else end_tour(false);
            return true;
        }
    return tour_picture.areas.empty() ? false
                                      : x >= tour_picture.x && y >= tour_picture.y && x < tour_picture.x + 380 && y < tour_picture.y + 132;
}

}  // namespace xdesk::compositor
