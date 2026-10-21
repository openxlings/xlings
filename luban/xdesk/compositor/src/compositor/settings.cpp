// The settings (server.cppm; design 2026-10-20 §11.4, §12.4): one place for
// what the desktop lets a person choose -- the displays, the layout, the
// look, the keys, and what this system is. Drawn by xdesk.shell over the
// desktop (Super+I, Super+/ for the keys, the launcher); every change takes
// effect at once and is kept where the title bar's menu and the keys keep
// theirs: the person's settings file (xdesk.config), keys.json.
module xdesk.compositor;

import std;
import opensal;
import opensal.wire;
import xdesk.os;
import xdesk.layout;
import xdesk.draw;
import xdesk.shell;
import xdesk.apps;
import xdesk.config;
import wlroots;
import xdesk.native.wayland;
import freedesktop.wayland.server;
import :server;
import :keys;

namespace xdesk::compositor {

namespace os = xdesk::os;
namespace layout = xdesk::layout;
namespace key = xdesk::native::key;
namespace shell = xdesk::shell;
namespace config = xdesk::config;
namespace fs = std::filesystem;

namespace {

std::string_view tr_(bool zh, std::string_view en, std::string_view cn) { return zh ? cn : en; }

constexpr std::array<SettingsPage, 5> kPages{SettingsPage::displays, SettingsPage::layout, SettingsPage::appearance,
                                              SettingsPage::keys, SettingsPage::about};

std::string_view page_label_(SettingsPage p, bool zh) {
    switch (p) {
        case SettingsPage::displays: return tr_(zh, "Displays", "显示器");
        case SettingsPage::layout: return tr_(zh, "Layout", "布局");
        case SettingsPage::appearance: return tr_(zh, "Appearance", "外观");
        case SettingsPage::keys: return tr_(zh, "Keys", "快捷键");
        case SettingsPage::about: return tr_(zh, "About", "关于");
    }
    return "";
}

// The first line of a file, or "".
std::string first_line_(const fs::path& f) {
    std::ifstream in(f);
    std::string line;
    std::getline(in, line);
    return line;
}

// key=value of an os-release, unquoted.
std::string release_value_(std::string_view key) {
    std::ifstream in("/etc/os-release");
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with(std::string(key) + "=")) continue;
        auto v = line.substr(key.size() + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

// The chord as keys.json keeps it (what parse_chord reads back).
std::string chord_spec_(const Chord& c) {
    std::string out;
    if (c.super) out += "Super+";
    if (c.ctrl) out += "Ctrl+";
    if (c.alt) out += "Alt+";
    if (c.shift) out += "Shift+";
    if (c.digit) return out + std::to_string(c.digit % 10);
    std::array<char, 64> name{};
    xkb_keysym_get_name(c.sym, name.data(), name.size());
    return out + name.data();
}

// The accents offered, and the backgrounds.
struct Swatch { std::string_view en, zh; std::uint32_t rgb; };
constexpr std::array<Swatch, 6> kAccents{{{"Blue", "蓝", 0x5e9cff}, {"Teal", "青", 0x3fbfb0}, {"Green", "绿", 0x6cc070},
                                          {"Orange", "橙", 0xf0a050}, {"Pink", "粉", 0xe07ab0}, {"Purple", "紫", 0xa58cf0}}};
constexpr std::array<Swatch, 4> kBackgrounds{{{"Night", "夜", 0x1b1e25}, {"Black", "黑", 0x0e0f12}, {"Slate", "石板", 0x2a3140},
                                              {"Forest", "林", 0x1d2a24}}};

}  // namespace

void Server::open_settings(SettingsPage page) {
    close_overlay(false);
    if (launcher.is_open()) { launcher.close(); draw_launcher(); }
    settings_page = page;
    settings_row = 0;
    settings_capture = false;
    settings_search.clear();
    settings_preedit.clear();
    settings_hover.reset();
    settings_keyboard = false;
    if (caret_timer) wl_event_source_timer_update(caret_timer, 530);
    overlay = Overlay::settings;
    draw_overlay();
}

// What is changed, kept: the person's settings file of the profile.
void Server::setting(std::string_view section, std::string_view key, std::string value) {
    if (auto r = config::set_value(o.config_profile, os::environment(), section, key, value); !r) {
        log("settings: " + r.error());
        show_hint(std::format("{}: {}", tr_(zh, "Not kept", "未能保存"), r.error()), 5000);
    }
}

namespace {

// The keys each page sets (what its "back to the defaults" removes).
std::vector<std::pair<std::string, std::string>> page_keys_(SettingsPage p) {
    switch (p) {
        case SettingsPage::layout:
            return {{"desktop", "inner_gap"}, {"desktop", "gap"}, {"desktop", "outer_gap"}, {"desktop", "min_column_width"}, {"desktop", "flow"},
                    {"desktop", "rule"}, {"desktop", "status_others"}, {"wuxian", "minimap"}, {"wuxian", "bookmarks"}, {"wuxian", "snap"}, {"canvas", "minimap"},
                    {"canvas", "snap"}, {"switchers", "hold_ms"}, {"switchers", "highlight_ms"}, {"switchers", "dim"}, {"scroll", "speed"},
                    {"scroll", "snap"}};
        case SettingsPage::appearance:
            return {{"theme", "accent"}, {"theme", "background"}, {"appearance", "reduce_motion"}, {"appearance", "renderer"},
                    {"appearance", "pointer_locate"}, {"appearance", "pointer_max"}};
        default: return {};
    }
}

// [appearance] pointer_locate (off, grow, grow-color) and pointer_max (2 to 4).
opensal::PointerLocate pointer_locate_(const config::Settings& s) {
    opensal::PointerLocate p;
    if (const auto m = s.get("appearance", "pointer_locate", "grow-color"); m == "off") p.mode = opensal::PointerLocate::Mode::off;
    else if (m == "grow") p.mode = opensal::PointerLocate::Mode::grow;
    p.max = std::clamp(s.get_int("appearance", "pointer_max", 4), 2, 4);
    return p;
}

}  // namespace

// The settings as the files now say them, taken at once (after a page went
// back to its defaults, or that was undone).
void Server::apply_settings() {
    const auto s = config::load(o.config_profile, os::environment());
    lay.set_spacing(s.get_int("desktop", "inner_gap", s.get_int("desktop", "gap", 4)), s.get_int("desktop", "outer_gap", 0),
                    s.get_int("desktop", "min_column_width", 480));
    minimap_on = s.get("wuxian", "minimap", s.get("canvas", "minimap", "on")) != "off";
    bookmarks_on = s.get("wuxian", "bookmarks", "on") != "off";
    snap_on = s.get("wuxian", "snap", s.get("canvas", "snap", "on")) != "off";
    reduce_motion = s.get("appearance", "reduce_motion", "off") == "on";
    status_others = s.get("desktop", "status_others", "on") != "off";
    hold_ms = std::clamp(s.get_int("switchers", "hold_ms", 500), 100, 3000);
    highlight_ms = std::clamp(s.get_int("switchers", "highlight_ms", 500), 0, 5000);
    if (const auto d = s.get("switchers", "dim", "0.5"); !d.empty()) dim_level = std::clamp(std::atof(d.c_str()), 0.1, 1.0);
    const auto speed = s.get("scroll", "speed", "medium");
    wheel_factor = speed == "slow" ? 0.6 : speed == "fast" ? 1.6 : 1.0;
    snap_columns = s.get("scroll", "snap", "on") != "off";
    locate = pointer_locate_(s);
    const auto base = shell::theme(o.profile);
    set_accent(s.colour("theme", "accent", (std::uint32_t(base.accent.r) << 16) | (std::uint32_t(base.accent.g) << 8) | base.accent.b));
    theme.background = draw::Color::rgb(s.colour("theme", "background",
                                                 (std::uint32_t(base.background.r) << 16) | (std::uint32_t(base.background.g) << 8) | base.background.b));
    const float bg[4] = {theme.background.r / 255.0f, theme.background.g / 255.0f, theme.background.b / 255.0f, 1.0f};
    wlr_scene_rect_set_color(background, bg);
    dots_drawn.clear();
    panel_drawn.clear();
    arrange();
}

void Server::reset_page(SettingsPage page) {
    auto removed = config::unset_values(o.config_profile, os::environment(), page_keys_(page));
    if (!removed) {
        show_hint(std::format("{}: {}", tr_(zh, "Not put back", "未能恢复"), removed.error()), 5000);
        return;
    }
    reset_undo = std::move(*removed);
    reset_at = std::chrono::steady_clock::now();
    apply_settings();
    show_hint(tr_(zh, "This page is as it came  ·  Ctrl+Z: undo (10 s)", "已恢复本页默认  ·  Ctrl+Z 撤销（10 秒内）"), 10000);
}

// Every page and every key back to the defaults (asked first, the About page).
void Server::reset_all() {
    std::vector<std::pair<std::string, std::string>> all;
    for (const auto p : {SettingsPage::layout, SettingsPage::appearance})
        for (auto& k : page_keys_(p)) all.push_back(std::move(k));
    if (auto removed = config::unset_values(o.config_profile, os::environment(), all)) reset_undo = std::move(*removed);
    reset_at = std::chrono::steady_clock::now();
    key_overrides.clear();
    const auto set = keys.set;
    keys.load({{"set", {set}}});
    save_keys();
    apply_settings();
    show_hint(tr_(zh, "Every setting and key is as it came", "全部设置与快捷键已恢复默认"), 5000);
}

void Server::undo_reset() {
    if (reset_undo.empty() || std::chrono::steady_clock::now() - reset_at > std::chrono::seconds(10)) return;
    for (const auto& s : reset_undo) setting(s.section, s.key, s.value);
    reset_undo.clear();
    apply_settings();
    show_hint(tr_(zh, "Put back as it was", "已撤销"), 2000);
}

void Server::save_keys() {
    auto all = key_overrides;
    if (keys.set != "luban") all["set"] = {keys.set};
    else all.erase("set");
    if (auto r = config::write_keys(os::environment(), all); !r) {
        log("keys: " + r.error());
        show_hint(std::format("{}: {}", tr_(zh, "Not kept", "未能保存"), r.error()), 5000);
    }
}

void Server::set_accent(std::uint32_t rgb) {
    theme.accent = theme.selected = draw::Color::rgb(rgb);
    for (auto& v : views) v.drawn.clear();
    panel_drawn.clear();
    const float c[4] = {theme.accent.r / 255.0f * 0.8f, theme.accent.g / 255.0f * 0.8f, theme.accent.b / 255.0f * 0.8f, 0.8f};
    wlr_scene_rect_set_color(guide_x, c);
    wlr_scene_rect_set_color(guide_y, c);
    send_locate();
    arrange();
}

// What this system is, as the About page says it and "Copy" copies it.
std::string Server::system_info() {
    std::string out;
    auto line = [&](std::string_view k, std::string_view v) { if (!v.empty()) out += std::format("{}: {}\n", k, v); };
    line("Luban OS", release_value_("PRETTY_NAME"));
    line("System", release_value_("VERSION_ID"));
    std::error_code ec;
    // A machine's kernel is started for it, with its name (H17: a proxy's file said nothing of it).
    const bool machine = first_line_("/proc/cmdline").find("luban.name=") != std::string::npos;
    line("Runs as", machine ? "a machine (its own kernel)" : "a sandbox (this computer's kernel)");
    std::string xdesk = "a development build";
    if (const auto exe = os::executable(); !exe.empty())
        if (const auto prov = first_line_(exe.parent_path().parent_path() / "README.provenance"); prov.starts_with("xdesk "))
            xdesk = prov.substr(6, prov.find(' ', 6) - 6);
    line("xdesk", xdesk);
    line("Kernel", first_line_("/proc/sys/kernel/osrelease"));
    const auto env = os::environment();
    line("User", env.contains("USER") ? env.at("USER") : "");
    line("Renderer", renderer_text);
    std::string displays;
    for (const auto& l : links)
        displays += std::format("{}{} {}", displays.empty() ? "" : ", ", l.number, &l == &links.front() ? "primary" : "mirror");
    line("Displays", std::format("{} ({}x{} @{})", displays, size.width, size.height, scale));
    {
        std::ifstream in("/proc/cpuinfo");
        std::string model;
        int cpus = 0;
        for (std::string l; std::getline(in, l);) {
            if (l.starts_with("processor")) ++cpus;
            if (model.empty() && (l.starts_with("model name") || l.starts_with("Model"))) model = l.substr(l.find(':') + 2);
        }
        line("CPU", std::format("{}{}", cpus, model.empty() ? std::string{} : " × " + model));
    }
    {
        std::ifstream in("/proc/meminfo");
        std::string k;
        long kb = 0;
        if (in >> k >> kb) line("Memory", std::format("{:.1f} GiB", kb / 1048576.0));
    }
    return out;
}

// The rows of the page shown, and what each does.
void Server::settings_rows() {
    settings_items.clear();
    using K = shell::SettingsRow::Kind;
    auto heading = [&](std::string_view label) { settings_items.push_back({.row = {.kind = K::heading, .label = std::string(label)}}); };
    auto text = [&](std::string label, std::string value = {}) {
        settings_items.push_back({.row = {.kind = K::text, .label = std::move(label), .value = std::move(value)}});
    };
    auto choice = [&](std::string label, std::vector<std::string> choices, std::size_t chosen, std::function<void(std::size_t)> set) {
        const auto n = choices.size();
        settings_items.push_back({.row = {.kind = K::choice, .label = std::move(label), .choices = std::move(choices), .chosen = chosen},
                                  .act = [set = std::move(set), chosen, n](int dir) {
                                      if (n == 0) return;
                                      set((chosen + n + static_cast<std::size_t>(dir == 0 ? 1 : dir)) % n);
                                  }});
    };
    auto toggle = [&](std::string label, bool on, std::function<void(bool)> set) {
        settings_items.push_back({.row = {.kind = K::toggle, .label = std::move(label), .on = on},
                                  .act = [set = std::move(set), on](int) { set(!on); }});
    };
    auto button = [&](std::string label, std::string note, std::function<void()> act) {
        settings_items.push_back({.row = {.kind = K::button, .label = std::move(label), .value = std::move(note)},
                                  .act = [act = std::move(act)](int dir) { if (dir == 0) act(); }});
    };
    auto pick = [](const auto& list, auto value) {
        const auto it = std::ranges::find(list, value);
        return it == list.end() ? std::size_t{0} : static_cast<std::size_t>(it - list.begin());
    };
    switch (settings_page) {
        case SettingsPage::displays: {
            heading(tr_(zh, "Connected", "已连接"));
            for (const auto& l : links) {
                const bool primary = &l == &links.front();
                text(std::format("{} {}", tr_(zh, "Display", "显示器"), l.number),
                     primary ? std::format("{} · {}×{} · {}x", tr_(zh, "primary", "主屏"), size.width, size.height, scale)
                             : std::format("{} {}", tr_(zh, "mirroring display", "镜像显示器"), links.front().number));
            }
            heading(tr_(zh, "Another display", "再加一块"));
            text(std::string(tr_(zh, "Open another Luban OS window on this system (its menu: New window): it mirrors this one.",
                                 "在这个系统上再开一个 Luban OS 窗口（菜单：新窗口）：它镜像这一块。")));
            text(std::string(tr_(zh, "The desktop's resolution is the first window's size: resize it, or make it full screen.",
                                 "桌面的分辨率就是第一个窗口的大小：缩放它，或让它全屏。")));
            break;
        }
        case SettingsPage::layout: {
            const std::vector<layout::Rule> rules{layout::Rule::tile, layout::Rule::stack};
            const std::vector<std::string> rule_names{std::string(tr_(zh, "Tile", "平铺")), std::string(tr_(zh, "Stack", "层叠"))};
            choice(std::string(tr_(zh, "This workspace", "这个工作区")), rule_names, pick(rules, lay.rule()), [this, rules](std::size_t k) {
                lay.set_rule(lay.current(), rules[k]);
                remember_arrangement();
                arrange();
                draw_shell();
            });
            choice(std::string(tr_(zh, "A new workspace", "新建的工作区")), rule_names, pick(rules, lay.options().rule), [this, rules](std::size_t k) {
                setting("desktop", "rule", std::string(layout::name(rules[k])));
                show_hint(tr_(zh, "From the next session", "下次会话起生效"), 3000);
            });
            const auto& lo = lay.options();
            choice(std::string(tr_(zh, "Columns run", "列的方向")),
                   {std::string(tr_(zh, "Side by side", "左右并排")), std::string(tr_(zh, "Downwards", "上下排"))},
                   lay.flow() == layout::Flow::vertical ? 1 : 0, [this](std::size_t k) {
                       const auto f = k == 1 ? layout::Flow::vertical : layout::Flow::horizontal;
                       lay.set_flow(lay.current(), f);
                       setting("desktop", "flow", std::string(layout::name(f)));
                       arrange();
                   });
            const std::vector<int> inner{0, 2, 4, 8, 12, 16}, outer{0, 4, 8, 16, 24}, columns{320, 400, 480, 560, 640, 800};
            auto numbers = [](const std::vector<int>& v) {
                std::vector<std::string> out;
                for (int n : v) out.push_back(std::to_string(n));
                return out;
            };
            choice(std::string(tr_(zh, "Gap between windows", "窗口间隔")), numbers(inner), pick(inner, int(lo.inner_gap)),
                   [this, inner](std::size_t k) {
                       const auto& now = lay.options();
                       lay.set_spacing(inner[k], now.outer_gap, now.min_column_width);
                       setting("desktop", "inner_gap", std::to_string(inner[k]));
                       arrange();
                   });
            choice(std::string(tr_(zh, "Gap at the edges", "边缘间隔")), numbers(outer), pick(outer, int(lo.outer_gap)),
                   [this, outer](std::size_t k) {
                       const auto& now = lay.options();
                       lay.set_spacing(now.inner_gap, outer[k], now.min_column_width);
                       setting("desktop", "outer_gap", std::to_string(outer[k]));
                       arrange();
                   });
            choice(std::string(tr_(zh, "Narrowest column", "最窄的列")), numbers(columns), pick(columns, int(lo.min_column_width)),
                   [this, columns](std::size_t k) {
                       const auto& now = lay.options();
                       lay.set_spacing(now.inner_gap, now.outer_gap, columns[k]);
                       setting("desktop", "min_column_width", std::to_string(columns[k]));
                       arrange();
                   });
            toggle(std::string(tr_(zh, "The status bar shows the other workspaces", "状态栏显示其他工作区")), status_others, [this](bool on) {
                status_others = on;
                setting("desktop", "status_others", on ? "on" : "off");
                draw_shell();
            });
            heading(tr_(zh, "WuXian (the infinite canvas)", "WuXian（无限画布）"));
            toggle(std::string(tr_(zh, "A minimap when WuXian holds more than the view", "WuXian 超出视野时显示小地图")), minimap_on,
                   [this](bool on) { minimap_on = on; setting("wuxian", "minimap", on ? "on" : "off"); draw_shell(); });
            toggle(std::string(tr_(zh, "Bookmarks for covered workspaces and windows", "被盖住的工作区和窗口显示书签")), bookmarks_on,
                   [this](bool on) { bookmarks_on = on; setting("wuxian", "bookmarks", on ? "on" : "off"); update_bookmarks(); });
            toggle(std::string(tr_(zh, "A moved window lines up with others", "移动的窗口与其他窗口对齐")), snap_on,
                   [this](bool on) { snap_on = on; setting("wuxian", "snap", on ? "on" : "off"); });
            // The switchers and the wheel (design 2026-10-20 WuXian §4).
            heading(tr_(zh, "Switching", "切换"));
            const std::vector<int> holds{250, 500, 750, 1000}, lits{0, 500, 1000};
            const std::vector<double> dims{0.3, 0.5, 0.7};
            choice(std::string(tr_(zh, "Held on an application, its windows after", "停留多久展开应用的窗口")), {"0.25 s", "0.5 s", "0.75 s", "1 s"},
                   pick(holds, hold_ms), [this, holds](std::size_t k) {
                       hold_ms = holds[k];
                       setting("switchers", "hold_ms", std::to_string(holds[k]));
                   });
            choice(std::string(tr_(zh, "The window chosen stays lit", "选中后突出")),
                   {std::string(tr_(zh, "Off", "关")), "0.5 s", "1 s"}, pick(lits, highlight_ms), [this, lits](std::size_t k) {
                       highlight_ms = lits[k];
                       setting("switchers", "highlight_ms", std::to_string(lits[k]));
                   });
            {
                std::size_t at = 1;
                for (std::size_t k = 0; k < dims.size(); ++k)
                    if (std::abs(dims[k] - dim_level) < 0.01) at = k;
                choice(std::string(tr_(zh, "The others dimmed to", "其他窗口的暗度")), {"0.3", "0.5", "0.7"}, at, [this, dims](std::size_t k) {
                    dim_level = dims[k];
                    setting("switchers", "dim", std::format("{}", dims[k]));
                });
            }
            heading(tr_(zh, "Scrolling", "滚动"));
            {
                const std::vector<double> speeds{0.6, 1.0, 1.6};
                const std::vector<std::string> names{"slow", "medium", "fast"};
                std::size_t at = 1;
                for (std::size_t k = 0; k < speeds.size(); ++k)
                    if (std::abs(speeds[k] - wheel_factor) < 0.01) at = k;
                choice(std::string(tr_(zh, "The wheel's speed", "滚动速度")),
                       {std::string(tr_(zh, "Slow", "慢")), std::string(tr_(zh, "Medium", "中")), std::string(tr_(zh, "Fast", "快"))}, at,
                       [this, speeds, names](std::size_t k) {
                           wheel_factor = speeds[k];
                           setting("scroll", "speed", names[k]);
                       });
            }
            toggle(std::string(tr_(zh, "A tiled strip's scroll lands on a column", "滚动对齐到列")), snap_columns,
                   [this](bool on) { snap_columns = on; setting("scroll", "snap", on ? "on" : "off"); });
            button(std::string(tr_(zh, "This page as it came", "恢复本页默认")), {}, [this] { reset_page(SettingsPage::layout); });
            break;
        }
        case SettingsPage::appearance: {
            std::vector<std::string> accents, backgrounds;
            std::vector<std::uint32_t> accent_rgb, background_rgb;
            for (const auto& a : kAccents) { accents.emplace_back(tr_(zh, a.en, a.zh)); accent_rgb.push_back(a.rgb); }
            for (const auto& b : kBackgrounds) { backgrounds.emplace_back(tr_(zh, b.en, b.zh)); background_rgb.push_back(b.rgb); }
            const auto rgb = [](draw::Color c) { return (std::uint32_t(c.r) << 16) | (std::uint32_t(c.g) << 8) | c.b; };
            choice(std::string(tr_(zh, "Accent", "强调色")), accents, pick(accent_rgb, rgb(theme.accent)), [this, accent_rgb](std::size_t k) {
                set_accent(accent_rgb[k]);
                setting("theme", "accent", std::format("#{:06x}", accent_rgb[k]));
            });
            choice(std::string(tr_(zh, "Background", "背景")), backgrounds, pick(background_rgb, rgb(theme.background)),
                   [this, background_rgb](std::size_t k) {
                       theme.background = draw::Color::rgb(background_rgb[k]);
                       const float bg[4] = {theme.background.r / 255.0f, theme.background.g / 255.0f, theme.background.b / 255.0f, 1.0f};
                       wlr_scene_rect_set_color(background, bg);
                       dots_drawn.clear();
                       draw_shell();
                       setting("theme", "background", std::format("#{:06x}", background_rgb[k]));
                   });
            toggle(std::string(tr_(zh, "Reduce motion: windows go at once, without gliding", "减少动态：窗口直接到位，不滑动")), reduce_motion,
                   [this](bool on) { reduce_motion = on; setting("appearance", "reduce_motion", on ? "on" : "off"); });
            {
                using M = opensal::PointerLocate::Mode;
                const std::vector<M> modes{M::off, M::grow, M::grow_color};
                const std::vector<std::string> names{"off", "grow", "grow-color"};
                choice(std::string(tr_(zh, "A shaken pointer, to find it", "晃动鼠标时的指针")),
                       {std::string(tr_(zh, "As it is", "不变")), std::string(tr_(zh, "Bigger", "放大")),
                        std::string(tr_(zh, "Bigger and coloured", "放大并变色"))},
                       static_cast<std::size_t>(std::ranges::find(modes, locate.mode) - modes.begin()), [this, modes, names](std::size_t k) {
                           locate.mode = modes[k];
                           setting("appearance", "pointer_locate", names[k]);
                           send_locate();
                       });
                if (locate.mode != M::off)
                    choice(std::string(tr_(zh, "Up to", "最大")), {"2x", "3x", "4x"}, static_cast<std::size_t>(std::clamp(locate.max, 2.0, 4.0) - 2),
                           [this](std::size_t k) {
                               locate.max = static_cast<double>(k + 2);
                               setting("appearance", "pointer_max", std::to_string(k + 2));
                               send_locate();
                           });
            }
            choice(std::string(tr_(zh, "Drawing (from the next session)", "渲染（下次会话起）")),
                   {std::string(tr_(zh, "The GPU when there is one", "自动（有 GPU 就用）")), std::string(tr_(zh, "Software", "软件"))},
                   o.renderer == "software" ? 1 : 0, [this](std::size_t k) {
                       setting("appearance", "renderer", k == 1 ? "software" : "auto");
                       show_hint(tr_(zh, "From the next session", "下次会话起生效"), 3000);
                   });
            text(std::string(tr_(zh, "Drawn now", "现在的渲染器")), renderer_text);
            button(std::string(tr_(zh, "This page as it came", "恢复本页默认")), {}, [this] { reset_page(SettingsPage::appearance); });
            break;
        }
        case SettingsPage::keys: {
            settings_items.push_back({.row = {.kind = K::search, .label = std::string(tr_(zh, "Type to find a key or what it does",
                                                                                          "输入以查找按键或功能")),
                                              .value = settings_search}});
            std::vector<std::string> sets{std::string(tr_(zh, "Luban", "Luban")), std::string(tr_(zh, "Luban without hjkl", "Luban（不用 hjkl）"))};
            choice(std::string(tr_(zh, "Set of keys", "键位方案")), sets, keys.set == "luban-no-hjkl" ? 1 : 0, [this](std::size_t k) {
                auto all = key_overrides;
                all["set"] = {k == 1 ? "luban-no-hjkl" : "luban"};
                keys.load(all);
                save_keys();
            });
            button(std::string(tr_(zh, "Every key as the set has it", "全部恢复为方案的默认")), {}, [this] {
                key_overrides.clear();
                const auto set = keys.set;
                keys.load({{"set", {set}}});
                save_keys();
            });
            std::string query;
            for (char ch : settings_search) query += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            std::optional<Group> group;
            for (const auto& a : actions()) {
                std::string chords;
                for (const auto& c : keys.chords(a.action)) chords += (chords.empty() ? "" : "  ") + chord_text(c);
                if (!query.empty()) {
                    std::string hay = std::format("{} {} {} {}", a.name, a.en, a.zh, chords);
                    for (auto& ch : hay) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (hay.find(query) == std::string::npos) continue;
                }
                if (group != a.group) {
                    group = a.group;
                    heading(group_label(a.group, zh));
                }
                settings_items.push_back({.row = {.kind = K::keys, .label = std::string(zh ? a.zh : a.en), .value = chords,
                                                  .changed = key_overrides.contains(std::string(a.name))},
                                          .act = [this](int dir) { if (dir == 0) settings_capture = true; },
                                          .action = std::string(a.name)});
            }
            break;
        }
        case SettingsPage::about: {
            for (auto line : std::views::split(system_info(), '\n')) {
                const std::string l(line.begin(), line.end());
                if (const auto colon = l.find(": "); colon != std::string::npos) text(l.substr(0, colon), l.substr(colon + 2));
            }
            button(std::string(tr_(zh, "Copy", "复制")), std::string(tr_(zh, "what this system is, to paste where a problem is told",
                                                                       "系统信息，报告问题时粘贴")),
                   [this] {
                       if (opensal::has(provided, opensal::Interface::Clipboard)) {
                           send(opensal::wire::encode('b', system_info()));
                           show_hint(tr_(zh, "Copied", "已复制"), 2000);
                       } else {
                           show_hint(tr_(zh, "This system may not copy to the computer's clipboard (luban config <system> --allow clipboard)",
                                         "这个系统不能复制到电脑的剪贴板（luban config <系统> --allow clipboard）"), 5000);
                       }
                   });
            button(std::string(tr_(zh, "Everything as it came", "全部恢复默认")),
                   std::string(tr_(zh, "every page and every key, asked first", "所有页面与快捷键（先确认）")), [this] {
                       asking_reset = true;
                       overlay = Overlay::dialog;
                       draw_overlay();
                   });
            heading(tr_(zh, "Files", "文件"));
            const auto env = os::environment();
            text("keys.json", config::keys_file(env).string());
            text("state.json", config::state_file(env).string());
            text(std::format("{}.ini", o.config_profile), config::person_file(o.config_profile, env).string());
            heading(tr_(zh, "Upgrading", "升级"));
            text(std::string(tr_(zh, "On the computer: luban upgrade <system> (luban status <system> says what is new)",
                                 "在电脑上：luban upgrade <系统>（luban status <系统> 说明有什么新内容）")));
            heading(tr_(zh, "Open source", "开源"));
            text("openxlings/xlings", "xlings, luban, Luban OS, xdesk · Apache-2.0");
            text("openxlings/xim-pkgindex", tr_(zh, "the packages and the editions", "软件包与 edition").data());
            text("openxlings/libxpkg", tr_(zh, "the package format", "软件包格式").data());
            text(std::string(tr_(zh, "Built on", "基于")), "wlroots, Wayland, pixman, FreeType, HarfBuzz, libpng, xkbcommon");
            break;
        }
    }
}

// `action` answers to `c` alone; another that had it keeps its others. Kept
// in keys.json. The other's name, when it had it.
std::string Server::rebind(const std::string& action, const Chord& c) {
    std::string taken;
    if (const auto other = keys.match(c)) {
        const auto* info = &*std::ranges::find(actions(), *other, &ActionInfo::action);
        if (info->name != action) {
            std::vector<std::string> left;
            for (const auto& oc : keys.chords(*other))
                if (!(oc == c)) left.push_back(chord_spec_(oc));
            key_overrides[std::string(info->name)] = left;
            taken = std::string(info->name);
        }
    }
    key_overrides[action] = {chord_spec_(c)};
    auto all = key_overrides;
    all["set"] = {keys.set};
    keys.load(all);
    save_keys();
    return taken;
}

void Server::draw_settings() {
    settings_rows();
    // The lit row: one that can be acted on (headings and text are skipped).
    auto selectable = [&](std::size_t k) {
        const auto kind = settings_items[k].row.kind;
        return kind != shell::SettingsRow::Kind::heading && kind != shell::SettingsRow::Kind::text;
    };
    if (!settings_items.empty()) {
        settings_row = std::min(settings_row, settings_items.size() - 1);
        if (!selectable(settings_row)) {
            std::size_t k = settings_row;
            while (k < settings_items.size() && !selectable(k)) ++k;
            if (k == settings_items.size()) {
                k = settings_row;
                while (k > 0 && !selectable(k)) --k;
            }
            settings_row = k;
        }
    }
    shell::SettingsView v;
    for (const auto p : kPages) v.pages.emplace_back(page_label_(p, zh));
    v.page = static_cast<std::size_t>(std::ranges::find(kPages, settings_page) - kPages.begin());
    v.title = std::string(page_label_(settings_page, zh));
    for (const auto& it : settings_items) v.rows.push_back(it.row);
    if (!settings_items.empty() && selectable(settings_row)) v.selected = settings_row;
    if (settings_hover && *settings_hover < settings_items.size() && selectable(*settings_hover)) v.hover = settings_hover;
    v.keyboard = settings_keyboard;
    v.caret = settings_caret;
    v.preedit = settings_preedit;
    v.capturing = settings_capture;
    v.hint = std::string(settings_page == SettingsPage::keys
        ? tr_(zh, "Enter: new keys for it  ·  Delete: back to the set's  ·  type to find  ·  Tab: the next page  ·  Esc: close",
              "回车：改它的键  ·  Delete：恢复默认  ·  输入即查找  ·  Tab：下一页  ·  Esc：关闭")
        : tr_(zh, "↑ ↓: a row  ·  ← →: change it  ·  Enter: switch, press  ·  Tab: the next page  ·  Esc: close",
              "↑ ↓：选行  ·  ← →：修改  ·  回车：切换、按下  ·  Tab：下一页  ·  Esc：关闭"));
    const int w = logical_w(), h = logical_h();
    overlay_picture = shell::settings(ui(), big_font ? &*big_font : nullptr, theme, v, w, h, zh, scale);
    wlr_scene_node_set_position(&overlay_node->node, 0, 0);
    show(overlay_node, std::move(overlay_picture.image), scale);
    overlay_picture.image = {};
    wlr_scene_node_set_enabled(&overlay_node->node, true);
}

// The pointer over the settings: the row it is on (its own look; the
// keyboard's ring goes until a key is used again).
void Server::settings_motion(double x, double y) {
    std::optional<std::size_t> over;
    for (const auto& a : overlay_picture.areas)
        if (a.target == shell::Target::item && x >= a.rect.x && y >= a.rect.y && x < a.rect.x + a.rect.width && y < a.rect.y + a.rect.height)
            over = static_cast<std::size_t>(a.index);
    if (over == settings_hover && !settings_keyboard) return;
    settings_hover = over;
    settings_keyboard = false;
    draw_overlay();
}

bool Server::settings_key(xkb_keysym_t s) {
    settings_keyboard = true;
    if (s == key::z && held(wlr_keyboard_modifier::WLR_MODIFIER_CTRL)) { undo_reset(); draw_overlay(); return true; }
    // A key given to an action (design 2026-10-20 §12.4): what is held with
    // it is the chord; one another action had is taken from it.
    if (settings_capture) {
        // A modifier on its own is part of the chord to come.
        if ((s >= 0xffe1 && s <= 0xffee) || (s >= 0xfe01 && s <= 0xfe13)) return true;
        settings_capture = false;
        if (s != key::Escape && settings_row < settings_items.size() && !settings_items[settings_row].action.empty()) {
            Chord c{.super = held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO), .ctrl = held(wlr_keyboard_modifier::WLR_MODIFIER_CTRL),
                    .shift = held(wlr_keyboard_modifier::WLR_MODIFIER_SHIFT), .alt = held(wlr_keyboard_modifier::WLR_MODIFIER_ALT)};
            if (pressed_code >= 2 && pressed_code <= 11) c.digit = static_cast<int>(pressed_code) - 1;
            else c.sym = s == key::ISO_Left_Tab ? key::Tab : xkb_keysym_to_lower(s);
            if (!c.super && !c.alt && !c.ctrl) {
                show_hint(tr_(zh, "A key for the desktop is held with Super (or Ctrl, Alt): alone it is the applications'",
                              "桌面的键要与 Super（或 Ctrl、Alt）一起按：单独的键是应用的"), 4000);
            } else if (const auto taken = rebind(settings_items[settings_row].action, c); !taken.empty()) {
                const auto* info = &*std::ranges::find(actions(), taken, &ActionInfo::name);
                show_hint(std::format("{} {} {}", chord_text(c), tr_(zh, "was", "原属于"), zh ? info->zh : info->en), 4000);
            }
        }
        draw_overlay();
        return true;
    }
    const auto n = settings_items.size();
    auto move = [&](int dir) {
        if (n == 0) return;
        std::size_t k = settings_row;
        for (std::size_t step = 0; step < n; ++step) {
            k = (k + n + static_cast<std::size_t>(dir)) % n;
            const auto kind = settings_items[k].row.kind;
            if (kind != shell::SettingsRow::Kind::heading && kind != shell::SettingsRow::Kind::text) break;
        }
        settings_row = k;
    };
    auto page = [&](int dir) {
        const auto at = static_cast<std::size_t>(std::ranges::find(kPages, settings_page) - kPages.begin());
        settings_page = kPages[(at + kPages.size() + static_cast<std::size_t>(dir)) % kPages.size()];
        settings_row = 0;
        settings_search.clear();
    };
    const bool searching = settings_page == SettingsPage::keys;
    if (s == key::Tab) page(1);
    else if (s == key::ISO_Left_Tab) page(-1);
    else if (s == key::Down) move(1);
    else if (s == key::Up) move(-1);
    else if ((s == key::Left || s == key::Right) && settings_row < n && settings_items[settings_row].act) settings_items[settings_row].act(s == key::Left ? -1 : 1);
    else if ((s == key::Return || s == key::KP_Enter || (s == key::space && !searching)) && settings_row < n && settings_items[settings_row].act)
        settings_items[settings_row].act(0);
    else if (s == key::Delete && settings_row < n && !settings_items[settings_row].action.empty()) {
        // Back to the set's own chords.
        key_overrides.erase(settings_items[settings_row].action);
        auto all = key_overrides;
        all["set"] = {keys.set};
        keys.load(all);
        save_keys();
    } else if (searching && s == key::BackSpace) {
        if (!settings_search.empty()) {
            // A whole UTF-8 character.
            do settings_search.pop_back();
            while (!settings_search.empty() && (static_cast<unsigned char>(settings_search.back()) & 0xc0) == 0x80);
        }
        settings_row = 0;
    } else if (searching) {
        const auto cp = xkb_keysym_to_utf32(s);
        if (cp >= 0x20 && cp != 0x7f && !held(wlr_keyboard_modifier::WLR_MODIFIER_LOGO) && !held(wlr_keyboard_modifier::WLR_MODIFIER_CTRL)) {
            std::array<char, 8> utf8{};
            const int len = xkb_keysym_to_utf8(s, utf8.data(), utf8.size());
            if (len > 1) settings_search.append(utf8.data(), static_cast<std::size_t>(len - 1));
            settings_row = 0;
        }
    }
    draw_overlay();
    return true;
}

bool Server::settings_click(double x, double y) {
    const shell::Area* chosen = nullptr;
    auto inside = [&](const shell::Area& a) { return x >= a.rect.x && y >= a.rect.y && x < a.rect.x + a.rect.width && y < a.rect.y + a.rect.height; };
    // A choice before its row, its row before nothing.
    for (const auto target : {shell::Target::choice, shell::Target::item, shell::Target::page}) {
        for (const auto& a : overlay_picture.areas)
            if (a.target == target && inside(a)) { chosen = &a; break; }
        if (chosen) break;
    }
    if (!chosen) {
        // Outside the card: closed (inside it, nothing happens).
        const double cw = std::min(940.0, logical_w() - 40.0), ch = std::min(660.0, logical_h() - 40.0);
        const double cx = std::round((logical_w() - cw) / 2), cy = std::round((logical_h() - ch) / 2);
        if (x < cx || y < cy || x >= cx + cw || y >= cy + ch) close_overlay(false);
        return true;
    }
    settings_capture = false;
    if (chosen->target == shell::Target::page) {
        settings_page = kPages[static_cast<std::size_t>(chosen->index) % kPages.size()];
        settings_row = 0;
        settings_search.clear();
    } else if (chosen->target == shell::Target::choice) {
        const auto row = static_cast<std::size_t>(chosen->index / 64), k = static_cast<std::size_t>(chosen->index % 64);
        if (row < settings_items.size() && settings_items[row].act) {
            const auto now = settings_items[row].row.chosen, n = settings_items[row].row.choices.size();
            // The choice clicked: as many steps from the one chosen.
            if (n > 0 && k != now) settings_items[row].act(static_cast<int>((k + n - now) % n));
        }
    } else {
        // A click does what the row does; it does not make it the keyboard's row.
        const auto row = static_cast<std::size_t>(chosen->index);
        if (row < settings_items.size() && settings_items[row].act) {
            if (!settings_items[row].action.empty()) settings_row = row;   // a key to give: it is the one
            settings_items[row].act(0);
        }
    }
    draw_overlay();
    return true;
}

}  // namespace xdesk::compositor
