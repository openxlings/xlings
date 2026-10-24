// The desktop around the windows: title bars, the panel, the launcher, starting programs (server.cppm).
module xdesk.compositor;

import std;
import xdesk.words;
import openbus;
import opensal;
import opensal.wire;
import opensal.keys;
import xdesk.os;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import xdesk.apps;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;
import :keys;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace key = xdesk::native::key;
namespace draw = xdesk::draw;
namespace shell = xdesk::shell;
namespace apps = xdesk::apps;
namespace fs = std::filesystem;

// A window's title bar, drawn again only when what it shows changed.
void Server::draw_title(View& v, int width, bool active, bool rounded) {
    // A free window in the WuXian view folds: its mark, and folded, said (design 2026-10-24 §9.3).
    const int fold = wuxian() && lay.is_free(v.id) ? (lay.collapsed(v.id) ? 2 : 1) : 0;
    const std::string title = fold == 2 ? std::format("{} · {}", name_of(v), i18n::tr("collapsed")) : name_of(v);
    const bool hover = close_hover == v.id, max = max_hover == v.id;
    // Its application's icon (design 2026-10-24 §9.5).
    const auto* icon = v.toplevel->app_id ? icon_of(v.toplevel->app_id, shell::kTitleIcon) : nullptr;
    auto key = std::format("{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}", title, width, active, hover, scale, max, rounded, fold,
                           static_cast<const void*>(icon));
    if (key == v.drawn) return;
    v.drawn = std::move(key);
    show(v.title, shell::title_bar(ui(), theme, title, width, active, hover, scale, max, rounded, fold, icon));
}

// The panel and (canvas) the grid, drawn again only when they changed.
void Server::draw_shell() {
    // The workspaces as the status bar shows them (design 2026-10-20 WuXian
    // §3.13), the windows free on WuXian, the WuXian view's own.
    shell::PanelState st;
    st.workspaces = chips();
    st.others = status_others;
    for (const auto& v : views)
        if (v.mapped && lay.is_free(v.id)) ++st.free_windows;
    if (View* f = focused(); f && (wuxian() || lay.workspace_of(f->id) == lay.current()) && f->toplevel->title) st.title = f->toplevel->title;
    st.clock = clock_text();
    st.zoom = lay.zoom();
    st.wuxian = wuxian();
    st.minimap = minimap_on;
    st.zh = zh;
    std::string key = std::format("{} {} {} {} {} {} {} {}x{}@{}", st.title, st.clock, st.zoom, st.wuxian, st.free_windows, st.minimap, st.others,
                                  size.width, size.height, scale);
    for (const auto& c : st.workspaces)
        key += std::format(" {}:{}:{:06x}:{}:{}:{}:{}:{}", c.id, c.name, c.color, c.stack, c.vertical, c.windows, c.current, c.morph);
    // A window full screen: no panel (nothing of it to click either).
    const bool full = std::ranges::any_of(placements, &layout::Placement::fullscreen);
    wlr_scene_node_set_enabled(&panel_node->node, !full);
    if (full) {
        panel_picture.areas.clear();
        panel_drawn.clear();
    } else if (key != panel_drawn) {
        panel_drawn = std::move(key);
        panel_picture = shell::panel(ui(), theme, st, logical_w(), logical_h(), pill, scale);
        wlr_scene_node_set_position(&panel_node->node, static_cast<int>(panel_picture.x), static_cast<int>(panel_picture.y));
        show(panel_node, std::move(panel_picture.image), scale);
        panel_picture.image = {};
    }
    wlr_scene_node_set_enabled(&wuxian_dots->node, wuxian());
    // The minimap, while the plane holds more than fits in the view.
    {
        const auto m = lay.minimap();
        const bool shown = wuxian() && minimap_on && !m.boxes.empty()
                           && (m.bounds.width > m.view.width + 1 || m.bounds.height > m.view.height + 1);
        wlr_scene_node_set_enabled(&minimap_node->node, shown);
        if (!shown) minimap_shown = {};
        if (shown) {
            std::string key = std::format("{} {} {} {} {}x{}@{}", m.view.x, m.view.y, m.view.width, m.boxes.size(), size.width, size.height, scale);
            for (const auto& b : m.boxes) key += std::format(" {} {} {} {}", b.x, b.y, b.width, b.height);
            if (key != minimap_drawn) {
                minimap_drawn = std::move(key);
                std::vector<draw::Rect> boxes;
                for (const auto& b : m.boxes) boxes.push_back({b.x, b.y, b.width, b.height});
                minimap_shown = shell::minimap(theme, boxes, {m.view.x, m.view.y, m.view.width, m.view.height},
                                               {m.bounds.x, m.bounds.y, m.bounds.width, m.bounds.height}, logical_w(), logical_h(), scale);
                wlr_scene_node_set_position(&minimap_node->node, static_cast<int>(minimap_shown.picture.x), static_cast<int>(minimap_shown.picture.y));
                show(minimap_node, std::move(minimap_shown.picture.image), scale);
                minimap_shown.picture.image = {};
            }
        }
    }
    if (wuxian()) {
        const auto cam = lay.camera();
        auto dots = std::format("{} {} {} {}x{}@{}", cam.at.x, cam.at.y, cam.zoom, size.width, size.height, scale);
        if (dots != dots_drawn) {
            dots_drawn = std::move(dots);
            show(wuxian_dots, shell::canvas_background(theme, logical_w(), logical_h(), cam.at.x, cam.at.y, cam.zoom, scale), scale);
        }
    }
}

std::string Server::clock_text() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    try {
        return std::format("{:%H:%M}", std::chrono::zoned_time{std::chrono::current_zone(), now});
    } catch (...) {
        return std::format("{:%H:%M}", now);    // no time zone database: UTC
    }
}

// ── the launcher ─────────────────────────────────────────────────
void Server::open_launcher() {
    auto env = os::environment();
    std::string lang;
    for (const char* k : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (env.contains(k) && !env.at(k).empty()) { lang = env.at(k); break; }
    auto all = apps::scan(apps::dirs(env), lang);
    // Its own settings are an application among the others (design 2026-10-20 WuXian §5.6);
    // their pages and its actions are found when searched.
    // The desktop's own (design 2026-10-20 §12.5): its settings' pages and
    // its actions, each with its keys -- searched once, learnt.
    struct Page { std::string_view id, en, words; };
    for (const auto& p : {Page{"displays", "Settings: displays", "monitor screen resolution mirror 分辨率 镜像 屏幕"},
                          Page{"layout", "Settings: layout", "tile stack wuxian canvas workspace gap column 平铺 层叠 无限画布 工作区 间隔 列"},
                          Page{"appearance", "Settings: appearance", "accent colour background motion 颜色 背景 动画"},
                          Page{"keys", "Settings: keys", "shortcut keyboard keys 按键 快捷键 键位"},
                          Page{"apps", "Settings: applications", "applications programs logs crash 应用 程序 日志 崩溃"},
                          Page{"about", "Settings: about", "version system about information 版本 系统 信息"}})
        all.push_back({.id = std::format("xdesk:settings:{}", p.id), .name = std::string(i18n::tr(p.en)),
                       .generic = p.id == "keys" ? std::string("Super+/") : std::string("Super+I"), .keywords = std::string(p.words)});
    all.push_back({.id = "xdesk:tour", .name = std::string(i18n::tr("Tour of the desktop")), .keywords = "tour help start learn 上手 入门 教程"});
    for (const auto& a : actions()) {
        const auto chords = keys.chords(a.action);
        all.push_back({.id = std::format("xdesk:action:{}", a.name), .name = std::string(i18n::tr(a.en)),
                       .generic = chords.empty() ? std::string{} : chord_text(chords.front()),
                       .keywords = std::format("{} {}", a.name, a.en)});
    }
    launcher.open(std::move(all));
    publish("launcher", {});
    draw_launcher();
}

void Server::draw_launcher() {
    if (!launcher.is_open()) {
        wlr_scene_node_set_enabled(&launcher_node->node, false);
        return;
    }
    auto pic = launcher.draw(ui(), big_font ? &*big_font : nullptr, theme, logical_w(), logical_h(), zh, scale,
                             [this](const apps::App& a) {
                                 return a.id.starts_with("xdesk:") ? nullptr : icon_named(a.id, a.icon, shell::Launcher::kLauncherIcon);
                             });
    show(launcher_node, std::move(pic.image), scale);
    wlr_scene_node_set_enabled(&launcher_node->node, true);
}

// A terminal with `command` in it (none: a shell), its window titled and
// identified as the application -- not as the terminal (E21). The one the
// settings name, else the system's default terminal; foot when there is
// none declared (what this desktop shipped with).
std::vector<std::string> Server::terminal_argv(const std::vector<std::string>& command, std::string_view title,
                                       std::string_view app_id) {
    // A configured terminal takes the title and the id as its entry says (F19).
    if (!o.terminal.empty()) return apps::in_terminal(apps::configured_terminal(os::environment(), o.terminal), command, title, app_id);
    static const apps::Terminal foot{{"foot"}, std::string{}, std::string{"--title="}, std::string{"--app-id="}};
    const auto t = apps::default_terminal(os::environment());
    if (!t) log("no default terminal (share/xdg-terminals): foot");
    return apps::in_terminal(t ? *t : foot, command, title, app_id);
}

void Server::start_app(const apps::App& app) {
    if (app.id == "xdesk-settings") { open_settings(SettingsPage::displays); return; }
    if (app.id.starts_with("xdesk:settings:")) {
        if (const auto page = settings_page_named(app.id.substr(15))) open_settings(*page);
        return;
    }
    if (app.id == "xdesk:tour") { start_tour(); return; }
    if (app.id.starts_with("xdesk:action:")) {
        const auto name = app.id.substr(13);
        if (const auto it = std::ranges::find(actions(), name, &ActionInfo::name); it != actions().end()) act(it->action);
        return;
    }
    auto argv = apps::argv(app.exec);
    if (argv.empty()) return;
    if (app.terminal) argv = terminal_argv(argv, app.name, app.id);
    launch(argv, app.id, app.name);
}

// A key while the launcher is open: it is the launcher's.
void Server::launcher_key(std::uint32_t code) {
    const xkb_keysym_t* syms = nullptr;
    const int n = xkb_state_key_get_syms(keyboard.xkb_state, code + 8, &syms);
    const xkb_keysym_t sym = n > 0 ? syms[0] : 0;
    if (sym == key::Escape) launcher.close();
    else if (sym == key::Return || sym == key::KP_Enter) { if (auto app = launcher.choose()) start_app(*app); }
    else if (sym == key::Up) launcher.move(-1);
    else if (sym == key::Down || sym == key::Tab) launcher.move(1);
    else if (sym == key::BackSpace) launcher.backspace();
    else {
        std::array<char, 16> utf8{};
        const int len = xkb_state_key_get_utf8(keyboard.xkb_state, code + 8, utf8.data(), utf8.size());
        if (len > 0 && static_cast<unsigned char>(utf8[0]) >= 0x20 && utf8[0] != 0x7f) launcher.type(std::string_view(utf8.data(), static_cast<std::size_t>(len)));
    }
    draw_launcher();
}

// ── programs ─────────────────────────────────────────────────────
void Server::launch(const std::vector<std::string>& argv, std::string id, std::string name) {
    if (argv.empty()) return;
    auto env = os::environment();
    env["WAYLAND_DISPLAY"] = socket;
    env["XDG_SESSION_TYPE"] = "wayland";
    env["GDK_BACKEND"] = "wayland";
    env["QT_QPA_PLATFORM"] = "wayland";
    env["MOZ_ENABLE_WAYLAND"] = "1";
    env.erase("DISPLAY");
    const fs::path home = env.contains("HOME") ? fs::path(env.at("HOME")) : fs::path{};
    // What it says goes to a pipe the apps' reader takes (design 2026-10-24 §6.1).
    const auto out = os::pipe();
    const int pid = os::spawn({.argv = argv, .env = env, .cwd = home,
                               .stdio = {-1, out ? (*out)[1] : -1, out ? (*out)[1] : -1}});
    if (out) os::close((*out)[1]);
    if (id.empty()) id = fs::path(argv[0]).filename().string();
    if (name.empty()) name = id;
    if (pid <= 0) {
        if (out) os::close((*out)[0]);
        log("cannot start " + argv[0]);
        publish("app-exit", {{"id", id}, {"error", "it could not be started"}});
        show_hint(std::format("{}: {}", name, i18n::tr("it could not be started")), 6000);
        return;
    }
    children.push_back(pid);
    // Its log: the state's apps/<id>-<pid>.log, the last five of each kept.
    const fs::path dir = (env.contains("XDG_STATE_HOME") ? fs::path(env.at("XDG_STATE_HOME")) : home / ".local" / "state") / "xdesk" / "apps";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<fs::path> older;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().starts_with(id + "-")) older.push_back(it->path());
    std::ranges::sort(older, [](const fs::path& a, const fs::path& b) {
        std::error_code e;
        return fs::last_write_time(a, e) < fs::last_write_time(b, e);
    });
    while (older.size() >= 5) { fs::remove(older.front(), ec); older.erase(older.begin()); }
    AppRun r;
    r.pid = pid;
    r.id = id;
    r.name = name;
    r.log = dir / std::format("{}-{}.log", id, pid);
    r.log_fd = os::open_append(r.log);
    r.started = r.refilled = std::chrono::steady_clock::now();
    r.out = out ? (*out)[0] : -1;
    {
        std::lock_guard l(apps_m);
        app_runs.push_back(std::move(r));
    }
    publish("app-start", {{"id", id}, {"pid", std::to_string(pid)}, {"log", (dir / std::format("{}-{}.log", id, pid)).string()}});
    // Its log as a stream on the bus: the last mebibyte, then what it says.
    if (bus) {
        (void)bus->offer_stream(std::format("service/xdesk/apps/{}/log", pid),
                                {.kind = openbus::EndpointKind::stream, .interfaces = {"xdesk.log@1"}},
                                [this, pid](xbus::Stream st) {
                                    std::lock_guard l(apps_m);
                                    for (auto& a : app_runs)
                                        if (a.pid == pid) {
                                            if (!a.tail.empty()) (void)st.write(a.tail, std::chrono::seconds(2));
                                            if (a.ended && a.out < 0) st.close();
                                            else a.followers.push_back(st);
                                            return;
                                        }
                                    st.close();
                                });
    }
}

}  // namespace xdesk::compositor
