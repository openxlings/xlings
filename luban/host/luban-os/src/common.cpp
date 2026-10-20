// What every view uses: the interface's words, the app's own settings, a job's
// phases, the page frame and its pieces, and the overlays (closing, confirming,
// the Host key's menu, the bar over a system).
module luban.os;

import std;
import luban.compat;
import xlings.libs.json;
import imkit;
import tray;
import luban.service;
import luban.vm;
import xdesk.term;
import opensal;
import opensal.wire;
import xlings.subos.home_view;
import xlings.subos.located;
import xlings.subos.policy_store;
import xlings.subos.policy;
import xlings.subos.root_etc;
import luban.service.archive;
import xlings.libs.sha512;
import xlings.observe;
import xlings.platform;
import xlings.platform.target;
import luban.desktop.host;
import opensal.wlr;
import :core;

namespace luban::os {

AppSettings read_app_settings(const fs::path& home) {
    AppSettings s;
    std::error_code ec;
    const auto file = home / "config" / "luban-os.json";
    if (!fs::is_regular_file(file, ec)) return s;
    auto j = Json::parse(platform::read_file_to_string(file.string()), nullptr, false);
    if (!j.is_object()) return s;
    s.default_system = j.value("default_system", std::string{});
    s.countdown = std::clamp(j.value("countdown", 10), 0, 120);
    if (auto u = j.find("last_used"); u != j.end() && u->is_object())
        for (auto it = u->begin(); it != u->end(); ++it)
            if (it.value().is_string()) s.last_used[it.key()] = it.value().get<std::string>();
    s.language = j.value("language", std::string("auto"));
    if (s.language != "en" && s.language != "zh") s.language = "auto";
    s.reduce_motion = j.value("reduce_motion", false);
    s.lock_keyboard = j.value("lock_keyboard", true);
    s.console_size = std::clamp(j.value("console_size", 16.0f), 8.0f, 40.0f);
    if (auto c = j.find("console_sizes"); c != j.end() && c->is_object())
        for (auto it = c->begin(); it != c->end(); ++it)
            if (it.value().is_number()) s.console_sizes[it.key()] = std::clamp(it.value().get<float>(), 8.0f, 40.0f);
    s.minimize_to_tray = j.value("minimize_to_tray", true);
    s.keep_in_tray = j.value("keep_in_tray", false);
    s.close_choice = std::clamp(j.value("close_choice", 0), 0, 1);
    return s;
}

void write_app_settings(const fs::path& home, const AppSettings& s) {
    Json j{{"default_system", s.default_system}, {"countdown", s.countdown}, {"last_used", Json::object()},
           {"language", s.language}, {"reduce_motion", s.reduce_motion}, {"lock_keyboard", s.lock_keyboard}, {"console_size", s.console_size},
           {"console_sizes", Json::object()}, {"minimize_to_tray", s.minimize_to_tray}, {"keep_in_tray", s.keep_in_tray},
           {"close_choice", s.close_choice}};
    for (const auto& [k, v] : s.last_used) j["last_used"][k] = v;
    for (const auto& [k, v] : s.console_sizes) j["console_sizes"][k] = v;
    std::error_code ec;
    fs::create_directories(home / "config", ec);
    platform::write_file_atomic((home / "config" / "luban-os.json").string(), j.dump(2));
}

void Job::take(const service::Event& e) {
    std::lock_guard l(m);
    using K = service::Event::Kind;
    // The operation's one progress, when it has one (design 2026-10-20 G22):
    // the bar and the stage follow it and never go back.
    const auto now = std::chrono::steady_clock::now();
    if (e.overall >= 0) {
        overall = std::max(overall, e.overall);
        if (!e.stage.empty() && e.stage != stage) stage_since = now;
        if (!e.stage.empty()) stage = e.stage;
        stages = e.stages;
        stage_percent = e.stage_percent;
        stage_detail = e.stage_detail;
    }
    if (!e.text.empty() && (e.kind == K::Phase || e.kind == K::Progress || e.kind == K::Log)) said_at = now;
    switch (e.kind) {
    case K::Phase: {
        auto it = std::ranges::find(phases, e.phase, &Phase::name);
        if (it == phases.end()) phases.push_back({e.phase, e.text, e.state});
        else { it->state = e.state; if (!e.text.empty()) it->text = e.text; }
        current = e.text;
        if (e.state == "begin") percent = -1;
        if (!e.phase.empty() && e.overall < 0) stage = e.phase;
        break;
    }
    case K::Progress:
        if (e.percent >= 0) percent = e.percent;
        if (!e.text.empty()) current = e.text;
        if (!e.phase.empty() && e.overall < 0) stage = e.phase;
        log.push_back(e.text);
        break;
    case K::Log: log.push_back(e.text); break;
    case K::Notice: log.push_back(e.text); break;
    case K::Error:
        errors.push_back(e.hint.empty() ? e.text : std::format("{}\n  {}", e.text, e.hint));
        log.push_back(e.text);
        break;
    case K::Data:
        if (e.data_kind == "download_progress" && e.data.is_object()) {
            // The bytes so far of the batch: a share the bar can show.
            double total = 0, done_bytes = 0;
            for (const auto& f : e.data.value("files", Json::array())) {
                total += f.value("totalBytes", 0.0);
                done_bytes += f.value("downloadedBytes", 0.0);
            }
            if (total > 0) {
                percent = done_bytes / total;
                if (e.overall < 0) stage = "download";
            }
        }
        break;
    }
    if (log.size() > 2000) log.erase(log.begin(), log.begin() + 500);
}

// ── views ───────────────────────────────────────────────────────────────

using imkit::px;

void begin_full_(const char* id, bool transparent) {
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                  | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar
                                  | ImGuiWindowFlags_NoScrollWithMouse | (transparent ? ImGuiWindowFlags_NoBackground : 0));
    ImGui::PopStyleVar();
}

// The last item drawn, by name (App::anchors).
void mark_(App& a, std::string name) {
    (void)a;
    imkit::anchor(name);   // what can be seen whole (imkit decides), into a.anchors
}

const imkit::Theme& th_() { return imkit::theme(); }

// A line of text centred across the window at `y`.
void centred_(ImDrawList* dl, float y, std::string_view s, ImU32 color, float size, bool title) {
    const float W = ImGui::GetWindowWidth();
    const auto shown = imkit::fit(s, W - px(32), size, title);
    const auto ts = imkit::text_size(shown, size, title);
    imkit::text(dl, ImVec2(ImGui::GetWindowPos().x + (W - ts.x) / 2, ImGui::GetWindowPos().y + y), shown, color, size, title);
}

// A paragraph across `width` at `at`; returns the height it took.
float paragraph_(ImDrawList* dl, ImVec2 at, std::string_view s, float width, ImU32 color, float size) {
    const float h = imkit::text_size("Ag", size).y + px(3);
    float y = at.y;
    for (const auto& line : imkit::wrap(s, width, size)) {
        imkit::text(dl, ImVec2(at.x, y), line, color, size);
        y += h;
    }
    return y - at.y;
}

// The app's bar at the top of a page: its title in the pixel face, a way
// back when there is one. True when back was clicked.
bool page_bar_(App& a, std::string_view title, bool back, std::string_view back_id) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = px(64);
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), th_().bg_deep);
    dl->AddRectFilled(ImVec2(o.x, o.y + H - imkit::unit() / 3), ImVec2(o.x + W, o.y + H), th_().line);
    float x = px(20);
    bool clicked = false;
    if (back) {
        ImGui::SetCursorPos(ImVec2(x, px(14)));
        clicked = imkit::button(back_id, tr("luban.os.common.back"), ImVec2(96, 36), imkit::Look::Ghost, true, false, imkit::Icon::Back);
        mark_(a, std::string(back_id));
        x += px(112);
    }
    const auto ts = imkit::text_size(title, 22, true);
    imkit::text(dl, ImVec2(o.x + x, o.y + (H - ts.y) / 2), title, th_().text, 22, true);
    return clicked;
}

// "3 h ago", from an ISO time.
std::string ago_(const std::string& iso) {
    if (iso.size() < 19) return {};
    std::tm t{};
    std::istringstream in(iso);
    in >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S");
    if (in.fail()) return {};
    const auto then = std::chrono::sys_days(std::chrono::year(t.tm_year + 1900) / (t.tm_mon + 1) / t.tm_mday)
                      + std::chrono::hours(t.tm_hour) + std::chrono::minutes(t.tm_min) + std::chrono::seconds(t.tm_sec);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - then).count();
    if (secs < 60) return std::string(tr("luban.os.common.just_now"));
    const auto mins = secs / 60, hours = secs / 3600;
    if (secs < 3600) return std::vformat(tr("luban.os.common.min_ago"), std::make_format_args(mins));
    if (secs < 86400) return std::vformat(tr("luban.os.common.h_ago"), std::make_format_args(hours));
    const auto days = secs / 86400;
    return std::vformat(tr("luban.os.common.d_ago"), std::make_format_args(days));
}

std::string edition_label_(const service::Environment& e) {
    // An environment with no edition is a view of this host (its packages, no root of its own).
    if (e.edition.empty()) return std::string(e.kind == "view" ? tr("luban.os.common.host_view") : tr("luban.os.common.root"));
    auto label = e.edition;
    label = std::string(luban::compat::without_frozen_prefix(label));
    return label;   // with its version
}

// The systems shown, after the filter typed on the home.
std::vector<int> shown_(const App& a) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(a.systems.size()); ++i)
        if (a.filter.empty() || a.systems[static_cast<std::size_t>(i)].name.find(a.filter) != std::string::npos) out.push_back(i);
    return out;
}

// A small coloured word in a frame (a system's mode, its state).
void chip_(ImDrawList* dl, ImVec2 at, std::string_view s, ImU32 fg, ImU32 bg) {
    const auto ts = imkit::text_size(s, 13);
    dl->AddRectFilled(at, ImVec2(at.x + ts.x + px(12), at.y + ts.y + px(4)), bg);
    imkit::text(dl, ImVec2(at.x + px(6), at.y + px(2)), s, fg, 13);
}


ImVec2 overlay_(ImDrawList* dl, ImVec2 size) {
    const auto* vp = ImGui::GetMainViewport();
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(8, 9, 16, 170));
    const ImVec2 a(std::round(vp->Pos.x + (vp->Size.x - size.x) / 2), std::round(vp->Pos.y + (vp->Size.y - size.y) / 2));
    imkit::panel(dl, a, ImVec2(a.x + size.x, a.y + size.y), imkit::Bevel::Raised, th_().panel_lo);
    return a;
}

void begin_overlay_(const char* id) {
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                  | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
}

// Closing the window of a running machine (design 2026-10-16 L §13.11): two
// cards, the chosen one plain to see.
void close_dialog_(App& a) {
    begin_overlay_("close");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(std::min(px(640), ImGui::GetWindowWidth() - px(32)), px(360));
    const ImVec2 at = overlay_(dl, size);
    const ImVec2 wo = ImGui::GetWindowPos();
    imkit::text(dl, ImVec2(at.x + px(24), at.y + px(20)), std::vformat(tr("luban.os.common.close"), std::make_format_args(a.current)),
                th_().text, 22, true);
    const float cardW = (size.x - px(48) - px(16)) / 2;
    const bool temp = a.close_temp;
    struct Card { imkit::Icon icon; std::string_view title, detail; };
    const std::array<Card, 2> cards{{
        {imkit::Icon::Moon, "luban.os.common.close.keep", "luban.os.common.close.keep_detail"},
        {imkit::Icon::Power, "luban.os.common.close.power_off",
         temp ? "luban.os.common.close.power_off_detail_temp" : "luban.os.common.close.power_off_detail"},
    }};
    for (int k = 0; k < 2; ++k) {
        const auto& c = cards[static_cast<std::size_t>(k)];
        ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(24) + k * (cardW + px(16)), at.y - wo.y + px(64)));
        const imkit::Picture pic{imkit::kIconSize, [icon = c.icon, k, &a](ImDrawList* d, ImVec2 p, float cl) {
            imkit::icon(d, icon, p, cl * 1.5f, a.close_choice == k ? th_().accent_hi : th_().text_dim);
        }};
        if (imkit::option_card(k == 0 ? "close:background" : "close:poweroff", pic, tr(c.title), tr(c.detail), a.close_choice == k,
                               ImVec2(cardW / imkit::scale(), 170), a.keyboard && a.close_choice == k))
            a.close_choice = k;
        mark_(a, k == 0 ? "close:background" : "close:poweroff");
    }
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(24), at.y - wo.y + px(252)));
    imkit::checkbox("close:remember", tr("luban.os.common.remember_my_choice"), a.close_remember);
    mark_(a, "close:remember");
    bool decided = false;
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + size.x - px(24) - px(120) - px(12) - px(120), at.y - wo.y + size.y - px(64)));
    if (imkit::button("close:cancel", tr("luban.os.common.cancel"), ImVec2(120, 40), imkit::Look::Ghost)) {
        a.close_dialog = false;
        a.window->clear_close_request();
    }
    mark_(a, "close:cancel");
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + size.x - px(24) - px(120), at.y - wo.y + size.y - px(64)));
    if (imkit::button("close:confirm", tr("luban.os.common.ok"), ImVec2(120, 40), imkit::Look::Primary)) decided = true;
    mark_(a, "close:confirm");
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Left" || k.name == "Right") { a.close_choice = 1 - a.close_choice; a.keyboard = true; }
        else if (k.name == "Enter") decided = true;
        else if (k.name == "Escape") { a.close_dialog = false; a.window->clear_close_request(); }
    }
    if (decided) {
        const bool off = a.close_choice == 1;
        a.settings.close_choice = a.close_choice;
        write_app_settings(a.home, a.settings);
        if (a.close_remember) (void)service::set_setting(a.home, a.current, "on-close", off ? "poweroff" : "background");
        a.close_dialog = false;
        if (off) {
            service::Context c = a.ctx;
            (void)vm::stop(c, a.current);
        }
        a.console.reset();
        a.close_display();
        a.quit = true;
    }
}

// A question over the window (removing a system).
void confirm_(App& a) {
    begin_overlay_("confirm");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(std::min(px(a.confirm->alt.empty() ? 520 : 700), ImGui::GetWindowWidth() - px(32)), px(220));
    const ImVec2 at = overlay_(dl, size);
    const ImVec2 wo = ImGui::GetWindowPos();
    auto& c = *a.confirm;
    imkit::icon(dl, c.danger ? imkit::Icon::Warn : imkit::Icon::Dot, ImVec2(at.x + px(24), at.y + px(24)), std::round(px(2)),
                c.danger ? th_().bad : th_().accent);
    imkit::text(dl, ImVec2(at.x + px(60), at.y + px(24)), c.title, th_().text, 20, true);
    paragraph_(dl, ImVec2(at.x + px(24), at.y + px(68)), c.body, size.x - px(48), th_().text_dim, 15);
    bool yes = false, no = false, alt = false;
    float bx = size.x - px(24) - px(140) - px(12) - px(120);
    if (!c.alt.empty()) {
        const float aw = std::max(px(140), imkit::text_size(c.alt).x + px(32));
        bx -= aw + px(12);
        ImGui::SetCursorPos(ImVec2(at.x - wo.x + size.x - px(24) - px(140) - px(12) - aw, at.y - wo.y + size.y - px(60)));
        alt = imkit::button("confirm:alt", c.alt, ImVec2(aw / imkit::scale(), 40), imkit::Look::Danger);
        mark_(a, "confirm:alt");
    }
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + bx, at.y - wo.y + size.y - px(60)));
    no = imkit::button("confirm:no", tr("luban.os.common.cancel"), ImVec2(120, 40), imkit::Look::Ghost);
    mark_(a, "confirm:no");
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + size.x - px(24) - px(140), at.y - wo.y + size.y - px(60)));
    yes = imkit::button("confirm:yes", c.yes, ImVec2(140, 40), c.danger ? imkit::Look::Danger : imkit::Look::Primary);
    mark_(a, "confirm:yes");
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Escape") no = true;
        else if (k.name == "Enter") yes = true;
    }
    if (alt) { auto act = std::move(c.on_alt); a.confirm.reset(); if (act) act(); }
    else if (yes) { auto act = std::move(c.on_yes); a.confirm.reset(); if (act) act(); }
    else if (no) a.confirm.reset();
}

// The Host key's menu, over the system.
void host_menu_(App& a) {
    begin_overlay_("host");
    auto* dl = ImGui::GetWindowDrawList();
    struct Item { std::string label; std::string id; };
    std::vector<Item> items{{std::string(tr("luban.os.common.menu.back")), "menu:back-to-the-list"},
                            {std::string(tr("luban.os.common.menu.manage")), "menu:manage"},
                            {std::string(tr(a.window->fullscreen() ? "luban.os.common.menu.leave_full_screen" : "luban.os.common.menu.full_screen")),
                             a.window->fullscreen() ? "menu:leave-full-screen" : "menu:full-screen"},
                            {std::string(tr("luban.os.common.menu.new_window")), "menu:new-window"}};
    const bool machine = (a.console && a.console->is_machine()) || (a.display && a.display->is_machine());
    // Its desktops and its console, to switch to (design 2026-10-18 §3): the
    // session changes, the machine does not restart.
    std::vector<std::string> labels;   // owns the words of the desktop items
    labels.reserve(a.menu_desktops.size() + 1);
    for (const auto& d : a.menu_desktops) {
        if (a.view == View::Desktop && a.shown_desktop == d) continue;   // the one on screen
        labels.push_back(std::vformat(tr("luban.os.common.desktop"), std::make_format_args(d)));
        items.push_back({labels.back(), "menu:desktop:" + d});
    }
    if (a.view == View::Desktop) {
        items.push_back({std::string(tr("luban.os.common.menu.console")), "menu:console"});
        // The keyboard held or let go: the same as Ctrl+Alt+K.
        items.push_back({std::string(tr(a.window->keyboard_grabbed() ? "luban.os.common.menu.release_keyboard"
                                        : a.keyboard_wanted              ? "luban.os.common.menu.keyboard_not_held"
                                                                         : "luban.os.common.menu.hold_keyboard")),
                         "menu:keyboard"});
    }
    if (machine) items.push_back({std::string(tr("luban.os.common.close.power_off")), "menu:power-off"});
    items.push_back({std::string(tr("luban.os.common.menu.close")), "menu:close-this-menu"});
    const ImVec2 size(px(340), px(64) + static_cast<float>(items.size()) * px(48));
    const ImVec2 at = overlay_(dl, size);
    const ImVec2 wo = ImGui::GetWindowPos();
    imkit::text(dl, ImVec2(at.x + px(20), at.y + px(18)), imkit::fit(a.current, size.x - px(40), 20, true), th_().text, 20, true);
    std::string picked;
    a.menu_focus = std::clamp(a.menu_focus, 0, static_cast<int>(items.size()) - 1);
    for (std::size_t k = 0; k < items.size(); ++k) {
        ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(16), at.y - wo.y + px(56) + k * px(48)));
        if (imkit::button(items[k].id, items[k].label, ImVec2((size.x - px(32)) / imkit::scale(), 40),
                          imkit::Look::Ghost, true, a.keyboard && a.menu_focus == static_cast<int>(k)))
            picked = items[k].id;
        mark_(a, items[k].id);
    }
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Down") { a.menu_focus = (a.menu_focus + 1) % static_cast<int>(items.size()); a.keyboard = true; }
        else if (k.name == "Up") { a.menu_focus = (a.menu_focus + static_cast<int>(items.size()) - 1) % static_cast<int>(items.size()); a.keyboard = true; }
        else if (k.name == "Enter") picked = items[static_cast<std::size_t>(a.menu_focus)].id;
        else if (k.name == "Escape") picked = "menu:close-this-menu";
    }
    if (picked.empty()) return;
    a.menu = false;
    if (picked == "menu:back-to-the-list") a.back_home();
    else if (picked == "menu:manage") { a.before_admin = a.view; a.view = View::Admin; }
    else if (picked == "menu:full-screen" || picked == "menu:leave-full-screen") a.window->set_fullscreen(!a.window->fullscreen());
    else if (picked == "menu:keyboard") {
        a.keyboard_wanted = !a.keyboard_wanted;
        a.keyboard_told = true;
        if (!a.keyboard_wanted) a.pointer_lock_asked = false;
    }
    else if (picked == "menu:new-window") {
        const auto self = platform::get_executable_path();
        (void)platform::spawn_detached({self.string(), "--attach", a.current}, a.home / "logs" / "luban-os.log");
    } else if (picked.starts_with("menu:desktop:") || picked == "menu:console") {
        a.once_console = picked == "menu:console";
        a.once_desktop = a.once_console ? std::string{} : picked.substr(13);
        const bool was_machine = machine;
        a.console.reset();
        a.close_display();
        if (was_machine) a.open_machine(a.current);
        else a.open_console(a.current, false);
    } else if (picked == "menu:power-off") {
        a.console.reset();
        a.close_display();
        a.power_off(a.current);
    }
}

// The app's bar over a system (design 2026-10-16 L §13.6): its badge, name,
// how it runs, and the Host key -- the app's, not the system's.
float system_bar_(App& a, std::string_view mode) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = px(30);
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), th_().bg_deep);
    auto env = std::ranges::find(a.systems, a.current, &service::Environment::name);
    const std::string edition = env != a.systems.end() ? App::edition_name(*env) : std::string{};
    const float cell = std::max(1.0f, std::floor(px(1.5f)));
    art::badge(dl, art::badge_of(edition), edition, ImVec2(o.x + px(8), o.y + (H - 16 * cell) / 2), cell);
    imkit::text(dl, ImVec2(o.x + px(8) + 16 * cell + px(8), o.y + px(6)), a.current, th_().text, 14, true);
    const float nx = o.x + px(8) + 16 * cell + px(16) + imkit::text_size(a.current, 14, true).x;
    chip_(dl, ImVec2(nx, o.y + px(5)), mode, th_().text, art::edition_color(edition, -1));
    const std::string hint(tr("luban.os.common.ctrl_alt_menu_ctrl_ctrl"));
    const auto ts = imkit::text_size(hint, 13);
    imkit::text(dl, ImVec2(o.x + W - ts.x - px(12), o.y + px(7)), hint, th_().text_faint, 13);
    return H;
}

}  // namespace luban::os
