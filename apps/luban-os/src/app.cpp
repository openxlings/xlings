module;

#include <imgui.h>

module luban.os;

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.platform;
import opensal;
import luban.service;
import luban.vm;
import luban.desktop.host;
import luban.desktop.term;
import luban.desktop.wlr;

namespace luban::os {

namespace platform = xlings::platform;
namespace host = luban::desktop::host;
using Json = nlohmann::json;

std::string_view tr(std::string_view en, std::string_view zh) {
    static const bool chinese = [] {
        for (const auto* v : {"LUBAN_OS_LANG", "LC_ALL", "LC_MESSAGES", "LANG"})
            if (const char* l = std::getenv(v); l && *l) return std::string_view(l).starts_with("zh");
        return false;
    }();
    return chinese ? zh : en;
}

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
    return s;
}

void write_app_settings(const fs::path& home, const AppSettings& s) {
    Json j{{"default_system", s.default_system}, {"countdown", s.countdown}, {"last_used", Json::object()}};
    for (const auto& [k, v] : s.last_used) j["last_used"][k] = v;
    std::error_code ec;
    fs::create_directories(home / "config", ec);
    platform::write_file_atomic((home / "config" / "luban-os.json").string(), j.dump(2));
}

void Job::take(const service::Event& e) {
    std::lock_guard l(m);
    using K = service::Event::Kind;
    switch (e.kind) {
    case K::Phase: {
        auto it = std::ranges::find(phases, e.phase, &Phase::name);
        if (it == phases.end()) phases.push_back({e.phase, e.text, e.state});
        else { it->state = e.state; if (!e.text.empty()) it->text = e.text; }
        current = e.text;
        if (e.state == "begin") percent = -1;
        break;
    }
    case K::Progress:
        if (e.percent >= 0) percent = e.percent;
        if (!e.text.empty()) current = e.text;
        log.push_back(e.text);
        break;
    case K::Log: log.push_back(e.text); break;
    case K::Notice: log.push_back(e.text); break;
    case K::Error:
        errors.push_back(e.hint.empty() ? e.text : std::format("{}\n  {}", e.text, e.hint));
        log.push_back(e.text);
        break;
    case K::Data: break;
    }
    if (log.size() > 2000) log.erase(log.begin(), log.begin() + 500);
}

namespace {

fs::path home_dir_() {
    if (const char* h = std::getenv("XLINGS_HOME"); h && *h) return h;
    if (const char* u = std::getenv("HOME"); u && *u) return fs::path(u) / ".xlings";
    return ".xlings";
}

enum class View { Launcher, QuickStart, Job, Console, Off, Admin, Desktop };

struct Edition {
    std::string_view name;
    std::string_view title_en, title_zh;
    std::string_view about_en, about_zh;
};

constexpr std::array<Edition, 4> kQuick{{
    {"nano", "nano", "nano", "the smallest system: a shell", "最小的系统：一个 shell"},
    {"tiny", "tiny", "tiny", "a shell and the basic tools", "shell 与基础工具"},
    {"core", "core", "core", "a development system", "开发用的系统"},
    {"desktop", "desktop", "desktop", "a graphical desktop", "图形桌面"},
}};

struct App {
    fs::path home = home_dir_();
    std::unique_ptr<host::Window> window;
    service::Context ctx{.home = home, .port = service::Port(service::xlings_path())};
    AppSettings settings = read_app_settings(home);
    View view { View::Launcher };
    View before_admin { View::Launcher };
    std::vector<service::Environment> systems;
    std::string list_error;
    int selected { 0 };
    std::optional<std::chrono::steady_clock::time_point> countdown_until;
    std::unique_ptr<Job> job;
    std::function<void(App&)> after_job;
    std::unique_ptr<Console> console;
    std::unique_ptr<Display> display;  // a machine's desktop
    host::Window::Texture texture;
    opensal::Size desktop_size;        // what the window shows it at
    std::optional<std::chrono::steady_clock::time_point> resize_at;
    std::string current;               // the system on screen
    bool menu { false };               // the Host key's menu
    bool close_dialog { false };
    int close_choice { 0 };            // 0 background, 1 power off
    std::string notice;
    // Quick start
    char new_name[64] = {};
    int new_edition { 2 };
    int new_mode { 0 };
    bool more { false };
    // Admin
    int admin_selected { 0 };
    std::string attach;                // --attach
    bool dump_state { false };
    std::string input;                 // tests: typed into the console (or desktop) once it shows something
    std::optional<std::chrono::steady_clock::time_point> input_at;
    bool quit { false };

    void refresh() {
        auto l = service::list(ctx);
        if (!l) { list_error = l.error(); systems.clear(); return; }
        list_error.clear();
        systems = std::move(*l);
        // The default first, then the most recently used.
        std::ranges::stable_sort(systems, [&](const auto& a, const auto& b) {
            const bool da = a.name == settings.default_system, db = b.name == settings.default_system;
            if (da != db) return da;
            return settings.last_used[a.name] > settings.last_used[b.name];
        });
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(systems.size())));
    }

    std::string title_for(std::string_view what) const {
        return std::format("{} — Luban OS · Ctrl+Alt {}", what,
                           window->keyboard_grabbed() ? tr("releases the keyboard", "释放键盘") : tr("menu", "菜单"));
    }

    void start_job(std::string title, std::function<void(Job&, service::Context)> body, std::function<void(App&)> then) {
        job = std::make_unique<Job>();
        job->title = std::move(title);
        auto* j = job.get();
        auto* w = window.get();
        service::Context c = ctx;
        c.sink = [j, w](const service::Event& e) { j->take(e); w->wake(); };
        c.yes = true;
        after_job = std::move(then);
        job->worker = std::thread([j, w, c, body = std::move(body)] {
            body(*j, c);
            j->done = true;
            w->wake();
        });
        view = View::Job;
    }

    void enter(const std::string& name) {
        countdown_until.reset();
        settings.last_used[name] = xlings::observe::utc_now();
        write_app_settings(home, settings);
        current = name;
        auto env = service::describe(ctx, name);
        const auto mode = vm::running(home, name) ? std::string("machine") : env ? env->settings.mode : std::string("sandbox");
        if (mode == "machine") {
            start_job(std::format("{} {}", tr("Starting", "启动"), name),
                      [name](Job& j, service::Context c) { j.code = vm::start(c, name, {}); },
                      [name](App& a) {
                          if (a.job->code != 0) return;
                          a.open_machine(name);
                      });
        } else {
            open_console(name, false);
        }
    }

    // A machine shows its desktop when its root declares one (and its
    // setting is not `none`), else its console.
    void open_machine(const std::string& name) {
        auto env = service::describe(ctx, name);
        if (auto session = desktop_session(home, name, env ? env->settings.desktop : std::string{})) {
            std::string error;
            const auto [w, h] = window->size();
            desktop_size = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
            auto* win = window.get();
            display = Display::machine(home, name, *session, desktop_size, [win] { win->wake(); }, error);
            if (display) {
                view = View::Desktop;
                window->set_title(title_for(name));
                window->text_input(false);
                return;
            }
            notice = error;
        }
        open_console(name, true);
    }

    void close_display() {
        display.reset();
        if (texture.handle) window->destroy(texture);
        texture = {};
    }

    // The luban a sandbox runs for its desktop: the one beside this app, else the home's.
    fs::path luban_binary() const {
        std::error_code ec;
        for (const auto& p : {platform::get_executable_path().parent_path() / "luban", home / "bin" / "luban"})
            if (fs::is_regular_file(p, ec)) return p;
        return {};
    }

    void open_console(const std::string& name, bool machine) {
        std::string error;
        auto* w = window.get();
        auto env = service::describe(ctx, name);
        // A sandbox shows its desktop the same way a machine does: the
        // session, bridged, on two pipes instead of a channel.
        if (!machine) {
            const auto luban = luban_binary();
            if (auto session = desktop_session(home, name, env ? env->settings.desktop : std::string{}); session && !luban.empty()) {
                const auto [ww, wh] = window->size();
                desktop_size = {static_cast<std::uint32_t>(ww), static_cast<std::uint32_t>(wh)};
                display = Display::sandbox(ctx.port.xlings(), luban, home, name, *session, env ? env->settings.persist : "all",
                                           desktop_size, [w] { w->wake(); }, error);
                if (display) {
                    view = View::Desktop;
                    window->set_title(title_for(name));
                    window->text_input(false);
                    return;
                }
                notice = error;
            }
        }
        console = machine ? Console::machine(home, name, [w] { w->wake(); }, error)
                          : Console::sandbox(ctx.port.xlings(), home, name, env ? env->settings.persist : "all", [w] { w->wake(); }, error);
        if (!console) {
            notice = error;
            view = View::Launcher;
            return;
        }
        view = View::Console;
        window->set_title(title_for(name));
        window->text_input(true);
    }

    void power_off(const std::string& name) {
        start_job(std::format("{} {}", tr("Powering off", "关闭"), name),
                  [name](Job& j, service::Context c) { j.code = vm::stop(c, name).code; },
                  [](App& a) { a.view = View::Off; });
    }
};

// ── views ───────────────────────────────────────────────────────────────

void begin_full_(const char* id) {
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                  | ImGuiWindowFlags_NoBringToFrontOnFocus);
}

void center_text_(std::string_view text, float scale = 1.0f) {
    const auto w = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x * scale;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) / 2);
    ImGui::SetWindowFontScale(scale);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::SetWindowFontScale(1.0f);
}

std::string edition_label_(const service::Environment& e) {
    auto label = e.edition.empty() ? e.kind : e.edition;
    if (label.starts_with("subos:luban-")) label = label.substr(12);
    if (const auto at = label.find('@'); at != std::string::npos) label = label.substr(0, at);
    return label;
}

void launcher_(App& a) {
    begin_full_("launcher");
    // The gear: the management panel (drawn: not every font has the glyph).
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 54, 14));
    if (ImGui::InvisibleButton("admin", ImVec2(36, 36))) { a.before_admin = a.view; a.view = View::Admin; }
    {
        const auto mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const ImVec2 c((mn.x + mx.x) / 2, (mn.y + mx.y) / 2);
        const ImU32 col = ImGui::IsItemHovered() ? IM_COL32(230, 235, 242, 255) : IM_COL32(150, 160, 175, 255);
        auto* dl = ImGui::GetWindowDrawList();
        for (int k = 0; k < 8; ++k) {
            const float t = k * 3.14159265f / 4;
            dl->AddLine(ImVec2(c.x + std::cos(t) * 7, c.y + std::sin(t) * 7), ImVec2(c.x + std::cos(t) * 12, c.y + std::sin(t) * 12), col, 3.5f);
        }
        dl->AddCircle(c, 8, col, 24, 3.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", std::string(tr("Manage (F2)", "管理（F2）")).c_str());
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.18f);
    center_text_("Luban OS", 2.2f);
    ImGui::Dummy(ImVec2(0, 24));
    const float listWidth = std::min(560.0f, ImGui::GetWindowWidth() - 80);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - listWidth) / 2);
    ImGui::BeginChild("systems", ImVec2(listWidth, ImGui::GetWindowHeight() * 0.5f));
    const int rows = static_cast<int>(a.systems.size()) + 1;
    const float lineH = ImGui::GetTextLineHeight();
    for (int i = 0; i < rows; ++i) {
        const bool last = i == rows - 1;
        ImGui::PushID(i);
        if (ImGui::Selectable("##row", a.selected == i, 0, ImVec2(0, 36))) {
            a.selected = i;
            a.countdown_until.reset();
            if (last) a.view = View::QuickStart;
            else a.enter(a.systems[i].name);
        }
        const auto mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float y = (mn.y + mx.y - lineH) / 2;
        auto* dl = ImGui::GetWindowDrawList();
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        if (last) {
            const std::string label = std::string("+  ") + std::string(tr("New system", "新系统"));
            dl->AddText(ImVec2(mn.x + 16, y), dim, label.c_str());
        } else {
            const auto& e = a.systems[i];
            dl->AddText(ImVec2(mn.x + 16, y), text, e.name.c_str());
            const auto label = edition_label_(e);
            dl->AddText(ImVec2(mn.x + 200, y), dim, label.c_str());
            if (e.running || vm::running(a.home, e.name))
                dl->AddCircleFilled(ImVec2(mx.x - 22, (mn.y + mx.y) / 2), 5, IM_COL32(102, 217, 128, 255));
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (a.countdown_until && !a.systems.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(*a.countdown_until - std::chrono::steady_clock::now()).count() + 1;
        center_text_(std::format("{}", std::vformat(tr("{} s, then {}", "{} 秒后进入 {}"), std::make_format_args(left, a.systems[0].name))));
    }
    if (!a.notice.empty()) center_text_(a.notice);
    if (!a.list_error.empty()) center_text_(a.list_error);
    ImGui::End();
}

void quick_start_(App& a) {
    begin_full_("quick");
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.12f);
    center_text_(tr("Welcome to Luban OS", "欢迎使用 Luban OS"), 1.6f);
    center_text_(tr("Pick a system: one click makes it and goes in", "选一个系统，点一下就创建并进入"));
    ImGui::Dummy(ImVec2(0, 20));
    const float card = 180, gap = 16;
    const float total = card * kQuick.size() + gap * (kQuick.size() - 1);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total) / 2);
    for (std::size_t i = 0; i < kQuick.size(); ++i) {
        if (i) ImGui::SameLine(0, gap);
        const auto& e = kQuick[i];
        ImGui::PushID(static_cast<int>(i));
        const bool chosen = a.new_edition == static_cast<int>(i);
        if (chosen) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        const auto text = std::format("{}\n\n{}", tr(e.title_en, e.title_zh), tr(e.about_en, e.about_zh));
        if (ImGui::Button(text.c_str(), ImVec2(card, 120))) a.new_edition = static_cast<int>(i);
        if (chosen) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 16));
    if (a.new_name[0] == 0) {
        const auto base = std::string(kQuick[a.new_edition].name);
        for (int k = 1; k < 100; ++k) {
            const auto candidate = std::format("{}-{}", base, k);
            if (std::ranges::none_of(a.systems, [&](const auto& s) { return s.name == candidate; })) {
                std::snprintf(a.new_name, sizeof a.new_name, "%s", candidate.c_str());
                break;
            }
        }
    }
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 420) / 2);
    ImGui::SetNextItemWidth(220);
    ImGui::InputText(std::string(tr("name", "名字")).c_str(), a.new_name, sizeof a.new_name);
    ImGui::SameLine();
    ImGui::Checkbox(std::string(tr("more", "更多选项")).c_str(), &a.more);
    if (a.more) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 420) / 2);
        const char* modes[] = {"sandbox", "machine"};
        ImGui::SetNextItemWidth(220);
        ImGui::Combo(std::string(tr("runs as", "运行方式")).c_str(), &a.new_mode, modes, 2);
    }
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 200) / 2);
    if (ImGui::Button(std::string(tr("Make it and go in", "创建并进入")).c_str(), ImVec2(200, 40))) {
        const std::string name = a.new_name;
        const std::string edition(kQuick[a.new_edition].name);
        const bool machine = a.new_mode == 1;
        a.start_job(std::format("{} {}", tr("Making", "创建"), name),
                    [name, edition, machine](Job& j, service::Context c) {
                        auto out = service::create(c, {.name = name, .edition = edition});
                        j.code = out.code;
                        if (out.ok() && machine) (void)service::configure(c, name, {{"mode", "machine"}});
                    },
                    [name](App& app) {
                        if (app.job->code != 0) return;
                        app.refresh();
                        app.enter(name);
                    });
    }
    if (!a.systems.empty()) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 200) / 2);
        if (ImGui::Button(std::string(tr("Back", "返回")).c_str(), ImVec2(200, 32))) a.view = View::Launcher;
    }
    ImGui::End();
}

void job_(App& a) {
    begin_full_("job");
    auto& j = *a.job;
    std::lock_guard l(j.m);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.2f);
    center_text_(j.title, 1.5f);
    ImGui::Dummy(ImVec2(0, 16));
    const float width = std::min(620.0f, ImGui::GetWindowWidth() - 80);
    const float x = (ImGui::GetWindowWidth() - width) / 2;
    for (const auto& p : j.phases) {
        ImGui::SetCursorPosX(x);
        const auto* mark = p.state == "done" ? "✓" : p.state == "failed" ? "✗" : "▸";
        const ImVec4 color = p.state == "done" ? ImVec4(0.4f, 0.85f, 0.5f, 1) : p.state == "failed" ? ImVec4(0.95f, 0.4f, 0.4f, 1) : ImVec4(0.4f, 0.7f, 1, 1);
        ImGui::TextColored(color, "%s", mark);
        ImGui::SameLine();
        ImGui::TextUnformatted(p.text.c_str());
    }
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::SetCursorPosX(x);
    const float fraction = j.percent >= 0 ? static_cast<float>(j.percent) : static_cast<float>(std::fmod(ImGui::GetTime() * 0.4, 1.0));
    ImGui::ProgressBar(fraction, ImVec2(width, 8), "");
    ImGui::SetCursorPosX(x);
    ImGui::TextDisabled("%s", j.current.c_str());
    for (const auto& e : j.errors) {
        ImGui::SetCursorPosX(x);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1), "%s", e.c_str());
    }
    ImGui::SetCursorPosX(x);
    if (ImGui::CollapsingHeader(std::string(tr("Details", "详情")).c_str())) {
        ImGui::SetCursorPosX(x);
        ImGui::BeginChild("log", ImVec2(width, 200), ImGuiChildFlags_Borders);
        for (const auto& line : j.log) ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    if (j.done && j.code != 0) {
        ImGui::SetCursorPosX(x);
        if (ImGui::Button(std::string(tr("Back to the list", "返回列表")).c_str())) a.view = View::Launcher;
    }
    ImGui::End();
}

void off_(App& a) {
    begin_full_("off");
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.4f);
    center_text_(std::vformat(tr("{} is off", "{} 已关机"), std::make_format_args(a.current)), 1.4f);
    center_text_(tr("Enter starts it again · Esc goes back to the list", "Enter 重新启动 · Esc 返回列表"));
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Enter") a.enter(a.current);
        else if (k.name == "Escape") { a.view = View::Launcher; a.refresh(); a.window->set_title("Luban OS"); }
    }
}

void close_dialog_(App& a) {
    ImGui::OpenPopup("close");
    if (ImGui::BeginPopupModal("close", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        auto env = service::describe(a.ctx, a.current);
        const bool temp = false;
        ImGui::TextUnformatted(std::vformat(tr("Close {}", "关闭 {}"), std::make_format_args(a.current)).c_str());
        ImGui::Separator();
        ImGui::RadioButton(std::string(tr("Keep it running in the background", "在后台继续运行")).c_str(), &a.close_choice, 0);
        ImGui::RadioButton(temp ? std::string(tr("Power off (what it wrote goes)", "关机（修改将丢弃）")).c_str()
                                : std::string(tr("Power off", "关机")).c_str(), &a.close_choice, 1);
        ImGui::Dummy(ImVec2(0, 8));
        bool decided = false, as_default = false;
        if (ImGui::Button(std::string(tr("Cancel", "取消")).c_str())) { a.close_dialog = false; a.window->clear_close_request(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(std::string(tr("Confirm", "确认")).c_str())) decided = true;
        ImGui::SameLine();
        if (ImGui::Button(std::string(tr("Confirm and make it the default", "确认并设为默认")).c_str())) decided = as_default = true;
        if (decided) {
            const bool off = a.close_choice == 1;
            if (as_default) (void)service::set_setting(a.home, a.current, "on-close", off ? "poweroff" : "background");
            a.close_dialog = false;
            ImGui::CloseCurrentPopup();
            if (off) {
                service::Context c = a.ctx;
                (void)vm::stop(c, a.current);
            }
            a.console.reset();
            a.close_display();
            a.quit = true;
        }
        ImGui::EndPopup();
    }
}

// The Host key's menu, over the system.
void host_menu_(App& a) {
    ImGui::OpenPopup("host");
    if (ImGui::BeginPopupModal("host", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(a.current.c_str());
        ImGui::Separator();
        auto item = [&](std::string_view en, std::string_view zh) { return ImGui::Selectable(std::string(tr(en, zh)).c_str()); };
        if (item("Back to the list", "回到列表")) {
            a.menu = false;
            a.console.reset();
            a.close_display();
            a.view = View::Launcher;
            a.refresh();
            a.window->set_title("Luban OS");
        }
        if (item("Manage", "管理")) { a.menu = false; a.before_admin = a.view; a.view = View::Admin; }
        if (item(a.window->fullscreen() ? "Leave full screen" : "Full screen", a.window->fullscreen() ? "退出全屏" : "全屏")) {
            a.window->set_fullscreen(!a.window->fullscreen());
            a.menu = false;
        }
        if (item("New window", "新窗口")) {
            const auto self = platform::get_executable_path();
            (void)platform::spawn_detached({self.string(), "--attach", a.current}, a.home / "logs" / "luban-os.log");
            a.menu = false;
        }
        if (((a.console && a.console->is_machine()) || a.display) && item("Power off", "关机")) {
            a.menu = false;
            a.console.reset();
            a.close_display();
            a.power_off(a.current);
        }
        if (item("Close this menu", "关闭菜单") || ImGui::IsKeyPressed(ImGuiKey_Escape)) a.menu = false;
        if (!a.menu) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void console_view_(App& a) {
    begin_full_("console");
    if (!a.console) { ImGui::End(); return; }
    // Keys the program gets: text from the input method; named keys and
    // control chords as xterm sends them.
    if (!a.menu && !a.close_dialog) {
        bool app_cursor, bracketed;
        {
            std::lock_guard l(a.console->lock());
            app_cursor = a.console->screen().application_cursor();
            bracketed = a.console->screen().bracketed_paste();
        }
        for (const auto& e : a.window->events())
            if (auto* t = std::get_if<opensal::TextCommit>(&e)) a.console->write(t->text);
        for (const auto& k : a.window->keys()) {
            const auto mods = k.key.mods;
            if ((mods & opensal::Ctrl) && (mods & opensal::Shift) && (k.name == "V" || k.name == "v")) {
                a.console->write(term::encode_paste(a.window->clipboard(), bracketed));
                continue;
            }
            const bool printable = k.name.size() == 1;
            if (printable && !(mods & (opensal::Ctrl | opensal::Alt))) continue;   // came as text
            a.console->write(term::encode_key(k.name, mods & (opensal::Shift | opensal::Ctrl | opensal::Alt), app_cursor));
        }
    }
    const auto [rows, cols] = draw_console(*a.console, a.window->mono_font(), a.window->mono_size(), !a.menu);
    a.console->resize(rows, cols);
    ImGui::End();
    if (!a.console->alive()) {
        const bool machine = a.console->is_machine();
        a.console.reset();
        if (machine && vm::running(a.home, a.current)) {
            a.notice = std::vformat(tr("{} keeps running", "{} 仍在运行"), std::make_format_args(a.current));
            a.view = View::Launcher;
            a.refresh();
        } else {
            a.view = View::Off;
        }
        a.window->set_title("Luban OS");
    }
}

// Tests: sends `input` up to its next chord (or all of it) to the desktop as
// key presses; returns what is left.
std::string type_desktop_(Display& d, std::string_view input) {
    auto press = [&](std::uint32_t sym, std::uint32_t mods) {
        d.send(opensal::Key{true, sym, mods});
        d.send(opensal::Key{false, sym, mods});
    };
    static constexpr std::array<std::pair<std::string_view, std::uint32_t>, 6> names{{
        {"Return", 0xff0d}, {"Tab", 0xff09}, {"Escape", 0xff1b}, {"BackSpace", 0xff08}, {"Up", 0xff52}, {"Down", 0xff54}}};
    std::size_t k = 0;
    if (input.starts_with('{')) {
        const auto end = input.find('}');
        if (end == std::string_view::npos) return {};
        auto chord = input.substr(1, end - 1);
        std::uint32_t mods = 0;
        std::vector<std::uint32_t> held;
        for (auto [name, bit, sym] : {std::tuple{"Alt+", opensal::Alt, 0xffe9u}, {"Ctrl+", opensal::Ctrl, 0xffe3u},
                                      {"Shift+", opensal::Shift, 0xffe1u}, {"Super+", opensal::Super, 0xffebu}})
            if (chord.starts_with(name)) { mods |= bit; held.push_back(sym); chord.remove_prefix(std::string_view(name).size()); }
        for (auto sym : held) d.send(opensal::Key{true, sym, mods});
        std::uint32_t key = chord.size() == 1 ? static_cast<unsigned char>(chord[0]) : 0;
        for (const auto& [n, sym] : names) if (chord == n) key = sym;
        if (key) press(key, mods);
        for (auto sym : held) d.send(opensal::Key{false, sym, 0});
        return std::string(input.substr(end + 1));
    }
    for (; k < input.size() && input[k] != '{'; ++k) {
        if (input[k] == '\\' && k + 1 < input.size() && input[k + 1] == 'n') { press(0xff0d, 0); ++k; continue; }
        const auto c = static_cast<unsigned char>(input[k]);
        press(c, std::isupper(c) ? opensal::Shift : 0);
    }
    return std::string(input.substr(k));
}

// A machine's desktop: its surface drawn whole (1:1 when it fits), and the
// window's input given to it -- the pointer in the surface's pixels.
void desktop_view_(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    begin_full_("desktop");
    ImGui::PopStyleVar();
    if (!a.display) { ImGui::End(); return; }
    auto& d = *a.display;
    std::vector<Display::Update> updates;
    const auto size = d.take(updates);
    if (size.width && size.height
        && (a.texture.width != static_cast<int>(size.width) || a.texture.height != static_cast<int>(size.height))) {
        if (a.texture.handle) a.window->destroy(a.texture);
        a.texture = a.window->texture(static_cast<int>(size.width), static_cast<int>(size.height));
    }
    for (const auto& u : updates)
        if (a.texture.handle && u.rect.x + u.rect.width <= static_cast<std::uint32_t>(a.texture.width)
            && u.rect.y + u.rect.height <= static_cast<std::uint32_t>(a.texture.height))
            a.window->update(a.texture, u.rect, u.rows);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (a.texture.handle) {
        const float scale = std::min({1.0f, avail.x / a.texture.width, avail.y / a.texture.height});
        const ImVec2 shown(a.texture.width * scale, a.texture.height * scale);
        const ImVec2 at(origin.x + (avail.x - shown.x) / 2, origin.y + (avail.y - shown.y) / 2);
        ImGui::SetCursorScreenPos(at);
        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(a.texture.handle)), shown);
        // The guest draws its own pointer into the picture.
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        if (!a.menu && !a.close_dialog) {
            for (const auto& e : a.window->events()) {
                if (const auto* m = std::get_if<opensal::PointerMotion>(&e))
                    d.send(opensal::PointerMotion{(m->x - at.x) / scale, (m->y - at.y) / scale});
                else if (std::holds_alternative<opensal::Key>(e) || std::holds_alternative<opensal::PointerButton>(e)
                         || std::holds_alternative<opensal::Wheel>(e) || std::holds_alternative<opensal::Focus>(e))
                    d.send(e);
            }
        }
    } else {
        ImGui::SetCursorPosY(avail.y * 0.45f);
        center_text_(std::vformat(tr("Starting the desktop of {}…", "正在启动 {} 的桌面…"), std::make_format_args(a.current)));
    }
    // The window's size is the desktop's, once it has held still a moment.
    const opensal::Size want{static_cast<std::uint32_t>(avail.x), static_cast<std::uint32_t>(avail.y)};
    if (want != a.desktop_size) { a.desktop_size = want; a.resize_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(300); }
    if (a.resize_at && std::chrono::steady_clock::now() >= *a.resize_at) { d.send(opensal::Resized{want}); a.resize_at.reset(); }
    ImGui::End();
    if (!d.alive()) {
        // A desktop that could not start (or ended) leaves the machine's console.
        const auto why = d.error();
        const bool machine = d.is_machine();
        a.close_display();
        if (machine && !vm::running(a.home, a.current)) {
            a.view = View::Off;
        } else if (machine) {
            a.notice = why;
            a.open_console(a.current, true);
            if (a.console) a.notice.clear();
        } else {
            // A sandbox's desktop that ended is the end of that session.
            a.notice = why;
            a.view = View::Launcher;
            a.refresh();
            a.window->set_title("Luban OS");
        }
    }
}

void admin_(App& a) {
    begin_full_("admin");
    if (ImGui::Button(std::string(tr("← Back", "← 返回")).c_str())) {
        a.view = a.display ? View::Desktop : a.console ? View::Console : View::Launcher;
        a.refresh();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(std::string(tr("Luban OS · Manage", "Luban OS · 管理")).c_str());
    ImGui::Separator();
    if (ImGui::BeginTabBar("admin")) {
        if (ImGui::BeginTabItem(std::string(tr("Systems", "系统")).c_str())) {
            ImGui::BeginChild("list", ImVec2(220, 0), ImGuiChildFlags_Borders);
            for (int i = 0; i < static_cast<int>(a.systems.size()); ++i)
                if (ImGui::Selectable(a.systems[i].name.c_str(), a.admin_selected == i)) a.admin_selected = i;
            if (ImGui::Selectable(std::string(tr("＋ New", "＋ 新建")).c_str(), false)) a.view = View::QuickStart;
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("detail");
            if (a.admin_selected < static_cast<int>(a.systems.size())) {
                auto& e = a.systems[a.admin_selected];
                const bool on = vm::running(a.home, e.name).has_value();
                ImGui::Text("%s", e.name.c_str());
                ImGui::TextDisabled("%s · %zu %s%s", edition_label_(e).c_str(), e.packages, std::string(tr("packages", "个包")).c_str(),
                                    e.generation ? std::format(" · {} {}", tr("generation", "第"), *e.generation).c_str() : "");
                ImGui::Separator();
                auto setting = [&](const char* key, std::string_view label, const std::vector<std::string_view>& values, const std::string& now) {
                    int index = 0;
                    for (std::size_t k = 0; k < values.size(); ++k) if (values[k] == now) index = static_cast<int>(k);
                    std::string items;
                    for (auto v : values) { items += v; items += '\0'; }
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::Combo(std::string(label).c_str(), &index, items.c_str())) {
                        (void)service::configure(a.ctx, e.name, {{key, std::string(values[index])}});
                        a.refresh();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("luban config %s --%s %s", e.name.c_str(), key, std::string(values[index]).c_str());
                };
                setting("mode", tr("runs as", "运行方式"), {"sandbox", "machine"}, e.settings.mode);
                setting("persist", tr("keeps", "保留修改"), {"all", "home", "none"}, e.settings.persist);
                setting("on-close", tr("closing", "关窗口时"), {"ask", "background", "poweroff"}, e.settings.on_close);
                {
                    // The desktop: the sessions its root declares, or none (its console).
                    std::vector<std::string> ids{"", "none"};
                    if (const auto root = service::root_of(a.home, e.name))
                        for (const auto& s : luban::desktop::wlr::sessions({*root / "usr" / "share" / "wayland-sessions"})) ids.push_back(s.id);
                    int index = 0;
                    for (std::size_t k = 0; k < ids.size(); ++k) if (ids[k] == e.settings.desktop) index = static_cast<int>(k);
                    std::string items = std::string(tr("(the first it declares)", "（它声明的第一个）")) + '\0';
                    for (std::size_t k = 1; k < ids.size(); ++k) { items += ids[k]; items += '\0'; }
                    ImGui::SetNextItemWidth(180);
                    if (ImGui::Combo(std::string(tr("desktop", "桌面")).c_str(), &index, items.c_str())) {
                        (void)service::configure(a.ctx, e.name, {{"desktop", ids[index]}});
                        a.refresh();
                    }
                    if (!ids[index].empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("luban config %s --desktop %s", e.name.c_str(), ids[index].c_str());
                    }
                }
                if (!e.shell.empty()) ImGui::Text("%s  %s", std::string(tr("shell", "shell")).c_str(), e.shell.c_str());
                ImGui::Separator();
                if (ImGui::Button(std::string(tr("Enter", "进入")).c_str())) a.enter(e.name);
                ImGui::SameLine();
                if (on && ImGui::Button(std::string(tr("Power off", "关机")).c_str())) a.power_off(e.name);
                ImGui::SameLine();
                if (ImGui::Button(std::string(tr("Make it the default", "设为默认")).c_str())) {
                    a.settings.default_system = e.name;
                    write_app_settings(a.home, a.settings);
                    a.refresh();
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1));
                if (ImGui::Button(std::string(tr("Remove…", "删除…")).c_str())) ImGui::OpenPopup("remove");
                ImGui::PopStyleColor();
                if (ImGui::BeginPopupModal("remove", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted(std::vformat(tr("Remove {} and every file in it?", "删除 {} 以及其中的所有文件？"),
                                                        std::make_format_args(e.name)).c_str());
                    if (ImGui::Button(std::string(tr("Remove", "删除")).c_str())) {
                        const auto name = e.name;
                        ImGui::CloseCurrentPopup();
                        a.start_job(std::format("{} {}", tr("Removing", "删除"), name),
                                    [name](Job& j, service::Context c) { j.code = service::remove(c, name).code; },
                                    [](App& app) { app.refresh(); app.view = View::Admin; });
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(std::string(tr("Cancel", "取消")).c_str())) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(std::string(tr("Activity", "活动")).c_str())) {
            std::vector<fs::path> files;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(a.home / "logs" / "ops", ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == ".ndjson") files.push_back(it->path());
            std::ranges::sort(files, [](const auto& x, const auto& y) {
                std::error_code e1, e2;
                return fs::last_write_time(x, e1) > fs::last_write_time(y, e2);
            });
            if (ImGui::BeginTable("ops", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                for (std::size_t k = 0; k < std::min<std::size_t>(files.size(), 40); ++k) {
                    std::ifstream in(files[k]);
                    std::string first;
                    std::getline(in, first);
                    auto j = Json::parse(first, nullptr, false);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(j.is_object() ? j.value("ts", "").c_str() : "");
                    ImGui::TableNextColumn();
                    std::string cmd = j.is_object() ? j.value("client", "") : "";
                    if (j.is_object()) for (const auto& w : j.value("argv", Json::array())) cmd += " " + w.get<std::string>();
                    ImGui::TextUnformatted(cmd.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("xlings self log %s", files[k].stem().string().substr(0, 12).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(std::string(tr("Settings", "设置")).c_str())) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::SliderInt(std::string(tr("countdown (s, 0: none)", "倒计时（秒，0 为不自动进入）")).c_str(), &a.settings.countdown, 0, 60))
                write_app_settings(a.home, a.settings);
            ImGui::Text("%s  %s", std::string(tr("default", "默认系统")).c_str(),
                        a.settings.default_system.empty() ? std::string(tr("(the most recently used)", "（最近使用的）")).c_str()
                                                          : a.settings.default_system.c_str());
            ImGui::TextDisabled("%s", std::string(tr("Host key: Ctrl+Alt pressed and released alone", "Host 键：单独按下再松开 Ctrl+Alt")).c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

std::string dump_(const App& a) {
    static constexpr std::array<std::string_view, 7> names{"launcher", "quick-start", "job", "console", "off", "admin", "desktop"};
    Json j{{"view", std::string(names[static_cast<int>(a.view)])}, {"systems", Json::array()}};
    for (const auto& s : a.systems) j["systems"].push_back(s.name);
    if (a.countdown_until) j["countdown_to"] = a.systems.empty() ? "" : a.systems[0].name;
    return j.dump();
}

}  // namespace

int run(int argc, char* argv[]) {
    App a;
    host::Options o;
    int frames = -1;
    std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--attach" && i + 1 < argc) a.attach = argv[++i];
        else if (arg == "--headless") { o.hidden = true; platform::set_env_variable("SDL_VIDEODRIVER", "offscreen"); }
        else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (arg == "--dump-state") a.dump_state = true;
        else if (arg == "--input" && i + 1 < argc) a.input = argv[++i];
        else if (arg == "--view" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (v == "admin") { a.before_admin = View::Launcher; a.view = View::Admin; }
            else if (v == "quick-start") a.view = View::QuickStart;
        }
        else if (arg == "--version") { std::println("luban-os {}", service::kVersion); return 0; }
        else if (arg == "--help" || arg == "-h") {
            std::println("luban-os [--attach <name>]   Luban OS: the systems of this home, in a window");
            return 0;
        }
    }
    auto window = host::Window::open(o);
    if (!window) {
        std::println(std::cerr, "luban-os: {}", window.error());
        return 1;
    }
    a.window = std::move(*window);
    a.window->set_title("Luban OS");
    if (auto refused = a.ctx.port.handshake()) a.notice = *refused;
    a.refresh();
    if (!a.attach.empty()) a.enter(a.attach);
    else if (a.view == View::Admin || a.view == View::QuickStart) {}
    else if (a.systems.empty()) a.view = View::QuickStart;
    else if (a.settings.countdown > 0) a.countdown_until = std::chrono::steady_clock::now() + std::chrono::seconds(a.settings.countdown);
    for (int frame = 0; (frames < 0 || frame < frames) && !a.quit; ++frame) {
        const bool busy = a.countdown_until || (a.job && !a.job->done) || frames >= 0 || a.resize_at;
        if (!a.window->begin_frame(busy ? 100 : 1000) && !a.close_dialog) {
            // The window is to close: a running machine's on-close decides.
            // A sandbox's session ends with its window; only a machine has a choice.
            if ((a.view == View::Console && a.console && a.console->is_machine())
                || (a.view == View::Desktop && a.display && a.display->is_machine())) {
                auto env = service::describe(a.ctx, a.current);
                const auto on_close = env ? env->settings.on_close : std::string("ask");
                if (on_close == "ask") { a.close_dialog = true; }
                else {
                    if (on_close == "poweroff") (void)vm::stop(a.ctx, a.current);
                    break;
                }
            } else {
                break;
            }
        }
        // Any input cancels the countdown.
        if (a.countdown_until) {
            const bool input = std::ranges::any_of(a.window->events(), [](const auto& e) {
                return std::holds_alternative<opensal::Key>(e) || std::holds_alternative<opensal::PointerButton>(e)
                    || std::holds_alternative<opensal::PointerMotion>(e);
            });
            if (input) a.countdown_until.reset();
            else if (std::chrono::steady_clock::now() >= *a.countdown_until && !a.systems.empty()) a.enter(a.systems[0].name);
        }
        if (a.window->host_key() && (a.view == View::Console || a.view == View::Off || a.view == View::Desktop)) a.menu = !a.menu;
        for (const auto& k : a.window->keys())
            if (k.name == "F2" && a.view != View::Console && a.view != View::Desktop) { a.before_admin = a.view; a.view = View::Admin; }
        if (a.view == View::Launcher) {
            for (const auto& k : a.window->keys()) {
                const int rows = static_cast<int>(a.systems.size()) + 1;
                if (k.name == "Down") a.selected = (a.selected + 1) % rows;
                else if (k.name == "Up") a.selected = (a.selected + rows - 1) % rows;
                else if (k.name == "Enter") {
                    if (a.selected >= static_cast<int>(a.systems.size())) a.view = View::QuickStart;
                    else a.enter(a.systems[a.selected].name);
                }
            }
        }
        if (!a.input.empty() && a.view == View::Console && a.console) {
            bool shown = false;
            {
                std::lock_guard l(a.console->lock());
                shown = a.console->screen().cursor_col() > 0 || a.console->screen().cursor_row() > 0;
            }
            if (shown) {
                std::string text;
                for (std::size_t k = 0; k < a.input.size(); ++k) {
                    if (a.input[k] == '\\' && k + 1 < a.input.size() && a.input[k + 1] == 'n') { text += '\r'; ++k; }
                    else text += a.input[k];
                }
                a.console->write(text);
                a.input.clear();
            }
        }
        // Tests: typed into the desktop once it shows something -- characters,
        // `\n` for Return, `{Alt+Return}`-style chords -- a second apart per chord.
        if (!a.input.empty() && a.view == View::Desktop && a.display && a.texture.handle) {
            if (!a.input_at) a.input_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            if (std::chrono::steady_clock::now() >= *a.input_at) {
                a.input = type_desktop_(*a.display, a.input);
                a.input_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
        }
        if (a.view == View::Job && a.job && a.job->done && a.job->code == 0 && a.after_job) {
            auto then = std::move(a.after_job);
            a.after_job = nullptr;
            then(a);
        }
        switch (a.view) {
        case View::Launcher: launcher_(a); break;
        case View::QuickStart: quick_start_(a); break;
        case View::Job: job_(a); break;
        case View::Console: console_view_(a); break;
        case View::Off: off_(a); break;
        case View::Admin: admin_(a); break;
        case View::Desktop: desktop_view_(a); break;
        }
        if (a.menu) host_menu_(a);
        if (a.close_dialog) close_dialog_(a);
        a.window->end_frame();
    }
    if (a.dump_state) std::println("{}", dump_(a));
    if (!screenshot.empty()) {
        int w = 0, h = 0;
        const auto px = a.window->read_pixels(w, h);
        std::ofstream out(screenshot, std::ios::binary);
        out << std::format("P6\n{} {}\n255\n", w, h);
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) out << static_cast<char>(px[i + 2]) << static_cast<char>(px[i + 1]) << static_cast<char>(px[i]);
    }
    return 0;
}

}  // namespace luban::os
