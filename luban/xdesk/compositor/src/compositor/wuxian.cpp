// WuXian on the screen (server.cppm; design 2026-10-20 WuXian §3): the
// WuXian view's workspaces -- a plate a little lighter than WuXian, no
// shadow, a border of its colour, a label above its top-left -- the views'
// change (the camera from the workspace's 1:1 to WuXian's, by a spring),
// workspaces moved by their labels and scaled by their corners, their menus
// and WuXian's, removing one that holds windows (asked; "close all" waits
// five seconds), naming one, and the status bar's word on each.
module xdesk.compositor;

import std;
import xdesk.words;
import opensal;
import openwx;
import xdesk.os;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;
import :keys;

namespace xdesk::compositor {

namespace layout = xdesk::layout;
namespace shell = xdesk::shell;
namespace draw = xdesk::draw;
namespace key = xdesk::native::key;

namespace {

constexpr double kHandle = 18;      // a frame's corner that scales it, in screen pixels
constexpr int kCloseWait = 5;       // seconds before "close all" may be pressed
constexpr int kMorphMs = 250;       // a rule's shape becoming the other's

// A colour as a scene rect takes it: premultiplied.
std::array<float, 4> rgba_(std::uint32_t rgb, float a) {
    return {((rgb >> 16) & 0xff) / 255.0f * a, ((rgb >> 8) & 0xff) / 255.0f * a, (rgb & 0xff) / 255.0f * a, a};
}

std::uint32_t lighter_(draw::Color c, double by) {
    auto up = [&](std::uint8_t v) { return static_cast<std::uint32_t>(std::lround(v + (255 - v) * by)); };
    return (up(c.r) << 16) | (up(c.g) << 8) | up(c.b);
}

void place_(wlr_scene_rect* r, double x, double y, double w, double h) {
    wlr_scene_rect_set_size(r, std::max(1, static_cast<int>(std::lround(w))), std::max(1, static_cast<int>(std::lround(h))));
    wlr_scene_node_set_position(&r->node, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
}

std::string named_(int id, const std::string& name) { return name.empty() ? std::to_string(id) : std::format("{}「{}」", id, name); }

}  // namespace

// ── the status bar's and the labels' workspaces ──────────────────────

std::vector<shell::WorkspaceChip> Server::chips() {
    std::vector<shell::WorkspaceChip> out;
    const auto now = std::chrono::steady_clock::now();
    for (const int id : lay.workspaces()) {
        const auto* w = lay.workspace_info(id);
        if (!w) continue;
        shell::WorkspaceChip c{.id = id, .name = w->name, .color = w->color, .stack = w->rule == layout::Rule::stack,
                               .vertical = w->flow == layout::Flow::vertical, .windows = static_cast<int>(lay.windows_in(id).size()),
                               .current = id == lay.current()};
        if (const auto it = rule_changed.find(id); it != rule_changed.end()) {
            const double t = std::chrono::duration<double, std::milli>(now - it->second).count() / kMorphMs;
            if (t >= 1 || reduce_motion) rule_changed.erase(it);
            else c.morph = t;
        }
        out.push_back(std::move(c));
    }
    return out;
}

void Server::set_rule_of(int id, layout::Rule r) {
    const auto* w = lay.workspace_info(id);
    if (!w || w->rule == r) return;
    lay.set_rule(id, r);
    rule_changed[id] = std::chrono::steady_clock::now();
    if (morph_timer) wl_event_source_timer_update(morph_timer, 16);
    remember_arrangement();
    arrange();
}

// The status bar's word on a workspace the pointer rests on (design
// 2026-10-20 WuXian §3.13): its name, its rule, how many windows it has.
void Server::status_hover(double x, double y) {
    const auto a = shell::hit(panel_picture, x, y);
    if (!a || (a->target != shell::Target::workspace && a->target != shell::Target::rule)) {
        hint_for.clear();
        return;
    }
    const auto key = std::format("{}", a->index);
    if (key == hint_for) return;
    hint_for = key;
    const auto* w = lay.workspace_info(a->index);
    if (!w) return;
    const auto n = lay.windows_in(a->index).size();
    const auto rule = w->rule == layout::Rule::stack ? i18n::tr("stack") : i18n::tr("tile");
    show_hint(n == 1 ? i18n::trf("Workspace {} · {} · 1 window", named_(w->id, w->name), rule)
                     : i18n::trf("Workspace {} · {} · {} windows", named_(w->id, w->name), rule, n),
              1500);
}

// ── the WuXian view's workspaces ─────────────────────────────────────

void Server::update_frames(const std::vector<layout::Placement>& ps) {
    (void)ps;
    const bool on = wuxian() || view_move.has_value();
    const auto& frames = lay.frames();
    std::erase_if(ws_nodes, [&](WorkspaceNodes& n) {
        const bool keep = on && std::ranges::any_of(frames, [&](const layout::Frame& f) { return f.id == n.id; });
        if (!keep) {
            wlr_scene_node_destroy(&n.plate->node);
            for (auto* b : n.border) wlr_scene_node_destroy(&b->node);
            wlr_scene_node_destroy(&n.label->node);
        }
        return !keep;
    });
    if (!on) return;
    const auto cs = chips();
    for (const auto& f : frames) {
        auto it = std::ranges::find(ws_nodes, f.id, &WorkspaceNodes::id);
        if (it == ws_nodes.end()) {
            WorkspaceNodes n{.id = f.id};
            const auto c = rgba_(0, 1);
            n.plate = wlr_scene_rect_create(window_layer, 1, 1, c.data());
            for (auto& b : n.border) b = wlr_scene_rect_create(window_layer, 1, 1, c.data());
            n.label = wlr_scene_buffer_create(shell_layer, nullptr);
            ws_nodes.push_back(n);
            it = ws_nodes.end() - 1;
        }
        auto& n = *it;
        n.frame = f.screen;
        const auto* info = lay.workspace_info(f.id);
        const auto chip = std::ranges::find(cs, f.id, &shell::WorkspaceChip::id);
        if (!info || chip == cs.end()) continue;
        // The plate: a little lighter than WuXian, the current one lighter still; no shadow.
        const bool hit = drag == Drag::move && drop.workspace == f.id;
        const auto plate = hit ? rgba_(lighter_(theme.background, 0.04), 1) : rgba_(lighter_(theme.background, f.current ? 0.07 : 0.045), 1);
        wlr_scene_rect_set_color(n.plate, plate.data());
        place_(n.plate, f.screen.x, f.screen.y, f.screen.width, f.screen.height);
        // The border: its colour, brighter for the current one, the accent where a window would land.
        const auto colour = hit ? rgba_((std::uint32_t(theme.accent.r) << 16) | (std::uint32_t(theme.accent.g) << 8) | theme.accent.b, 1)
                                : rgba_(info->color, f.current || ws_selected == f.id ? 1.0f : 0.6f);
        const double b = ws_selected == f.id ? 2.5 : 1.5;
        for (auto* r : n.border) wlr_scene_rect_set_color(r, colour.data());
        place_(n.border[0], f.screen.x - b, f.screen.y - b, f.screen.width + 2 * b, b);
        place_(n.border[1], f.screen.x - b, f.screen.y + f.screen.height, f.screen.width + 2 * b, b);
        place_(n.border[2], f.screen.x - b, f.screen.y, b, f.screen.height);
        place_(n.border[3], f.screen.x + f.screen.width, f.screen.y, b, f.screen.height);
        // The label above its top-left; shown small, the dot and the number.
        const bool small = f.screen.width < 240;
        const auto key = std::format("{} {} {} {:06x} {} {} {} {} {}", chip->id, chip->name, chip->stack, chip->color, chip->current, small,
                                     chip->morph, scale, ws_selected == f.id);
        if (key != n.drawn) {
            n.drawn = key;
            auto pic = shell::workspace_label(ui(), theme, *chip, small, scale);
            n.label_at = {0, 0, pic.image.width() / scale, static_cast<double>(shell::kLabelHeight)};
            show(n.label, std::move(pic.image), scale);
        }
        n.label_at.x = f.screen.x;
        n.label_at.y = f.screen.y - shell::kLabelHeight - 5;
        wlr_scene_node_set_position(&n.label->node, static_cast<int>(std::lround(n.label_at.x)), static_cast<int>(std::lround(n.label_at.y)));
    }
}

// Into the WuXian view, or out of it into `into` (else the workspace of
// the focused window): the camera goes from one to the other by a spring --
// the workspace drawn smaller as it goes out, larger as it comes in.
void Server::change_view(std::optional<int> into) {
    if (view_move) return;
    publish("view", {{"to", wuxian() ? "workspace" : "wuxian"}});
    if (!wuxian()) {
        const auto from = lay.locked_camera();
        lay.set_view(layout::View::wuxian);
        if (reduce_motion) { arrange(); follow_focus(); return; }
        view_move = ViewMove{.from = from, .to = lay.camera(), .t = {.value = 0, .target = 1000}};
    } else {
        const int target = into.value_or(lay.focused_workspace());
        const auto to = lay.locked_camera(target);
        auto leave = [this, target] {
            lay.show(target);
            lay.set_view(layout::View::workspace);
            ws_selected = 0;
        };
        if (reduce_motion) { leave(); arrange(); follow_focus(); return; }
        view_move = ViewMove{.from = lay.camera(), .to = to, .t = {.value = 0, .target = 1000}, .leaving = true};
        // What goes on at the end: the workspace shown, its view.
        lay.show(target);
        lay.look(view_move->from);
    }
    view_step();
}

void Server::view_step() {
    if (!view_move) return;
    auto& m = *view_move;
    m.t.step(1.0 / 60);
    const double t = std::clamp(m.t.value / 1000, 0.0, 1.0);
    const layout::Camera cam{.at = {m.from.at.x + (m.to.at.x - m.from.at.x) * t, m.from.at.y + (m.to.at.y - m.from.at.y) * t},
                             .zoom = std::exp(std::log(m.from.zoom) + (std::log(m.to.zoom) - std::log(m.from.zoom)) * t)};
    placements = lay.arrange(output_size(), reserved(), cam);
    with_extends(placements);
    apply(placements, true);
    if (!m.t.settled()) {
        if (view_timer) wl_event_source_timer_update(view_timer, 16);
        return;
    }
    const bool leaving = m.leaving;
    view_move.reset();
    if (leaving) {
        lay.set_view(layout::View::workspace);
        ws_selected = 0;
    }
    arrange();
    follow_focus();
}

// Another workspace in the workspace view: its windows slide in from the
// side it is on (by number).
void Server::switch_workspace(int id) {
    const int from = lay.current();
    lay.show(id);
    arrange();
    follow_focus();
    if (wuxian() || reduce_motion || from == id || lay.current() != id) return;
    const double dx = (id > from ? 1 : -1) * logical_w() * 0.25;
    for (const auto& p : placements) {
        if (p.workspace != id || p.pinned || p.fullscreen) continue;
        if (View* v = view_of(p.id)) {
            v->x.value = v->x.target + dx;
            v->moving = true;
        }
    }
    animate();
}

// ── the pointer in the WuXian view ───────────────────────────────────

// A button there before the windows' gestures: a workspace's label (moved,
// chosen, its menu; twice: in), its corner (its scale), its plate, WuXian
// itself (panned; its menu). False: the window's, as everywhere.
bool Server::wuxian_button(const opensal::PointerButton& b) {
    if (!wuxian() || view_move) return false;
    const double x = cursor->x, y = cursor->y;
    using B = opensal::Button;
    auto twice = [&](int id) {
        const auto now = std::chrono::steady_clock::now();
        const bool again = title_pressed_id == layout::Id(1000000 + id) && now - title_pressed < std::chrono::milliseconds(400);
        title_pressed_id = again ? 0 : layout::Id(1000000 + id);
        title_pressed = now;
        return again;
    };
    auto choose = [&](int id) {
        ws_selected = id;
        plane_focus = true;
        wlr_seat_keyboard_clear_focus(seat);
        lay.raise(id);
        arrange();
    };
    for (auto it = ws_nodes.rbegin(); it != ws_nodes.rend(); ++it) {
        if (!it->label_at.contains(x, y)) continue;
        if (!b.down) return true;
        if (b.button == B::Right) { open_workspace_menu(it->id, x, y); return true; }
        if (b.button != B::Left) return true;
        if (twice(it->id)) { change_view(it->id); return true; }
        choose(it->id);
        drag = Drag::workspace;
        ws_dragged = it->id;
        ws_drag_origin = lay.workspace_info(it->id)->origin;
        ws_drag_x = x;
        ws_drag_y = y;
        return true;
    }
    for (auto it = ws_nodes.rbegin(); it != ws_nodes.rend(); ++it) {
        if (lay.workspace_collapsed(it->id)) continue;   // a label strip has no corner to scale by
        const auto& f = it->frame;
        const layout::Rect corner{f.x + f.width - kHandle, f.y + f.height - kHandle, kHandle * 1.5, kHandle * 1.5};
        if (!corner.contains(x, y) || b.button != B::Left) continue;
        if (!b.down) return true;
        choose(it->id);
        drag = Drag::scale;
        ws_dragged = it->id;
        ws_drag_scale = lay.workspace_info(it->id)->scale;
        ws_drag_x = x;
        ws_drag_y = y;
        return true;
    }
    if (view_at(x, y)) return false;
    // A workspace's plate where it has no window, or WuXian.
    const auto ws = lay.workspace_at(x, y);
    if (!b.down) return false;
    if (b.button == B::Right) {
        if (ws) open_workspace_menu(*ws, x, y);
        else open_plane_menu(x, y);
        return true;
    }
    if (b.button == B::Left && ws && twice(*ws)) {
        // A collapsed one's strip: expanded; else into it.
        if (lay.workspace_collapsed(*ws)) collapse_workspace(*ws);
        else change_view(*ws);
        return true;
    }
    if (b.button == B::Left || b.button == B::Middle) {
        if (ws) choose(*ws);
        else {
            ws_selected = 0;
            plane_focus = true;
            wlr_seat_keyboard_clear_focus(seat);
            arrange();
        }
        drag = Drag::pan;
        drag_x = x;
        drag_y = y;
        return true;
    }
    return false;
}

void Server::workspace_motion() {
    const auto* w = lay.workspace_info(ws_dragged);
    if (!w) { drag = Drag::none; return; }
    if (drag == Drag::workspace) {
        pointer_shape("grabbing");
        const double z = lay.zoom();
        lay.move_workspace(ws_dragged, {ws_drag_origin.x + (cursor->x - ws_drag_x) / z, ws_drag_origin.y + (cursor->y - ws_drag_y) / z});
    } else {
        // Its scale, its top-left where it is: as far as the corner is dragged.
        pointer_shape("se-resize");
        const auto f = std::ranges::find(ws_nodes, ws_dragged, &WorkspaceNodes::id);
        if (f == ws_nodes.end()) return;
        const double from = std::max(20.0, ws_drag_x - f->frame.x), now = std::max(20.0, cursor->x - f->frame.x);
        lay.set_scale(ws_dragged, ws_drag_scale * now / from);
    }
    arrange(true);
}

// ── menus ────────────────────────────────────────────────────────────

void Server::open_workspace_menu(int id, double x, double y) {
    close_overlay(false);
    menu_of = MenuOf::workspace;
    menu_workspace = id;
    overlay = Overlay::menu;
    overlay_picture.x = x;
    overlay_picture.y = y;
    draw_overlay();
}

void Server::open_plane_menu(double x, double y) {
    close_overlay(false);
    menu_of = MenuOf::plane;
    const auto area = layout::Rect{0, reserved().top, double(logical_w()), logical_h() - reserved().top};
    menu_world = openwx::to_world(lay.camera(), {x, y}, {area.x, area.y});
    overlay = Overlay::menu;
    overlay_picture.x = x;
    overlay_picture.y = y;
    draw_overlay();
}

void Server::open_desktop_menu(double x, double y) {
    close_overlay(false);
    menu_of = MenuOf::desktop;
    overlay = Overlay::menu;
    overlay_picture.x = x;
    overlay_picture.y = y;
    draw_overlay();
}

bool Server::workspace_action(const std::string& what) {
    const int id = menu_workspace;
    if (what == "enter") change_view(id);
    else if (what == "rename") { start_naming(id); return true; }
    else if (what == "rule:tile") set_rule_of(id, layout::Rule::tile);
    else if (what == "rule:stack") set_rule_of(id, layout::Rule::stack);
    else if (what == "color") {
        // The next of the colours, by the one it has.
        if (const auto* w = lay.workspace_info(id)) {
            int k = 1;
            for (; k <= 9; ++k)
                if (openwx::color_for(k) == w->color) break;
            lay.set_color(id, openwx::color_for(k % 9 + 1));
        }
    } else if (what == "scale:0.5") lay.set_scale(id, 0.5);
    else if (what == "scale:1") lay.set_scale(id, 1);
    else if (what == "collapse") { collapse_workspace(id); return true; }
    else if (what.starts_with("bm:")) {
        // A covered item from a bookmark's "+N" list: bm:w:<window> or bm:s:<workspace>.
        const auto n = std::strtoull(what.c_str() + 5, nullptr, 10);
        if (what[3] == 'w') bookmark_choose(n, 0);
        else bookmark_choose(0, static_cast<int>(n));
        return true;
    }
    else if (what == "delete") { ask_remove(id); return true; }
    else if (what == "new:tile" || what == "new:stack") {
        const auto rule = what == "new:stack" ? layout::Rule::stack : layout::Rule::tile;
        // Its label's top-left where the menu was opened.
        const int made = lay.add_workspace(rule, std::nullopt, layout::Point{menu_world.x, menu_world.y + shell::kLabelHeight / lay.zoom()});
        if (made == 0) show_hint(i18n::tr("Nine workspaces are as many as there are"), 3000);
        else ws_selected = made;
    } else if (what == "tidy") lay.line_up();
    else if (what == "show-all") lay.show_all();
    else if (what == "terminal") { launch(terminal_argv({})); return true; }
    else if (what == "settings") { open_settings(SettingsPage::displays); return true; }
    else if (what == "wuxian") { change_view(); return true; }
    else return false;
    remember_arrangement();
    arrange();
    return true;
}

// ── removing a workspace (design 2026-10-20 WuXian §3.9) ─────────────

void Server::ask_remove(int id) {
    if (!lay.workspace_info(id)) return;
    if (lay.workspaces().size() <= 1) {
        show_hint(i18n::tr("The last workspace stays"), 3000);
        return;
    }
    const auto n = lay.windows_in(id).size();
    if (n == 0) {
        lay.remove_workspace(id);
        if (ws_selected == id) ws_selected = 0;
        remember_arrangement();
        arrange();
        return;
    }
    close_overlay(false);
    removal = Removal{.id = id, .since = std::chrono::steady_clock::now(), .windows = static_cast<int>(n)};
    overlay = Overlay::dialog;
    if (removal_timer) wl_event_source_timer_update(removal_timer, 250);
    draw_overlay();
}

void Server::draw_dialog() {
    if (asking_reset) {
        overlay_picture = shell::dialog(ui(), big_font ? &*big_font : nullptr, theme, i18n::tr("Everything as it came?"),
                                        i18n::tr("The layout's, the switchers', the wheel's and the look's settings, and every key changed"),
                                        {{.label = std::string(i18n::tr("Put everything back (Enter)")), .primary = true},
                                         {.label = std::string(i18n::tr("Cancel (Esc)"))}},
                                        logical_w(), logical_h(), scale);
        return;
    }
    if (!removal) return;
    const auto* w = lay.workspace_info(removal->id);
    if (!w) { close_overlay(false); return; }
    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - removal->since).count();
    const int left = std::max(0, kCloseWait - static_cast<int>(waited));
    const auto name = named_(w->id, w->name);
    const auto title = i18n::trf("Remove workspace {}?", name);
    const auto text = removal->windows == 1 ? i18n::trf("Workspace {} holds 1 window", name)
                                            : i18n::trf("Workspace {} holds {} windows", name, removal->windows);
    std::vector<shell::DialogButton> buttons{
        {.label = std::string(i18n::tr("Make them free windows (Enter)")), .primary = true},
        {.label = left > 0 ? i18n::trf("Close them all ({})", left)
                           : std::string(i18n::tr("Close them all")),
         .enabled = left == 0, .danger = true},
        {.label = std::string(i18n::tr("Cancel (Esc)"))},
    };
    overlay_picture = shell::dialog(ui(), big_font ? &*big_font : nullptr, theme, title, text, buttons, logical_w(), logical_h(), scale);
}

bool Server::dialog_key(xkb_keysym_t s) {
    if (asking_reset) {
        if (s == key::Return || s == key::KP_Enter) { close_overlay(false); reset_all(); open_settings(SettingsPage::about); }
        else if (s == key::Escape) { close_overlay(false); open_settings(SettingsPage::about); }
        return true;
    }
    if (!removal) { close_overlay(false); return true; }
    if (s == key::Escape) close_overlay(false);
    else if (s == key::Return || s == key::KP_Enter) remove_with(removal->id, false);
    return true;
}

bool Server::dialog_click(double x, double y) {
    if (asking_reset) {
        const auto a = shell::hit(overlay_picture, x, y);
        close_overlay(false);
        if (a && a->index == 0) reset_all();
        open_settings(SettingsPage::about);
        return true;
    }
    if (const auto a = shell::hit(overlay_picture, x, y); a && removal) {
        if (a->index == 0) remove_with(removal->id, false);
        else if (a->index == 1) remove_with(removal->id, true);
        else close_overlay(false);
    }
    return true;
}

// Removed: its windows free on WuXian where they were -- or asked to close;
// one that has not closed in ten seconds (it asks to save) stays free.
void Server::remove_with(int id, bool close_all) {
    const auto ws = lay.windows_in(id);
    removal.reset();
    close_overlay(false);
    lay.remove_workspace(id);
    if (ws_selected == id) ws_selected = 0;
    if (close_all) {
        closing = ws;
        for (const auto wid : ws)
            if (View* v = view_of(wid)) wlr_xdg_toplevel_send_close(v->toplevel);
        if (closing_timer) wl_event_source_timer_update(closing_timer, 10000);
    }
    remember_arrangement();
    arrange();
    follow_focus();
}

// ── naming a workspace ───────────────────────────────────────────────

void Server::start_naming(int id) {
    const auto* w = lay.workspace_info(id);
    if (!w) return;
    close_overlay(false);
    naming = id;
    name_text = w->name;
    overlay = Overlay::naming;
    draw_overlay();
}

void Server::draw_naming() {
    const auto n = std::ranges::find(ws_nodes, naming, &WorkspaceNodes::id);
    const double x = n != ws_nodes.end() ? n->label_at.x : logical_w() / 2.0 - 160;
    const double y = n != ws_nodes.end() ? n->label_at.y + shell::kLabelHeight + 4 : logical_h() / 2.0 - 32;
    overlay_picture = shell::text_field(ui(), theme, i18n::trf("Workspace {}'s name (Enter: keep, Esc: cancel)", naming),
                                        name_text, x, y, logical_w(), logical_h(), scale);
}

bool Server::naming_key(xkb_keysym_t s, std::uint32_t code) {
    if (s == key::Escape) { close_overlay(false); return true; }
    if (s == key::Return || s == key::KP_Enter) {
        lay.rename(naming, name_text);
        close_overlay(false);
        remember_arrangement();
        arrange();
        return true;
    }
    if (s == key::BackSpace) {
        if (!name_text.empty()) {
            do name_text.pop_back();
            while (!name_text.empty() && (static_cast<unsigned char>(name_text.back()) & 0xc0) == 0x80);
        }
        draw_overlay();
        return true;
    }
    std::array<char, 16> utf8{};
    const int len = xkb_state_key_get_utf8(keyboard.xkb_state, code + 8, utf8.data(), utf8.size());
    if (len > 0 && static_cast<unsigned char>(utf8[0]) >= 0x20 && utf8[0] != 0x7f) naming_type(std::string_view(utf8.data(), static_cast<std::size_t>(len)));
    return true;
}

void Server::naming_type(std::string_view text) {
    if (name_text.size() + text.size() > 64) return;
    name_text += text;
    draw_overlay();
}

// ── the minimap (design 2026-10-21 §8.9) ─────────────────────────────

// Pressed on the minimap: a workspace or a free window there, else the view's
// box, else the plane (the view goes there).
void Server::minimap_press() {
    const auto m = lay.minimap();
    const auto w = minimap_world(cursor->x, cursor->y);
    MinimapDrag d{.from = w, .press_x = cursor->x, .press_y = cursor->y, .per_pixel = minimap_shown.per_pixel};
    // The view's box by its edge (3 map pixels): the camera; inside it, what is there.
    const double pp = minimap_shown.per_pixel, reach = 3 * pp;
    const layout::Rect outer{m.view.x - reach, m.view.y - reach, m.view.width + 2 * reach, m.view.height + 2 * reach};
    const layout::Rect inner{m.view.x + reach, m.view.y + reach, m.view.width - 2 * reach, m.view.height - 2 * reach};
    const bool on_edge = outer.contains(w.x, w.y) && !(inner.width > 0 && inner.height > 0 && inner.contains(w.x, w.y));
    if (on_edge) {
        d.what = MinimapDrag::What::view;
        d.origin = lay.camera().at;
    } else if (const auto hit = openwx::hit(m.layers, w)) {
        if (hit->kind == openwx::Layer::Kind::window) {
            d.what = MinimapDrag::What::window;
            d.window = hit->window;
            d.origin = {hit->world.x, hit->world.y};
        } else if (const auto* info = lay.workspace_info(hit->workspace)) {
            d.what = MinimapDrag::What::workspace;
            d.workspace = hit->workspace;
            d.origin = info->origin;
        }
    } else if (m.view.contains(w.x, w.y)) {
        d.what = MinimapDrag::What::view;
        d.origin = lay.camera().at;
    } else {
        lay.look_at(w.x, w.y);
        arrange();
        return;
    }
    minimap_drag = d;
    drag = Drag::minimap;
}

void Server::minimap_motion() {
    if (!minimap_drag) { drag = Drag::none; return; }
    auto& d = *minimap_drag;
    // In the map's pixels as it was pressed: what moves changes the map's scale.
    const double dx = (cursor->x - d.press_x) * d.per_pixel, dy = (cursor->y - d.press_y) * d.per_pixel;
    // A few pixels of the map before it is a drag, not a click.
    if (!d.moved && std::hypot(cursor->x - d.press_x, cursor->y - d.press_y) < 3) return;
    d.moved = true;
    pointer_shape("grabbing");
    using W = MinimapDrag::What;
    view_moved();
    if (d.what == W::view) lay.look({.at = {d.origin.x + dx, d.origin.y + dy}, .zoom = lay.camera().zoom});
    else if (d.what == W::workspace) lay.move_workspace(d.workspace, {d.origin.x + dx, d.origin.y + dy});
    else lay.move_free(d.window, {d.origin.x + dx, d.origin.y + dy});
    arrange(true);
}

void Server::minimap_release() {
    if (!minimap_drag) return;
    const auto d = *minimap_drag;
    minimap_drag.reset();
    using W = MinimapDrag::What;
    const auto m = lay.minimap();
    if (!d.moved) {
        // A click: that one focused and in the middle of the view, the zoom kept.
        if (d.what == W::view) lay.look_at(d.from.x, d.from.y);
        for (const auto& l : m.layers) {
            const bool it = (d.what == W::window && l.kind == openwx::Layer::Kind::window && l.window == d.window)
                            || (d.what == W::workspace && l.kind == openwx::Layer::Kind::workspace && l.workspace == d.workspace);
            if (it) lay.look_at(l.world.x + l.world.width / 2, l.world.y + l.world.height / 2);
        }
        if (d.what == W::workspace) {
            ws_selected = d.workspace;
            lay.raise(d.workspace);
        }
        if (d.what == W::window)
            if (View* v = view_of(d.window)) focus(v);
        arrange();
        return;
    }
    // A free window let go over a workspace: into it (openwx §3.5).
    if (d.what == W::window) {
        const auto w = minimap_world(cursor->x, cursor->y);
        std::vector<openwx::Layer> others;
        for (const auto& l : m.layers)
            if (!(l.kind == openwx::Layer::Kind::window && l.window == d.window)) others.push_back(l);
        if (const auto hit = openwx::hit(others, w); hit && hit->kind == openwx::Layer::Kind::workspace) lay.send_to(d.window, hit->workspace);
    }
    remember_arrangement();
    arrange();
}

}  // namespace xdesk::compositor
