module luban.os;

import std;
import imkit;
import xlings.subos.home_view;
import xlings.subos.located;
import xlings.subos.policy_store;
import xlings.subos.policy;
import xlings.subos.root_etc;
import luban.service.archive;
import xlings.libs.json;
import xlings.libs.sha512;
import xlings.observe;
import xlings.platform;
import xlings.platform.target;
import opensal;
import luban.service;
import luban.vm;
import luban.desktop.host;
import xdesk.term;
import opensal.wlr;
import tray;

namespace luban::os {

namespace platform = xlings::platform;
namespace host = luban::desktop::host;
using Json = nlohmann::json;

namespace {
std::string& language_() {
    static std::string lang = "auto";
    return lang;
}
}  // namespace

void set_language(std::string_view lang) { language_() = std::string(lang); }

std::string_view tr(std::string_view en, std::string_view zh) {
    static const bool locale_chinese = [] {
        for (const auto* v : {"LUBAN_OS_LANG", "LC_ALL", "LC_MESSAGES", "LANG"})
            if (const char* l = std::getenv(v); l && *l) return std::string_view(l).starts_with("zh");
        return false;
    }();
    // LUBAN_OS_LANG (tests, a person's override) wins over the setting.
    static const bool forced = [] { const char* l = std::getenv("LUBAN_OS_LANG"); return l && *l; }();
    const auto& lang = language_();
    const bool chinese = forced || lang == "auto" ? locale_chinese : lang == "zh";
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
    s.language = j.value("language", std::string("auto"));
    if (s.language != "en" && s.language != "zh") s.language = "auto";
    s.reduce_motion = j.value("reduce_motion", false);
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
           {"language", s.language}, {"reduce_motion", s.reduce_motion}, {"console_size", s.console_size},
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
    switch (e.kind) {
    case K::Phase: {
        auto it = std::ranges::find(phases, e.phase, &Phase::name);
        if (it == phases.end()) phases.push_back({e.phase, e.text, e.state});
        else { it->state = e.state; if (!e.text.empty()) it->text = e.text; }
        current = e.text;
        if (e.state == "begin") percent = -1;
        if (!e.phase.empty()) stage = e.phase;
        break;
    }
    case K::Progress:
        if (e.percent >= 0) percent = e.percent;
        if (!e.text.empty()) current = e.text;
        if (!e.phase.empty()) stage = e.phase;
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
            if (total > 0) { percent = done_bytes / total; stage = "download"; }
        }
        break;
    }
    if (log.size() > 2000) log.erase(log.begin(), log.begin() + 500);
}

namespace {

fs::path home_dir_() {
    if (const char* h = std::getenv("XLINGS_HOME"); h && *h) return h;
    if (const char* u = std::getenv("HOME"); u && *u) return fs::path(u) / ".xlings";
    return ".xlings";
}

enum class View { Launcher, QuickStart, Job, Console, Off, Admin, Desktop, Import, Run };

// The editions one can make, in two groups. What each is ships with Luban;
// whether it can be had here (published, for this machine) the index answers.
struct Edition {
    std::string_view name;                  // `luban new --edition`
    bool desktop;                           // the desktop group
    std::string_view title_en, title_zh;
    std::string_view about_en, about_zh;    // one line
    std::string_view detail_en, detail_zh;  // what is in it, and for whom
    std::string_view size;                  // about what it downloads
    std::string_view contents;              // its first few programs
    bool goes_in;                           // there is something to log into
};

constexpr std::array<Edition, 7> kEditions{{
    {"nano", false, "nano", "nano", "No shell: commands only", "没有 shell：只运行命令",
     "The base an edition of your own is built on: xlings and luban, nothing to log into.",
     "自己定制 edition 的基础：只有 xlings 与 luban，没有可以登录的界面。", "~15 MB", "xlings · luban", false},
    {"tiny", false, "tiny", "tiny", "A shell and the basic tools", "shell 与基础工具",
     "BusyBox and xlings: small and quick to make; add what you need with xlings install.",
     "BusyBox 与 xlings：小，创建快；需要什么用 xlings install 添加。", "~25 MB", "busybox · xlings · luban", true},
    {"tiny-musl", false, "tiny-musl", "tiny-musl", "tiny, on musl", "基于 musl 的 tiny",
     "tiny with musl as its C library: for building and testing static, musl-linked programs.",
     "以 musl 为 C 库的 tiny：构建与测试 musl 静态程序。", "~25 MB", "busybox · musl · xlings", true},
    {"core", false, "core", "core", "A console to develop in", "开发用的控制台",
     "glibc, fish and bash, gcc, git, curl and mcpp: a console for development.",
     "glibc、fish 与 bash、gcc、git、curl 与 mcpp：做开发的控制台。", "~400 MB", "fish · gcc · git · mcpp", true},
    {"agent-workspace", false, "agent workspace", "agent 工作区", "A console for coding agents", "给编程 agent 的控制台",
     "Everything of core, with node, python and the coding agents, in a private network by default.",
     "core 的一切，加上 node、python 与编程 agent，默认在私有网络中。", "~700 MB", "core · node · python · agents", true},
    {"desktop", true, "Classic desktop", "经典桌面", "Windows side by side, a panel", "窗口并排、面板",
     "Everything of core, a desktop, and the tools people reach for: terminals, editors, node, python, agents.",
     "core 的一切、一个桌面，以及常用的工具：终端、编辑器、node、python、agents。", "~1.2 GB", "labwc · foot · nvim · node", true},
    {"desktop-xdesk", true, "xdesk canvas", "xdesk 画布", "An infinite canvas for windows", "放窗口的无限画布",
     "The same environment on xdesk's canvas: pan, zoom, see everything at once. To explore.",
     "同样的环境，桌面是 xdesk 的画布：平移、缩放、一眼看全。供探索。", "~1.2 GB", "xdesk · foot · nvim · node", true},
}};

int edition_index_(std::string_view name) {
    for (std::size_t i = 0; i < kEditions.size(); ++i) if (kEditions[i].name == name) return static_cast<int>(i);
    return 3;   // core
}

// What the index said of an edition (asked once, in the background).
struct Availability {
    bool known { false };       // answered
    bool listed { true };       // in the index
    bool here { true };         // built for this machine
    std::string arch;
};

// An archive about to be imported: what it is, read before anything is made.
struct ImportPreview {
    fs::path file;
    std::string error;          // it could not be read
    std::string edition;
    std::size_t packages { 0 };
    std::string preset;
    std::vector<std::string> wider;     // what it may do beyond a new environment's default
    char name[64] = {};
    bool confirmed { false };
};

// A question over the window, answered before anything else.
struct Confirm {
    std::string title, body, yes;
    bool danger { false };
    std::function<void()> on_yes;
    std::string alt;                 // a second way to answer yes, when there is one
    std::function<void()> on_alt;
};

// The nano view: commands run one at a time, their output kept.
struct RunLine { std::string text; bool command { false }; bool error { false }; };
struct RunView {
    char input[512] = {};
    std::vector<RunLine> lines;
    std::vector<std::string> history;
    int history_at { -1 };
    std::mutex m;
    std::unique_ptr<std::jthread> worker;
    std::atomic<bool> busy { false };
    bool scroll_down { true };
};

// A character onto UTF-8 text.
void append_utf8_(std::string& out, char32_t ch) {
    if (ch < 0x80) out += static_cast<char>(ch);
    else if (ch < 0x800) { out += static_cast<char>(0xC0 | (ch >> 6)); out += static_cast<char>(0x80 | (ch & 0x3F)); }
    else if (ch < 0x10000) {
        out += static_cast<char>(0xE0 | (ch >> 12)); out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (ch >> 18)); out += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F)); out += static_cast<char>(0x80 | (ch & 0x3F));
    }
}

struct App {
    fs::path home = home_dir_();
    std::unique_ptr<host::Window> window;
    service::Context ctx{.home = home, .port = service::Port(service::xlings_path())};
    AppSettings settings = read_app_settings(home);
    View view { View::Launcher };
    View before_admin { View::Launcher };
    std::vector<service::Environment> systems;     // as the home shows them: recent first, then by name
    int recent { 0 };                              // how many of `systems` are the "recent" group
    std::string list_error;
    int selected { 0 };
    bool keyboard { false };                       // the keyboard moved last: its focus is drawn
    bool follow { false };                         // scroll the list to the selection
    std::string filter;                            // typed on the home: names shown
    std::optional<std::chrono::steady_clock::time_point> countdown_until;
    std::unique_ptr<Job> job;
    std::function<void(App&)> after_job;
    std::unique_ptr<Console> console;
    std::unique_ptr<Display> display;  // a machine's desktop
    host::Window::Texture texture;
    opensal::Size desktop_size;        // what the window shows it at
    bool desktop_text { false };       // the desktop takes text (its caret is set)
    double sent_density { 1 };         // the density the desktop was told
    std::string ended_why;             // a failed session's last lines (the off screen shows them)
    int ended_code { 0 };
    bool paste_on_open { true };       // the host's clipboard, given once it shows
    std::string pasted;                // what crossed last (not sent back)
    std::optional<std::chrono::steady_clock::time_point> resize_at;
    std::string current;               // the system on screen
    bool menu { false };               // the Host key's menu
    int menu_focus { 0 };
    bool close_dialog { false };
    int close_choice { 0 };            // 0 background, 1 power off
    bool close_remember { false };
    std::optional<Confirm> confirm;
    std::string notice;
    std::function<void()> later;       // done after the frame (what replaces what the frame drew)
    // The status-area icon (design 2026-10-16 L §13.12).
    std::unique_ptr<tray::Icon> tray;
    std::string tray_error;            // why there is none here
    std::string tray_said;             // what it shows now (it is told only changes)
    std::chrono::steady_clock::time_point tray_checked;
    bool hidden { false };             // the window is closed and the app is in the tray
    std::vector<std::string> tray_injected;   // tests: chosen at the icon
    // What is read from disk is read at most once a second, never once a
    // frame (design 2026-10-16 L §13.3): which systems run, what a
    // management page shows.
    std::map<std::string, bool> running_cache;
    std::chrono::steady_clock::time_point running_at;
    std::string page_cache_for;
    std::chrono::steady_clock::time_point page_at;
    std::vector<std::string> page_desktops;
    ClipboardGrant page_grant;
    std::vector<std::pair<std::string, std::string>> activity_rows;   // when, what
    std::vector<std::string> activity_refs;
    std::chrono::steady_clock::time_point activity_at;
    bool close_temp { false };
    bool refresh_after_frame { false };
    // How the app keeps up (design 2026-10-16 L §13.3), for the tests that hold it to it.
    struct Perf {
        std::deque<double> frame_ms;                  // the last frames: input, layout, drawing, presenting
        std::deque<double> present_ms;                // of which rendering and presenting
        double input_to_next_frame_ms { 0 };          // the latest input until the frame that shows its effect
        double input_to_next_frame_max { 0 };
        int wait_after_input_ms { 0 };                // what the scheduler waits after input
        int idle_wait_ms { 0 };                       // and when nothing happens (not drawing for nothing)
        int frames { 0 };
        std::optional<std::chrono::steady_clock::time_point> input_at;
        int frames_since_input { 0 };
    } perf;
    bool job_told { true };            // the last job's end was said (a notification when unseen)
    std::string job_subject;           // the system the last job made (its notification goes in)
    std::optional<std::chrono::steady_clock::time_point> notice_until;   // a notice over a system, until
    // Quick start
    char new_name[64] = {};
    char new_proxy[256] = {};
    char new_user[64] = {};
    char new_password[128] = {};
    std::mutex available_m;            // before the job: it is joined while this still exists
    std::map<std::string, Availability> available;     // by edition, from the index
    std::optional<service::Capabilities> caps;         // what this computer can do (luban status), once asked
    std::unique_ptr<std::jthread> availability_job;
    std::optional<ImportPreview> import;
    std::string quick_said;            // the quick start's line about the chosen edition
    bool quick_can_make { true };
    std::string auto_name;             // what the name field was given, until the person types
    int new_edition { 3 };             // core
    int new_mode { 0 };                // 0 sandbox, 1 machine
    int new_mode_for { -1 };           // the edition the mode was defaulted for
    int new_persist { 0 };             // all, home, none
    float panel_h { 0 };               // the quick start's panel, as tall as it was last drawn
    bool more { false };
    // Admin
    int admin_selected { 0 };
    std::string admin_tab { "systems" };
    int admin_page { 0 };              // overview, versions, settings, export & remove
    std::optional<Json> history;       // the selected system's generations
    std::string history_for;
    std::optional<Json> upgrade_plan;
    std::string attach;                // --attach
    bool dump_state { false };
    std::string input;                 // tests: typed into the console (or desktop) once it shows something
    std::optional<std::chrono::steady_clock::time_point> input_at;
    bool quit { false };
    // The console: scrolled back, a selection being made.
    int console_scroll { 0 };
    std::optional<std::array<int, 4>> selection;
    bool selecting { false };
    ConsoleGrid grid;
    // The nano view.
    RunView run;
    // Management: self-checks asked, what they said; fields being edited.
    std::mutex check_m;
    std::set<std::string> checking;
    std::map<std::string, Json> checks;
    std::vector<std::unique_ptr<std::jthread>> check_jobs;
    char proxy_edit[256] = {};
    char export_path[1024] = {};
    // Tests: where each named control was drawn this frame ({x0, y0, x1, y1}),
    // so a script clicks "the gear", not a coordinate.
    std::map<std::string, std::array<float, 4>> anchors;
    std::map<std::string, std::array<float, 4>> anchors_before;   // the frame before
    std::set<std::string> clipped;                                // drawn, not whole (scrolled partly out)

    // The home's order (design 2026-10-16 L §13.4): the three opened most
    // recently, then every other by name.
    void refresh() {
        auto l = service::list(ctx);
        if (!l) { list_error = l.error(); systems.clear(); recent = 0; return; }
        list_error.clear();
        auto all = std::move(*l);
        std::vector<service::Environment> used;
        for (const auto& e : all)
            if (auto it = settings.last_used.find(e.name); it != settings.last_used.end() && !it->second.empty()) used.push_back(e);
        std::ranges::stable_sort(used, [&](const auto& a, const auto& b) { return settings.last_used[a.name] > settings.last_used[b.name]; });
        if (used.size() > 3) used.resize(3);
        std::vector<service::Environment> rest;
        for (auto& e : all)
            if (std::ranges::none_of(used, [&](const auto& u) { return u.name == e.name; })) rest.push_back(std::move(e));
        std::ranges::sort(rest, {}, &service::Environment::name);
        recent = static_cast<int>(used.size());
        systems = std::move(used);
        systems.insert(systems.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(systems.size()) + 1));
    }

    // What the countdown goes into: the default system, else the one opened last, else the first.
    std::string countdown_target() const {
        if (systems.empty()) return {};
        if (!settings.default_system.empty()
            && std::ranges::any_of(systems, [&](const auto& s) { return s.name == settings.default_system; }))
            return settings.default_system;
        return systems.front().name;
    }

    std::string title_for(std::string_view what) const {
        return std::format("{} — Luban OS · Ctrl+Alt {}", what,
                           window->keyboard_grabbed() ? tr("releases the keyboard", "释放键盘") : tr("menu", "菜单"));
    }

    void start_job(std::string title, std::function<void(Job&, service::Context)> body, std::function<void(App&)> then) {
        job = std::make_unique<Job>();
        job->title = std::move(title);
        job->cancel = std::make_shared<std::atomic<bool>>(false);
        job_told = false;
        job_subject.clear();
        auto* j = job.get();
        auto* w = window.get();
        service::Context c = ctx;
        c.port = c.port.cancellable(job->cancel);
        c.sink = [j, w](const service::Event& e) { j->take(e); w->wake(); };
        c.yes = true;
        after_job = then;
        job->retry = [this, title = job->title, body, then] { start_job(title, body, then); };
        job->worker = std::thread([j, w, c, body = std::move(body)] {
            body(*j, c);
            j->done = true;
            w->wake();
        });
        view = View::Job;
    }

    // The edition of a system, as a name ("core").
    static std::string edition_name(const service::Environment& e) {
        auto label = e.edition;
        if (label.starts_with("subos:luban-")) label = label.substr(12);
        if (const auto at = label.find('@'); at != std::string::npos) label = label.substr(0, at);
        return label;
    }

    void enter(const std::string& name) {
        countdown_until.reset();
        settings.last_used[name] = xlings::observe::utc_now();
        write_app_settings(home, settings);
        current = name;
        auto env = service::describe(ctx, name);
        // nano has nothing to log into: its commands (design 2026-10-16 L §13.6).
        if (env && edition_name(*env) == "nano" && !vm::running(home, name)) {
            open_run(name);
            return;
        }
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

    void open_run(const std::string& name) {
        current = name;
        {
            std::lock_guard l(run.m);
            run.lines.clear();
            run.lines.push_back({std::vformat(tr("{} has no shell: each line here is one command, run in it (luban run {} -- …).",
                                                 "{} 没有 shell：这里每一行是在其中运行的一条命令（luban run {} -- …）。"),
                                              std::make_format_args(name, name))});
        }
        view = View::Run;
        window->set_title(std::format("{} — Luban OS", name));
        window->text_input(true);
    }

    // A machine shows its desktop when its root declares one (and its
    // setting is not `none`), else its console.
    void open_machine(const std::string& name) {
        auto env = service::describe(ctx, name);
        if (auto session = env ? desktop_session(*env) : std::nullopt) {
            std::string error;
            const auto [w, h] = window->size();
            desktop_size = device_size_(w, h);
            auto* win = window.get();
            display = Display::machine(home, name, *session, desktop_size, clipboard_grant(name), [win] { win->wake(); }, error);
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

    // The screen's density, as a remote desktop is told it; tests set it (--density).
    double density_override { 0 };
    double density() const {
        if (density_override > 0) return density_override;
        const double d = window ? window->density() : 1.0;
        return d > 0 ? d : 1.0;
    }
    // Device pixels per unit of the window's coordinates.
    double pixel_density() const {
        if (density_override > 0) return density_override;
        return window ? window->pixel_density() : 1.0;
    }

    // A desktop's size for a window of w x h: in its device pixels, so that
    // what the desktop draws is as sharp as the screen.
    opensal::Size device_size_(int w, int h) const {
        const double d = pixel_density();
        return {static_cast<std::uint32_t>(std::lround(w * d)), static_cast<std::uint32_t>(std::lround(h * d))};
    }

    // What the environment lets cross the clipboard: its grants; an
    // environment without a policy file is as open as it always was, and a
    // policy that cannot be read lets nothing cross.
    ClipboardGrant clipboard_grant(const std::string& name) const {
        auto p = xlings::subos::policy_store::read(xlings::subos::HomeView{home}, name);
        if (!p) return {};
        if (!*p) return {.copy = true, .paste = true};
        return {.copy = (*p)->grants.contains("clipboard"), .paste = (*p)->grants.contains("clipboard-paste")};
    }

    void close_display() {
        desktop_text = false;
        sent_density = 1;
        paste_on_open = true;
        pasted.clear();
        display.reset();
        if (texture.handle) window->destroy(texture);
        texture = {};
    }

    // The luban a sandbox runs for its desktop, as the sandbox sees it. A
    // system in a prefix domain sees its own home (/xlings), not this one:
    // the luban its domain carries. Elsewhere, the one beside this app, else
    // the home's.
    fs::path luban_binary(const std::string& name) const {
        std::error_code ec;
        if (std::ifstream in(xlings::subos::HomeView{home}.instance_file(name)); in) {
            const auto doc = nlohmann::json::parse(in, nullptr, false);
            if (doc.is_object() && doc.value("carrier", std::string("local")) != "local") return fs::path("/xlings/bin/luban");
        }
        if (const auto at = xlings::subos::locate(xlings::subos::HomeView{home}, name); at.domain)
            return fs::is_regular_file(at.home.home / "bin" / "luban", ec) ? at.logical / "bin" / "luban" : fs::path{};
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
            const auto luban = luban_binary(name);
            if (auto session = env ? desktop_session(*env) : std::nullopt; session && !luban.empty()) {
                const auto [ww, wh] = window->size();
                desktop_size = device_size_(ww, wh);
                display = Display::sandbox(ctx.port.xlings(), luban, home, name, *session, env ? env->settings.persist : "all",
                                           desktop_size, clipboard_grant(name), [w] { w->wake(); }, error);
                if (display) {
                    view = View::Desktop;
                    window->set_title(title_for(name));
                    window->text_input(false);
                    return;
                }
                notice = error;
            }
        }
        // The welcome a console says first, the same as `luban enter`'s.
        std::string greeting;
        if (env)
            for (const auto& line : service::welcome(*env, machine ? "machine" : "sandbox", false,
                                                     {.zh = tr("en", "zh") == "zh", .color = true, .utf8 = true}))
                greeting += line + "\r\n";
        console = machine ? Console::machine(home, name, [w] { w->wake(); }, error, greeting)
                          : Console::sandbox(ctx.port.xlings(), home, name, env ? env->settings.persist : "all", [w] { w->wake(); },
                                             error, greeting);
        if (!console) {
            notice = error;
            view = View::Launcher;
            return;
        }
        console_scroll = 0;
        selection.reset();
        view = View::Console;
        window->set_title(title_for(name));
        window->text_input(true);
    }

    void power_off(const std::string& name) {
        start_job(std::format("{} {}", tr("Powering off", "关闭"), name),
                  [name](Job& j, service::Context c) { j.code = vm::stop(c, name).code; },
                  [](App& a) { a.view = View::Off; });
    }

    bool is_running(const std::string& name) {
        const auto now = std::chrono::steady_clock::now();
        if (now - running_at > std::chrono::seconds(1)) {
            running_cache.clear();
            running_at = now;
        }
        auto it = running_cache.find(name);
        if (it == running_cache.end()) it = running_cache.emplace(name, vm::running(home, name).has_value()).first;
        return it->second;
    }

    float console_size(const std::string& name) const {
        if (auto it = settings.console_sizes.find(name); it != settings.console_sizes.end()) return it->second;
        return settings.console_size;
    }

    void back_home() {
        menu = false;
        console.reset();
        close_display();
        view = View::Launcher;
        refresh();
        window->set_title("Luban OS");
    }
};

// ── views ───────────────────────────────────────────────────────────────

using imkit::px;

void begin_full_(const char* id) {
    const auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                  | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar
                                  | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
}

// The last item drawn, by name (App::anchors).
void mark_(App& a, std::string name) {
    (void)a;
    imkit::anchor(name);   // what can be seen whole (imkit decides), into a.anchors
}

const imkit::Theme& th_() { return imkit::theme(); }

// A line of text centred across the window at `y`.
void centred_(ImDrawList* dl, float y, std::string_view s, ImU32 color, float size = 0, bool title = false) {
    const float W = ImGui::GetWindowWidth();
    const auto shown = imkit::fit(s, W - px(32), size, title);
    const auto ts = imkit::text_size(shown, size, title);
    imkit::text(dl, ImVec2(ImGui::GetWindowPos().x + (W - ts.x) / 2, ImGui::GetWindowPos().y + y), shown, color, size, title);
}

// A paragraph across `width` at `at`; returns the height it took.
float paragraph_(ImDrawList* dl, ImVec2 at, std::string_view s, float width, ImU32 color, float size = 0) {
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
bool page_bar_(App& a, std::string_view title, bool back, std::string_view back_id = "back") {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = px(64);
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), th_().bg_deep);
    dl->AddRectFilled(ImVec2(o.x, o.y + H - imkit::unit() / 3), ImVec2(o.x + W, o.y + H), th_().line);
    float x = px(20);
    bool clicked = false;
    if (back) {
        ImGui::SetCursorPos(ImVec2(x, px(14)));
        clicked = imkit::button(back_id, tr("Back", "返回"), ImVec2(96, 36), imkit::Look::Ghost, true, false, imkit::Icon::Back);
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
    if (secs < 60) return std::string(tr("just now", "刚刚"));
    const auto mins = secs / 60, hours = secs / 3600;
    if (secs < 3600) return std::vformat(tr("{} min ago", "{} 分钟前"), std::make_format_args(mins));
    if (secs < 86400) return std::vformat(tr("{} h ago", "{} 小时前"), std::make_format_args(hours));
    const auto days = secs / 86400;
    return std::vformat(tr("{} d ago", "{} 天前"), std::make_format_args(days));
}

std::string edition_label_(const service::Environment& e) {
    // An environment with no edition is a view of this host (its packages, no root of its own).
    if (e.edition.empty()) return std::string(e.kind == "view" ? tr("host view", "宿主视图") : tr("root", "根环境"));
    auto label = e.edition;
    if (label.starts_with("subos:luban-")) label = label.substr(12);
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

void launcher_(App& a) {
    begin_full_("launcher");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    // The bar: the craftsman and the name; the gear and "new" at the right.
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + px(72)), th_().bg_deep);
    art::badge(dl, art::Badge::Craftsman, "", ImVec2(o.x + px(20), o.y + px(12)), std::max(1.0f, std::round(px(3))),
               imkit::beat(2, 0.5));
    imkit::text(dl, ImVec2(o.x + px(76), o.y + px(22)), "Luban OS", th_().text, 26, true);
    ImGui::SetCursorPos(ImVec2(W - px(20) - px(48), px(16)));
    if (imkit::button("gear", "", ImVec2(48, 40), imkit::Look::Ghost, true, false, imkit::Icon::Gear)) {
        a.before_admin = a.view;
        a.view = View::Admin;
    }
    mark_(a, "gear");
    ImGui::SetCursorPos(ImVec2(W - px(20) - px(48) - px(12) - px(150), px(16)));
    if (imkit::button("new", tr("New system", "新建系统"), ImVec2(150, 40), imkit::Look::Primary, true, false, imkit::Icon::Plus))
        a.view = View::QuickStart;
    mark_(a, "new");

    // The list: recent first, then the rest by name; a new one, one from a file.
    const float listW = std::min(px(820), W - px(48));
    const float x0 = std::round((W - listW) / 2);
    const float top = px(92), bottom = H - px(84);
    const auto shown = shown_(a);
    if (!a.filter.empty()) {
        imkit::text(dl, ImVec2(o.x + x0, o.y + top - px(4)),
                    std::vformat(tr("Showing names with “{}” · Esc shows all", "名字含“{}”的系统 · Esc 显示全部"),
                                 std::make_format_args(a.filter)),
                    th_().warn, 14);
    }
    ImGui::SetCursorPos(ImVec2(x0, top + px(20)));
    ImGui::BeginChild("systems", ImVec2(listW, std::max(px(120), bottom - top - px(20))), ImGuiChildFlags_None);
    const float rowH = 68, rowW = ImGui::GetContentRegionAvail().x;
    auto* ldl = ImGui::GetWindowDrawList();
    auto group = [&](std::string_view label) {
        ImGui::Dummy(ImVec2(0, px(6)));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        imkit::text(ldl, at, label, th_().text_faint, 13, true);
        ImGui::Dummy(ImVec2(0, imkit::text_size(label, 13, true).y + px(6)));
    };
    const int n = static_cast<int>(shown.size());
    for (int k = 0; k < n; ++k) {
        const int i = shown[static_cast<std::size_t>(k)];
        if (a.filter.empty() && k == 0 && a.recent > 0) group(tr("RECENT", "最近"));
        if (a.filter.empty() && i == a.recent) group(tr("ALL SYSTEMS", "全部系统"));
        const auto& e = a.systems[static_cast<std::size_t>(i)];
        const bool chosen = a.selected == k;
        if (imkit::list_row("row:" + e.name, ImVec2(rowW, rowH), chosen, chosen && a.keyboard)) {
            a.selected = k;
            a.countdown_until.reset();
            a.enter(e.name);
        }
        mark_(a, "row:" + e.name);
        if (chosen && a.follow) ImGui::SetScrollHereY(0.5f);
        const auto mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const auto edition = App::edition_name(e);
        const float cell = std::max(1.0f, std::round(px(3)));
        art::badge(ldl, art::badge_of(edition), edition, ImVec2(mn.x + px(14), mn.y + (mx.y - mn.y - 16 * cell) / 2), cell,
                   chosen ? imkit::beat(2, 0.45) : 0);
        const float tx = mn.x + px(14) + 16 * cell + px(16);
        imkit::text(ldl, ImVec2(tx, mn.y + px(12)), imkit::fit(e.name, px(260), 18, true), th_().text, 18, true);
        const bool running = e.running || a.is_running(e.name);
        std::string second = edition_label_(e);
        if (edition == "nano") second += std::string(" · ") + std::string(tr("no shell · runs commands", "无 shell · 运行命令"));
        imkit::text(ldl, ImVec2(tx, mn.y + px(40)), imkit::fit(second, mx.x - tx - px(260), 14), th_().text_dim, 14);
        // The right side: its mode, whether it runs, when it was last opened.
        float rx = mx.x - px(16);
        if (auto it = a.settings.last_used.find(e.name); it != a.settings.last_used.end()) {
            const auto when = ago_(it->second);
            const auto ts = imkit::text_size(when, 13);
            rx -= ts.x;
            imkit::text(ldl, ImVec2(rx, mn.y + px(40)), when, th_().text_faint, 13);
        }
        const std::string mode = e.settings.mode == "machine" ? std::string(tr("machine", "机器")) : std::string(tr("sandbox", "沙箱"));
        const std::string state = running ? std::string(tr("running", "运行中")) : std::string(tr("stopped", "已停止"));
        const auto sw = imkit::text_size(state, 13).x + px(12), mw = imkit::text_size(mode, 13).x + px(12);
        chip_(ldl, ImVec2(mx.x - px(16) - sw, mn.y + px(12)), state, running ? th_().ink : th_().text_dim,
              running ? th_().good : th_().panel_lo);
        chip_(ldl, ImVec2(mx.x - px(16) - sw - px(8) - mw, mn.y + px(12)), mode, th_().text, art::edition_color(edition, -1));
    }
    if (n == 0 && a.systems.empty()) {
        // Nothing yet: the craftsman, and the first step.
        ImGui::Dummy(ImVec2(0, px(40)));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float cell = std::round(px(5));
        art::badge(ldl, art::Badge::Craftsman, "", ImVec2(at.x + (rowW - 16 * cell) / 2, at.y), cell, imkit::beat(2, 0.5));
        ImGui::Dummy(ImVec2(0, 16 * cell + px(16)));
    }
    if (n == 0 && !a.systems.empty()) group(tr("NOTHING MATCHES", "没有匹配的系统"));
    else ImGui::Dummy(ImVec2(0, px(10)));
    {
        const bool chosen = a.selected == n;
        if (imkit::list_row("row:new", ImVec2(rowW, 52), chosen, chosen && a.keyboard)) a.view = View::QuickStart;
        mark_(a, "row:new");
        if (chosen && a.follow) ImGui::SetScrollHereY(0.5f);
        const auto mn = ImGui::GetItemRectMin();
        const float cell = std::max(1.0f, std::round(px(2)));
        imkit::icon(ldl, imkit::Icon::Plus, ImVec2(mn.x + px(20), mn.y + px(26) - 6 * cell), cell, th_().accent);
        imkit::text(ldl, ImVec2(mn.x + px(52), mn.y + px(16)), tr("New system", "新建系统"), th_().text, 16);
    }
    {
        if (imkit::list_row("row:import", ImVec2(rowW, 52), false, false))
            a.window->pick_file(std::string(tr("Import a Luban system", "导入 Luban 系统")), "Luban", "luban");
        mark_(a, "row:import");
        const auto mn = ImGui::GetItemRectMin();
        const float cell = std::max(1.0f, std::round(px(2)));
        imkit::icon(ldl, imkit::Icon::Import, ImVec2(mn.x + px(20), mn.y + px(26) - 6 * cell), cell, th_().text_dim);
        imkit::text(ldl, ImVec2(mn.x + px(52), mn.y + px(16)), tr("Import from a file (.luban)…", "从文件导入（.luban）…"),
                    th_().text_dim, 16);
    }
    a.follow = false;
    ImGui::EndChild();
    // What is going on: the countdown, a notice, why the list is empty.
    float y = bottom + px(4);
    if (a.countdown_until && !a.systems.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(*a.countdown_until - std::chrono::steady_clock::now()).count() + 1;
        const auto target = a.countdown_target();
        centred_(dl, y, std::vformat(tr("{} s, then {} · any key stops it", "{} 秒后进入 {} · 按任意键取消"),
                                     std::make_format_args(left, target)), th_().accent_hi, 15);
        y += px(22);
    }
    if (!a.notice.empty()) { centred_(dl, y, a.notice, th_().warn, 14); y += px(20); }
    if (!a.list_error.empty()) centred_(dl, y, a.list_error, th_().bad, 14);
    centred_(dl, H - px(26), tr("↑ ↓ choose · Enter go in · type to find · Del remove · F2 manage",
                                "↑ ↓ 选择 · Enter 进入 · 直接输入查找 · Del 删除 · F2 管理"), th_().text_faint, 13);
    ImGui::End();
}

// Asks the index, once, what of each edition can be had here.
void ask_availability_(App& a) {
    if (a.availability_job) return;
    auto* app = &a;
    a.availability_job = std::make_unique<std::jthread>([app, ctx = a.ctx](std::stop_token stop) {
        // What this computer can do: the same report `luban status` gives.
        {
            auto caps = service::capabilities(service::gather(ctx.home, ctx.port));
            std::lock_guard l(app->available_m);
            app->caps = std::move(caps);
        }
        if (!stop.stop_requested() && app->window) app->window->wake();
        const std::string arch(platform::build_arch());
        for (const auto& e : kEditions) {
            if (stop.stop_requested()) return;   // the window is closing
            Availability v{.known = true, .arch = arch};
            if (auto got = service::edition_availability(ctx, e.name)) {
                v.listed = got->listed;
                v.here = !got->listed || got->archs.empty() || std::ranges::find(got->archs, arch) != got->archs.end();
            }   // the index could not be asked: offered, and making it says why
            std::lock_guard l(app->available_m);
            app->available[std::string(e.name)] = v;
        }
        if (!stop.stop_requested() && app->window) app->window->wake();
    });
}

// Whether this computer can run a Luban system (a sandbox or a machine is
// ready), as the capability report said; until it has, what this platform
// could always do.
bool can_run_(App& a) {
    std::lock_guard l(a.available_m);
    if (!a.caps) return platform::is_linux;
    const auto* s = a.caps->mode("sandbox");
    const auto* m = a.caps->mode("machine");
    return (s && s->ready) || (m && m->ready);
}

// Why this computer cannot run a Luban system, with the next step; nothing
// when it can, or while that is not known yet.
std::string cannot_run_(App& a) {
    if (can_run_(a)) return {};
    std::lock_guard l(a.available_m);
    std::string out(tr("This computer cannot run Luban systems yet: ", "这台电脑还不能运行 Luban 系统："));
    const auto* s = a.caps ? a.caps->mode("sandbox") : nullptr;
    if (!s) return out + std::string(tr("they are Linux systems and need a way to run one here.", "它们是 Linux 系统，这里还没有运行它们的方式。"));
    out += s->reason;
    if (!s->route.empty()) out += std::string(tr(" -- do it: ", "——处理：")) + s->route;
    return out;
}

// Why this edition cannot be made here, or nothing.
std::string unavailable_(App& a, const Edition& e) {
    if (auto why = cannot_run_(a); !why.empty()) return why;
    std::lock_guard l(a.available_m);
    const auto it = a.available.find(std::string(e.name));
    if (it == a.available.end() || !it->second.known) return {};
    if (!it->second.listed) return std::string(tr("Not published yet.", "尚未发布。"));
    if (!it->second.here)
        return std::vformat(tr("Not built for {} yet.", "尚未提供 {} 版本。"), std::make_format_args(it->second.arch));
    return {};
}

// Makes the system the quick start describes, then goes in.
void make_(App& a) {
    const auto& chosen = kEditions[static_cast<std::size_t>(a.new_edition)];
    service::NewOptions o{.name = a.new_name, .edition = std::string(chosen.name), .proxy = a.new_proxy, .user = a.new_user};
    if (a.new_password[0]) o.password_hash = xlings::sha512::crypt(a.new_password, xlings::sha512::random_salt());
    const bool machine = a.new_mode == 1, goes_in = chosen.goes_in;
    static constexpr std::array<std::string_view, 3> kPersist{"all", "home", "none"};
    const std::string persist(kPersist[static_cast<std::size_t>(std::clamp(a.new_persist, 0, 2))]);
    const std::string name = o.name;
    a.start_job(std::format("{} {}", tr("Making", "创建"), name),
                [o, machine, persist](Job& j, service::Context c) {
                    auto out = service::create(c, o);
                    j.code = out.code;
                    std::vector<std::pair<std::string, std::string>> changes;
                    if (machine) changes.emplace_back("mode", "machine");
                    if (persist != "all") changes.emplace_back("persist", persist);
                    if (out.ok() && !changes.empty()) (void)service::configure(c, o.name, changes);
                },
                [name, goes_in](App& app) {
                    if (app.job->code != 0) return;
                    app.refresh();
                    // Made is entered (design 2026-10-16 L §13.5): a console or
                    // a desktop; nano, its commands.
                    if (goes_in) { app.enter(name); return; }
                    app.settings.last_used[name] = xlings::observe::utc_now();
                    write_app_settings(app.home, app.settings);
                    app.open_run(name);
                });
    a.job_subject = name;
}

// The chosen edition's panel: what it is, its settings, making it. At
// (x, y) in the current window, `w` wide; returns its height.
float new_panel_(App& a, float x, float y, float w, float min_h) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o(ImGui::GetWindowPos().x - ImGui::GetScrollX(), ImGui::GetWindowPos().y - ImGui::GetScrollY());
    const auto& chosen = kEditions[static_cast<std::size_t>(a.new_edition)];
    const float pad = px(18), fw = w - pad * 2;
    float cy = y + px(16);
    const float start = y;
    // The face first (drawn under everything), sized once its content is known: last frame's height.
    const float h = std::max(min_h, a.panel_h);
    imkit::panel(dl, ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + w, o.y + y + h), imkit::Bevel::Raised, th_().panel_lo);
    const float cx = x + pad;
    art::badge(dl, art::badge_of(chosen.name), chosen.name, ImVec2(o.x + cx, o.y + cy), std::round(px(3)), imkit::beat(2, 0.45));
    imkit::text(dl, ImVec2(o.x + cx + px(60), o.y + cy + px(2)), imkit::fit(tr(chosen.title_en, chosen.title_zh), fw - px(60), 22, true),
                th_().text, 22, true);
    imkit::text(dl, ImVec2(o.x + cx + px(60), o.y + cy + px(30)), imkit::fit(std::format("{} · {}", chosen.size, chosen.contents), fw - px(60), 13),
                th_().text_faint, 13);
    cy += px(62);
    const auto why_not = unavailable_(a, chosen);
    a.quick_said = why_not.empty() ? std::string(tr(chosen.detail_en, chosen.detail_zh)) : why_not;
    a.quick_can_make = why_not.empty();
    cy += paragraph_(dl, ImVec2(o.x + cx, o.y + cy), a.quick_said, fw, why_not.empty() ? th_().text_dim : th_().warn, 14) + px(12);
    // The name follows the edition until the person types one of their own.
    if (a.new_name[0] == 0 || a.auto_name == a.new_name) {
        const auto base = std::string(chosen.name);
        for (int k = 1; k < 100; ++k) {
            const auto candidate = std::format("{}-{}", base, k);
            if (std::ranges::none_of(a.systems, [&](const auto& s2) { return s2.name == candidate; })) {
                std::snprintf(a.new_name, sizeof a.new_name, "%s", candidate.c_str());
                a.auto_name = candidate;
                break;
            }
        }
    }
    auto label = [&](std::string_view s2) {
        imkit::text(dl, ImVec2(o.x + cx, o.y + cy), s2, th_().text_dim, 13, true);
        cy += px(20);
    };
    const float fwn = fw / imkit::scale();
    label(tr("NAME", "名字"));
    ImGui::SetCursorPos(ImVec2(cx, cy));
    imkit::text_field("name", a.new_name, sizeof a.new_name, fwn);
    mark_(a, "name");
    cy += px(48);
    label(tr("RUNS AS", "运行方式"));
    ImGui::SetCursorPos(ImVec2(cx, cy));
    a.new_mode = imkit::segmented("mode", {std::string(tr("sandbox", "沙箱")), std::string(tr("machine", "机器"))}, a.new_mode, fwn,
                                  {"mode:sandbox", "mode:machine"});
    cy += px(46);
    ImGui::SetCursorPos(ImVec2(cx, cy));
    imkit::toggle("more", tr("More options", "更多选项"), a.more);
    mark_(a, "more");
    cy += px(36);
    if (a.more) {
        label(tr("KEEPS", "保留修改"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        a.new_persist = imkit::segmented("persist", {std::string(tr("all", "全部")), std::string(tr("/home", "仅 /home")),
                                                     std::string(tr("nothing", "不保留"))},
                                         a.new_persist, fwn);
        cy += px(46);
        label(tr("PROXY (SOCKS5h, optional)", "代理（SOCKS5h，可选）"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        imkit::text_field("proxy", a.new_proxy, sizeof a.new_proxy, fwn, 0, "socks5h://127.0.0.1:1080");
        mark_(a, "proxy");
        cy += px(48);
        label(tr("LOGS IN AS (optional)", "登录用户（可选）"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        imkit::text_field("user", a.new_user, sizeof a.new_user, (fw / 2 - px(6)) / imkit::scale(), 0, "root");
        mark_(a, "user");
        ImGui::SetCursorPos(ImVec2(cx + fw / 2 + px(6), cy));
        imkit::text_field("password", a.new_password, sizeof a.new_password, (fw / 2 - px(6)) / imkit::scale(),
                          ImGuiInputTextFlags_Password, tr("password", "密码"));
        mark_(a, "password");
        cy += px(48);
    }
    ImGui::SetCursorPos(ImVec2(cx, cy + px(4)));
    const auto make_label = chosen.goes_in ? tr("Create and go in", "创建并进入") : tr("Create", "创建");
    if (imkit::button("make", make_label, ImVec2(fwn, 44), imkit::Look::Primary, why_not.empty() && a.new_name[0], false, imkit::Icon::Run))
        make_(a);
    mark_(a, "make");
    cy += px(58);
    ImGui::SetCursorPos(ImVec2(cx, cy));
    if (imkit::button("import", tr("Import from a file (.luban)…", "从文件导入（.luban）…"), ImVec2(fwn, 36),
                      imkit::Look::Ghost, true, false, imkit::Icon::Import))
        a.window->pick_file(std::string(tr("Import a Luban system", "导入 Luban 系统")), "Luban", "luban");
    mark_(a, "import");
    cy += px(44);
    if (!a.notice.empty()) cy += paragraph_(dl, ImVec2(o.x + cx, o.y + cy), a.notice, fw, th_().warn, 13) + px(6);
    a.panel_h = cy - start + px(8);
    return std::max(min_h, a.panel_h);
}

void quick_start_(App& a) {
    begin_full_("quick");
    ask_availability_(a);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    if (page_bar_(a, tr("New system", "新建系统"), !a.systems.empty())) a.view = View::Launcher;
    // A mode chosen for the edition, until the person chooses: a desktop runs
    // as a machine (its own kernel, its own display), a console in a sandbox.
    if (a.new_mode_for != a.new_edition) {
        a.new_mode = kEditions[static_cast<std::size_t>(a.new_edition)].desktop ? 1 : 0;
        a.new_mode_for = a.new_edition;
    }
    const float margin = px(24), gap = px(12);
    float top = px(80);
    if (const auto why = cannot_run_(a); !why.empty()) {
        // Said first, not discovered by a failure.
        imkit::text(dl, ImVec2(o.x + margin, o.y + top), imkit::fit(why, W - margin * 2, 14), th_().warn, 14);
        top += px(24);
    }
    // Wide: the cards, and the panel beside them. Narrow: one column that
    // scrolls, the panel under the cards.
    const bool side = W >= px(860);
    const float panelW = side ? std::clamp(W * 0.32f, px(300), px(380)) : W - margin * 2;
    const float cardsW = side ? W - margin * 3 - panelW : W - margin * 2;
    const float minCard = px(168);
    const float inner = cardsW - ImGui::GetStyle().ScrollbarSize - px(4);   // a scrollbar may take the right edge
    const int perRow = std::max(1, static_cast<int>((inner + gap) / (minCard + gap)));
    const float cardW = std::floor(std::min(px(230), (inner - gap * (perRow - 1)) / perRow));
    const float cardH = px(156);
    ImGui::SetCursorPos(ImVec2(margin, top));
    ImGui::BeginChild("cards", ImVec2(side ? cardsW + px(8) : W - margin * 2 + px(8), H - top - px(12)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    auto* cdl = ImGui::GetWindowDrawList();
    const ImVec2 co(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y - ImGui::GetScrollY());
    float y = px(6);
    for (int group = 0; group < 2; ++group) {
        imkit::text(cdl, ImVec2(co.x, co.y + y),
                    group == 0 ? tr("CONSOLE · small, yours to shape", "控制台 · 精简，自己定制") : tr("DESKTOP · ready to use", "桌面 · 开箱即用"),
                    th_().text_faint, 13, true);
        y += px(26);
        int col = 0;
        for (std::size_t i = 0; i < kEditions.size(); ++i) {
            const auto& e = kEditions[i];
            if (e.desktop != (group == 1)) continue;
            if (col == perRow) { col = 0; y += cardH + gap; }
            ImGui::SetCursorPos(ImVec2(col * (cardW + gap), y));
            const bool on = a.new_edition == static_cast<int>(i);
            // One that cannot be made here is still chosen to read why.
            const bool usable = unavailable_(a, e).empty();
            const auto desc = usable ? std::format("{}\n{} · {}", tr(e.about_en, e.about_zh), e.size, e.contents)
                                     : std::format("{}\n{}", tr(e.about_en, e.about_zh), tr("Not available here", "这里不可用"));
            if (imkit::option_card("card:" + std::string(e.name), art::picture(e.name, on ? imkit::beat(2, 0.45) : 0),
                                   tr(e.title_en, e.title_zh), desc, on, ImVec2(cardW / imkit::scale(), cardH / imkit::scale()),
                                   on && a.keyboard, true))
                a.new_edition = static_cast<int>(i);
            mark_(a, "card:" + std::string(e.name));
            ++col;
        }
        y += cardH + px(22);
    }
    if (!side) {
        new_panel_(a, 0, y, panelW, 0);
        ImGui::SetCursorPos(ImVec2(0, y + a.panel_h + px(16)));
        ImGui::Dummy(ImVec2(1, 1));
    }
    ImGui::EndChild();
    if (side) new_panel_(a, margin * 2 + cardsW, top + px(6), panelW, H - top - px(28));
    ImGui::End();
}

void import_view_(App& a) {
    begin_full_("import");
    auto& p = *a.import;
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth();
    const auto file_name = p.file.filename().string();
    if (page_bar_(a, std::vformat(tr("Import {}", "导入 {}"), std::make_format_args(file_name)), false)) {}
    const float width = std::min(px(620), W - px(48)), x = std::round((W - width) / 2);
    float y = px(96);
    imkit::panel(dl, ImVec2(o.x + x - px(16), o.y + y - px(12)), ImVec2(o.x + x + width + px(16), o.y + ImGui::GetWindowHeight() - px(24)),
                 imkit::Bevel::Raised, th_().panel_lo);
    if (!p.error.empty()) {
        y += paragraph_(dl, ImVec2(o.x + x, o.y + y), std::vformat(tr("It cannot be read: {}", "无法读取：{}"), std::make_format_args(p.error)),
                        width, th_().bad) + px(16);
    } else {
        auto row = [&](std::string_view k, const std::string& v) {
            imkit::text(dl, ImVec2(o.x + x, o.y + y), k, th_().text_faint, 13, true);
            imkit::text(dl, ImVec2(o.x + x + px(140), o.y + y), imkit::fit(v, width - px(140)), th_().text);
            y += px(28);
        };
        row(tr("EDITION", "EDITION"), p.edition.empty() ? std::string("—") : p.edition);
        row(tr("PACKAGES", "软件包"), std::to_string(p.packages));
        row(tr("ISOLATION", "隔离"), p.preset.empty() ? std::string(tr("the default", "默认")) : p.preset);
        if (!p.wider.empty()) {
            // More than a new environment may do: said before, agreed to.
            y += px(8);
            imkit::icon(dl, imkit::Icon::Warn, ImVec2(o.x + x, o.y + y), std::round(px(1.5f)), th_().warn);
            imkit::text(dl, ImVec2(o.x + x + px(26), o.y + y), tr("It may do more than a new system:", "它能做的比新系统更多："), th_().warn);
            y += px(26);
            for (const auto& w : p.wider) {
                imkit::text(dl, ImVec2(o.x + x + px(16), o.y + y), imkit::fit("· " + w, width - px(16), 14), th_().text_dim, 14);
                y += px(22);
            }
            ImGui::SetCursorPos(ImVec2(x, y + px(4)));
            imkit::checkbox("import:confirm", tr("I know; import it as it is", "我知道了，按原样导入"), p.confirmed);
            mark_(a, "import:confirm");
            y += px(40);
        }
        y += px(8);
        imkit::text(dl, ImVec2(o.x + x, o.y + y), tr("NAME", "名字"), th_().text_dim, 13, true);
        y += px(20);
        ImGui::SetCursorPos(ImVec2(x, y));
        imkit::text_field("import:name", p.name, sizeof p.name, width / imkit::scale());
        mark_(a, "import:name");
        y += px(56);
        if (const auto why = cannot_run_(a); !why.empty()) y += paragraph_(dl, ImVec2(o.x + x, o.y + y), why, width, th_().warn, 14) + px(8);
        ImGui::SetCursorPos(ImVec2(x, y));
        const bool go = imkit::button("import:go", tr("Import and go in", "导入并进入"), ImVec2(width / imkit::scale(), 44),
                                      imkit::Look::Primary, (p.wider.empty() || p.confirmed) && can_run_(a));
        mark_(a, "import:go");
        y += px(56);
        if (go) {
            const std::string name = p.name;
            const fs::path file = p.file;
            a.import.reset();
            a.start_job(std::format("{} {}", tr("Importing", "导入"), name),
                        [name, file](Job& j, service::Context c) { j.code = service::import_environment(c, file, name).code; },
                        [name](App& app) {
                            if (app.job->code != 0) return;
                            app.refresh();
                            app.enter(name);
                        });
            ImGui::End();
            return;
        }
    }
    ImGui::SetCursorPos(ImVec2(x, y));
    if (imkit::button("import:cancel", tr("Cancel", "取消"), ImVec2(width / imkit::scale(), 36), imkit::Look::Ghost)) {
        a.import.reset();
        a.view = a.systems.empty() ? View::QuickStart : View::Launcher;
    }
    mark_(a, "import:cancel");
    ImGui::End();
}

// The stages of making a system, as a person reads them, and which one the
// events are in.
constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kStages{{
    {"Resolve", "解析"}, {"Download", "下载"}, {"Unpack", "解压"}, {"Configure", "配置"}, {"Check", "自检"}}};
int stage_of_(const Job& j) {
    const auto& s = j.stage;
    if (s == "check") return 4;
    if (s.find("config") != std::string::npos || s == "hook" || s == "configure") return 3;
    if (s.find("extract") != std::string::npos || s == "unpack") return 2;
    if (s.find("download") != std::string::npos) return 1;
    return 0;
}

void job_(App& a) {
    begin_full_("job");
    auto& j = *a.job;
    std::lock_guard l(j.m);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    const float width = std::min(px(680), W - px(48)), x = std::round((W - width) / 2);
    float y = std::max(px(40), H * 0.12f);
    const bool failed = j.done && j.code != 0;
    // The craftsman at work, or standing when it failed.
    const float cell = std::round(px(5));
    art::badge(dl, art::Badge::Craftsman, "", ImVec2(o.x + (W - 16 * cell) / 2, o.y + y), cell, failed ? 0 : imkit::beat(2, 0.3));
    y += 16 * cell + px(16);
    centred_(dl, y, j.title, th_().text, 26, true);
    y += px(44);
    // Its stages, the one it is in lit.
    const int at = j.done && j.code == 0 ? 5 : stage_of_(j);
    const float chipW = std::floor((width - px(8) * 4) / 5);
    for (int k = 0; k < 5; ++k) {
        const ImVec2 c0(o.x + x + k * (chipW + px(8)), o.y + y), c1(c0.x + chipW, c0.y + px(30));
        const bool done = k < at, now = k == at && !failed;
        imkit::panel(dl, c0, c1, now ? imkit::Bevel::Raised : imkit::Bevel::Flat,
                     done ? imkit::theme().accent_lo : now ? imkit::theme().panel : imkit::theme().bg_deep);
        const std::string label(tr(kStages[static_cast<std::size_t>(k)].first, kStages[static_cast<std::size_t>(k)].second));
        const auto ts = imkit::text_size(label, 13);
        imkit::text(dl, ImVec2(c0.x + (chipW - ts.x) / 2, c0.y + (px(30) - ts.y) / 2), label,
                    done || now ? th_().text : th_().text_faint, 13);
        if (done) imkit::icon(dl, imkit::Icon::Check, ImVec2(c0.x + px(6), c0.y + px(8)), 1.0f * std::max(1.0f, std::round(px(1))), th_().good);
    }
    y += px(46);
    imkit::progress(dl, ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + width, o.y + y + px(22)), failed ? 0 : j.done ? 1.0 : j.percent);
    y += px(30);
    if (j.percent >= 0 && !j.done) {
        const auto pct = std::format("{:.0f} %", j.percent * 100);
        imkit::text(dl, ImVec2(o.x + x + width - imkit::text_size(pct, 13).x, o.y + y), pct, th_().text_dim, 13);
    }
    imkit::text(dl, ImVec2(o.x + x, o.y + y), imkit::fit(j.current, width - px(70), 14), th_().text_dim, 14);
    y += px(30);
    for (const auto& e : j.errors) y += paragraph_(dl, ImVec2(o.x + x, o.y + y), e, width, th_().bad, 14) + px(6);
    y += px(8);
    ImGui::SetCursorPos(ImVec2(x, y));
    if (!j.done) {
        if (imkit::button("job:cancel", tr("Cancel", "取消"), ImVec2(140, 38), imkit::Look::Normal, j.cancel && !j.cancel->load(),
                          false, imkit::Icon::Cross))
            if (j.cancel) j.cancel->store(true);
        mark_(a, "job:cancel");
    } else if (failed) {
        bool retry = imkit::button("job:retry", tr("Try again", "重试"), ImVec2(140, 38), imkit::Look::Primary);
        mark_(a, "job:retry");
        ImGui::SetCursorPos(ImVec2(x + px(152), y));
        bool back = imkit::button("job:back", tr("Back to the list", "返回列表"), ImVec2(180, 38), imkit::Look::Ghost);
        mark_(a, "job:back");
        if (back) a.view = View::Launcher;
        if (retry && j.retry) a.later = j.retry;   // after the frame: it replaces this job
    }
    // What it said, quietly, at the bottom.
    y += px(52);
    const float logH = std::max(0.0f, H - y - px(24));
    if (logH > px(60)) {
        ImGui::SetCursorPos(ImVec2(x, y));
        imkit::panel(dl, ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + width, o.y + y + logH), imkit::Bevel::Sunken, th_().bg_deep);
        ImGui::SetCursorPos(ImVec2(x + px(8), y + px(6)));
        ImGui::BeginChild("log", ImVec2(width - px(16), logH - px(12)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        auto* ldl = ImGui::GetWindowDrawList();
        const float lh = imkit::text_size("Ag", 13).y + px(2);
        const std::size_t first = j.log.size() > 400 ? j.log.size() - 400 : 0;
        for (std::size_t k = first; k < j.log.size(); ++k) {
            imkit::text(ldl, ImGui::GetCursorScreenPos(), imkit::fit(j.log[k], width - px(24), 13), th_().text_faint, 13);
            ImGui::Dummy(ImVec2(0, lh));
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(4)) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

void off_(App& a) {
    begin_full_("off");
    auto* dl = ImGui::GetWindowDrawList();
    const float H = ImGui::GetWindowHeight();
    float y = H * 0.3f;
    centred_(dl, y, std::vformat(tr("{} is off", "{} 已关机"), std::make_format_args(a.current)), th_().text, 26, true);
    y += px(40);
    centred_(dl, y, tr("Enter starts it again · Esc goes back to the list", "Enter 重新启动 · Esc 返回列表"), th_().text_dim, 15);
    if (!a.ended_why.empty()) {
        // Why: what the session said last, as it said it.
        y += px(40);
        centred_(dl, y, std::vformat(tr("It stopped (exit {}). It said:", "它停止了（退出码 {}）。最后的输出："), std::make_format_args(a.ended_code)),
                 th_().warn, 14);
        y += px(26);
        const float width = std::min(px(720), ImGui::GetWindowWidth() - px(80));
        const float x = (ImGui::GetWindowWidth() - width) / 2;
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::BeginChild("why", ImVec2(width, px(130)), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(a.ended_why.c_str());
        ImGui::EndChild();
        mark_(a, "why");
    }
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Enter") a.enter(a.current);
        else if (k.name == "Escape") a.back_home();
    }
}

// A layer over the window: dim, and a panel in the middle; the panel's top-left.
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
    imkit::text(dl, ImVec2(at.x + px(24), at.y + px(20)), std::vformat(tr("Close {}", "关闭 {}"), std::make_format_args(a.current)),
                th_().text, 22, true);
    const float cardW = (size.x - px(48) - px(16)) / 2;
    const bool temp = a.close_temp;
    struct Card { imkit::Icon icon; std::string_view te, tz, de, dz; };
    const std::array<Card, 2> cards{{
        {imkit::Icon::Moon, "Keep it running", "后台运行", "The system goes on; Luban OS stays in the tray.", "系统继续运行；Luban OS 留在状态栏。"},
        {imkit::Icon::Power, "Power off", "关机", temp ? "Stop the system. What it wrote goes." : "Stop the system.",
         temp ? "停止系统。修改将丢弃。" : "停止系统。"},
    }};
    for (int k = 0; k < 2; ++k) {
        const auto& c = cards[static_cast<std::size_t>(k)];
        ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(24) + k * (cardW + px(16)), at.y - wo.y + px(64)));
        const imkit::Picture pic{imkit::kIconSize, [icon = c.icon, k, &a](ImDrawList* d, ImVec2 p, float cl) {
            imkit::icon(d, icon, p, cl * 1.5f, a.close_choice == k ? th_().accent_hi : th_().text_dim);
        }};
        if (imkit::option_card(k == 0 ? "close:background" : "close:poweroff", pic, tr(c.te, c.tz), tr(c.de, c.dz), a.close_choice == k,
                               ImVec2(cardW / imkit::scale(), 170), a.keyboard && a.close_choice == k))
            a.close_choice = k;
        mark_(a, k == 0 ? "close:background" : "close:poweroff");
    }
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(24), at.y - wo.y + px(252)));
    imkit::checkbox("close:remember", tr("Remember my choice", "记住我的选择"), a.close_remember);
    mark_(a, "close:remember");
    bool decided = false;
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + size.x - px(24) - px(120) - px(12) - px(120), at.y - wo.y + size.y - px(64)));
    if (imkit::button("close:cancel", tr("Cancel", "取消"), ImVec2(120, 40), imkit::Look::Ghost)) {
        a.close_dialog = false;
        a.window->clear_close_request();
    }
    mark_(a, "close:cancel");
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + size.x - px(24) - px(120), at.y - wo.y + size.y - px(64)));
    if (imkit::button("close:confirm", tr("OK", "确定"), ImVec2(120, 40), imkit::Look::Primary)) decided = true;
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
    no = imkit::button("confirm:no", tr("Cancel", "取消"), ImVec2(120, 40), imkit::Look::Ghost);
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
    struct Item { std::string_view en, zh; std::string id; };
    std::vector<Item> items{{"Back to the list", "回到列表", "menu:back-to-the-list"}, {"Manage", "管理", "menu:manage"},
                            {a.window->fullscreen() ? "Leave full screen" : "Full screen", a.window->fullscreen() ? "退出全屏" : "全屏",
                             a.window->fullscreen() ? "menu:leave-full-screen" : "menu:full-screen"},
                            {"New window", "新窗口", "menu:new-window"}};
    const bool machine = (a.console && a.console->is_machine()) || (a.display && a.display->is_machine());
    if (machine) items.push_back({"Power off", "关机", "menu:power-off"});
    items.push_back({"Close this menu", "关闭菜单", "menu:close-this-menu"});
    const ImVec2 size(px(340), px(64) + static_cast<float>(items.size()) * px(48));
    const ImVec2 at = overlay_(dl, size);
    const ImVec2 wo = ImGui::GetWindowPos();
    imkit::text(dl, ImVec2(at.x + px(20), at.y + px(18)), imkit::fit(a.current, size.x - px(40), 20, true), th_().text, 20, true);
    std::string picked;
    a.menu_focus = std::clamp(a.menu_focus, 0, static_cast<int>(items.size()) - 1);
    for (std::size_t k = 0; k < items.size(); ++k) {
        ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(16), at.y - wo.y + px(56) + k * px(48)));
        if (imkit::button(items[k].id, tr(items[k].en, items[k].zh), ImVec2((size.x - px(32)) / imkit::scale(), 40),
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
    else if (picked == "menu:new-window") {
        const auto self = platform::get_executable_path();
        (void)platform::spawn_detached({self.string(), "--attach", a.current}, a.home / "logs" / "luban-os.log");
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
    const std::string hint(a.window->keyboard_grabbed() ? tr("Ctrl+Alt releases the keyboard", "Ctrl+Alt 释放键盘")
                                                          : tr("Ctrl+Alt: menu · Ctrl+= / Ctrl+- size", "Ctrl+Alt：菜单 · Ctrl+= / Ctrl+- 字号"));
    const auto ts = imkit::text_size(hint, 13);
    imkit::text(dl, ImVec2(o.x + W - ts.x - px(12), o.y + px(7)), hint, th_().text_faint, 13);
    return H;
}

void console_view_(App& a) {
    begin_full_("console");
    if (!a.console) { ImGui::End(); return; }
    const float bar = system_bar_(a, a.console->is_machine() ? tr("machine", "机器") : tr("sandbox", "沙箱"));
    // Keys the program gets: text from the input method; named keys and
    // control chords as xterm sends them. The size keys are the window's.
    if (!a.menu && !a.close_dialog) {
        bool app_cursor, bracketed;
        int mouse = 0;
        {
            std::lock_guard l(a.console->lock());
            app_cursor = a.console->screen().application_cursor();
            bracketed = a.console->screen().bracketed_paste();
            mouse = a.console->screen().mouse_mode();
        }
        for (const auto& e : a.window->events())
            if (auto* t = std::get_if<opensal::TextCommit>(&e)) { a.console->write(t->text); a.console_scroll = 0; a.selection.reset(); }
        for (const auto& k : a.window->keys()) {
            const auto mods = k.key.mods;
            const bool ctrl = mods & opensal::Ctrl, shift = mods & opensal::Shift;
            if (ctrl && !shift && (k.name == "=" || k.name == "+" || k.name == "-" || k.name == "0")) {
                float size = a.console_size(a.current);
                size = k.name == "0" ? a.settings.console_size : std::clamp(size + (k.name == "-" ? -1.0f : 1.0f), 8.0f, 40.0f);
                a.settings.console_sizes[a.current] = size;
                write_app_settings(a.home, a.settings);
                continue;
            }
            if (ctrl && shift && (k.name == "V" || k.name == "v")) {
                a.console->write(term::encode_paste(a.window->clipboard(), bracketed));
                continue;
            }
            if (ctrl && shift && (k.name == "C" || k.name == "c")) {
                if (a.selection) a.window->set_clipboard(console_text(*a.console, a.console_scroll, *a.selection));
                continue;
            }
            if (shift && (k.name == "PageUp" || k.name == "PageDown")) {
                a.console_scroll = std::max(0, a.console_scroll + (k.name == "PageUp" ? 1 : -1) * std::max(1, a.grid.rows - 1));
                continue;
            }
            const bool printable = k.name.size() == 1;
            if (printable && !(mods & (opensal::Ctrl | opensal::Alt))) continue;   // came as text
            a.console->write(term::encode_key(k.name, mods & (opensal::Shift | opensal::Ctrl | opensal::Alt), app_cursor));
            a.console_scroll = 0;
        }
        // The wheel scrolls back (when the program does not take the mouse);
        // a drag selects, and what is selected is copied when it ends.
        if (ImGui::IsWindowHovered() && mouse == 0) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0) a.console_scroll = std::max(0, a.console_scroll + static_cast<int>(wheel * 3));
            const auto m = ImGui::GetMousePos();
            const int col = static_cast<int>((m.x - a.grid.x) / a.grid.cell_w), row = static_cast<int>((m.y - a.grid.y) / a.grid.cell_h);
            const bool inside = row >= 0 && col >= 0 && row < a.grid.rows && col < a.grid.cols;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside) { a.selecting = true; a.selection = std::array<int, 4>{row, col, row, col}; }
            else if (a.selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && a.selection)
                (*a.selection)[2] = std::clamp(row, 0, a.grid.rows - 1), (*a.selection)[3] = std::clamp(col, 0, a.grid.cols - 1);
            if (a.selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                a.selecting = false;
                if (a.selection && ((*a.selection)[0] != (*a.selection)[2] || (*a.selection)[1] != (*a.selection)[3]))
                    a.window->set_clipboard(console_text(*a.console, a.console_scroll, *a.selection));
                else a.selection.reset();
            }
        }
        // What the program put on the clipboard (OSC 52), when it may.
        std::optional<std::string> wanted;
        {
            std::lock_guard l(a.console->lock());
            wanted = a.console->screen().take_clipboard();
        }
        if (wanted && a.clipboard_grant(a.current).copy) a.window->set_clipboard(*wanted);
    }
    ImGui::SetCursorPos(ImVec2(0, bar));
    a.grid = draw_console(*a.console, {.font = a.window->mono_font(), .bold = a.window->mono_bold_font(),
                                       .size = a.console_size(a.current), .padding = imkit::unit(),
                                       .focused = !a.menu, .scroll = a.console_scroll, .selection = a.selection});
    mark_(a, "console");
    a.console->resize(a.grid.rows, a.grid.cols);
    ImGui::End();
    if (!a.console->alive()) {
        const bool machine = a.console->is_machine();
        // A session that failed says why on the way out: its last lines are
        // the reason shown, not a blank "off".
        a.ended_why.clear();
        if (a.console->exit_code() != 0) {
            std::lock_guard l(a.console->lock());
            auto& screen = a.console->screen();
            std::vector<std::string> lines;
            for (int row = 0; row < screen.rows(); ++row) {
                std::string line;
                for (int col = 0; col < screen.cols(); ++col) {
                    const auto& cell = screen.at(row, col);
                    if (cell.continuation) continue;
                    append_utf8_(line, cell.ch ? cell.ch : U' ');
                }
                while (!line.empty() && line.back() == ' ') line.pop_back();
                if (!line.empty()) lines.push_back(line);
            }
            if (lines.size() > 6) lines.erase(lines.begin(), lines.end() - 6);
            for (const auto& l2 : lines) a.ended_why += l2 + "\n";
            a.ended_code = a.console->exit_code();
        }
        a.console.reset();
        if (machine && vm::running(a.home, a.current)) {
            a.notice = std::vformat(tr("{} keeps running", "{} 仍在运行"), std::make_format_args(a.current));
            a.back_home();
        } else {
            a.view = View::Off;
        }
        a.window->set_title("Luban OS");
    }
}

// nano's view: one line a command, run in it, its output kept.
void run_view_(App& a) {
    begin_full_("run");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    const float bar = system_bar_(a, tr("commands", "命令"));
    const float inputH = px(56);
    // The output.
    ImGui::SetCursorPos(ImVec2(0, bar));
    dl->AddRectFilled(ImVec2(o.x, o.y + bar), ImVec2(o.x + W, o.y + H), IM_COL32(19, 21, 26, 255));
    ImGui::SetCursorPos(ImVec2(imkit::unit(), bar + imkit::unit()));
    ImGui::BeginChild("output", ImVec2(W - imkit::unit() * 2, H - bar - inputH - imkit::unit() * 2), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        std::lock_guard l(a.run.m);
        auto* odl = ImGui::GetWindowDrawList();
        auto* mono = static_cast<ImFont*>(a.window->mono_font());
        const float size = a.console_size(a.current) * imkit::scale();
        const float lh = std::round(size * 1.25f);
        for (const auto& line : a.run.lines) {
            const ImU32 c = line.command ? IM_COL32(115, 239, 247, 255) : line.error ? IM_COL32(239, 125, 87, 255) : IM_COL32(216, 222, 233, 255);
            std::string_view rest = line.text;
            for (;;) {
                const auto nl = rest.find('\n');
                const auto part = rest.substr(0, nl);
                odl->AddText(mono, size, ImGui::GetCursorScreenPos(), c, part.data(), part.data() + part.size());
                ImGui::Dummy(ImVec2(0, lh));
                if (nl == std::string_view::npos) break;
                rest.remove_prefix(nl + 1);
            }
        }
        if (a.run.scroll_down) { ImGui::SetScrollHereY(1.0f); a.run.scroll_down = false; }
    }
    ImGui::EndChild();
    mark_(a, "run:output");
    // The line to type into.
    const float y = H - inputH + px(8);
    ImGui::SetCursorPos(ImVec2(imkit::unit(), y));
    const bool busy = a.run.busy;
    imkit::text(dl, ImVec2(o.x + imkit::unit(), o.y + y + px(10)), busy ? "…" : ">", th_().accent_hi, 16);
    ImGui::SetCursorPos(ImVec2(imkit::unit() + px(20), y));
    if (!ImGui::GetIO().WantTextInput && !a.menu) ImGui::SetKeyboardFocusHere();
    const bool entered = imkit::text_field("run:input", a.run.input, sizeof a.run.input, (W - imkit::unit() * 2 - px(20)) / imkit::scale(),
                                           ImGuiInputTextFlags_EnterReturnsTrue,
                                           tr("a command, e.g. xlings list (Esc: back to the list)", "一条命令，例如 xlings list（Esc：回到列表）"));
    mark_(a, "run:input");
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Escape") { a.back_home(); return; }
        if ((k.name == "Up" || k.name == "Down") && !a.run.history.empty()) {
            const int n = static_cast<int>(a.run.history.size());
            a.run.history_at = k.name == "Up" ? (a.run.history_at < 0 ? n - 1 : std::max(0, a.run.history_at - 1))
                                              : (a.run.history_at < 0 ? -1 : a.run.history_at + 1 >= n ? -1 : a.run.history_at + 1);
            const auto& text = a.run.history_at < 0 ? std::string{} : a.run.history[static_cast<std::size_t>(a.run.history_at)];
            std::snprintf(a.run.input, sizeof a.run.input, "%s", text.c_str());
        }
    }
    if (entered && !busy && a.run.input[0]) {
        const std::string line = a.run.input;
        a.run.input[0] = 0;
        a.run.history.push_back(line);
        a.run.history_at = -1;
        auto argv = platform::split_plain_words(line);
        std::lock_guard l(a.run.m);
        a.run.lines.push_back({"> " + line, true});
        a.run.scroll_down = true;
        if (!argv || argv->empty()) {
            a.run.lines.push_back({std::string(tr("Plain words only: there is no shell here to read quotes, pipes or $.",
                                                  "只能是普通的词：这里没有 shell 来解释引号、管道或 $。")), false, true});
            return;
        }
        a.run.busy = true;
        auto* app = &a;
        auto* w = a.window.get();
        a.run.worker = std::make_unique<std::jthread>([app, w, argv = *argv, name = a.current, port = a.ctx.port] {
            std::string pending_err;
            const int rc = port.capability("subos_exec", Json{{"name", name}, {"argv", argv}}, [&](const service::Event& e) {
                if (e.kind == service::Event::Kind::Data && e.data_kind == "subos_exec_output" && e.data.is_object()) {
                    auto text = e.data.value("data", std::string{});
                    if (e.data.value("encoding", std::string{}) == "base64") text = "(binary output)";
                    const bool err = e.data.value("stream", std::string{}) == "stderr";
                    std::lock_guard l(app->run.m);
                    while (!text.empty() && text.back() == '\n') text.pop_back();
                    if (!text.empty()) app->run.lines.push_back({text, false, err});
                    app->run.scroll_down = true;
                } else if (e.kind == service::Event::Kind::Error) {
                    std::lock_guard l(app->run.m);
                    app->run.lines.push_back({e.hint.empty() ? e.text : e.text + " -- " + e.hint, false, true});
                }
                w->wake();
            });
            {
                std::lock_guard l(app->run.m);
                if (rc != 0) app->run.lines.push_back({std::format("(exit {})", rc), false, true});
                app->run.scroll_down = true;
            }
            app->run.busy = false;
            w->wake();
        });
    }
}

// Reads an archive for its preview: what it is and what it may do.
ImportPreview preview_import_(const fs::path& file, const std::vector<std::string>& taken) {
    ImportPreview p;
    p.file = file;
    auto manifest = luban::service::archive::read(file);
    if (!manifest) { p.error = manifest.error(); return p; }
    const auto& m = *manifest;
    if (m.contains("edition") && m["edition"].is_object()) p.edition = m["edition"].value("ref", "");
    if (m.contains("packages") && m["packages"].is_object()) p.packages = m["packages"].size();
    if (m.contains("policy") && m["policy"].is_object()) {
        if (auto pol = xlings::subos::policy::from_json(m["policy"])) {
            p.preset = std::string(xlings::subos::policy::to_string(pol->preset));
            if (pol->preset == xlings::subos::policy::Preset::Legacy)
                p.wider.push_back(std::string(tr("no isolation preset (as an environment made before policies)",
                                                 "没有隔离预设（如同策略出现之前的环境）")));
            // A grant as what it lets the system do.
            static constexpr std::array<std::array<std::string_view, 3>, 6> kGrants{{
                {"clipboard", "can copy to this computer's clipboard", "可以复制到这台电脑的剪贴板"},
                {"clipboard-paste", "can read this computer's clipboard", "可以读取这台电脑的剪贴板"},
                {"display", "can show windows on this computer's screen", "可以在这台电脑的屏幕上显示窗口"},
                {"audio", "can play and record sound", "可以播放和录制声音"},
                {"gpu", "can use this computer's graphics card", "可以使用这台电脑的显卡"},
                {"ssh-agent", "can use your SSH keys", "可以使用你的 SSH 密钥"},
            }};
            for (const auto& g : pol->grants) {
                const auto known = std::ranges::find_if(kGrants, [&](const auto& k) { return k[0] == g; });
                p.wider.push_back(known != kGrants.end() ? std::string(tr((*known)[1], (*known)[2]))
                                                         : std::vformat(tr("granted: {}", "授权：{}"), std::make_format_args(g)));
            }
            for (const auto& mnt : pol->mounts)
                p.wider.push_back(std::format("{} {} ({})", tr("mounts the host's", "挂载宿主的"), mnt.src, mnt.rw ? "rw" : "ro"));
        } else {
            p.wider.push_back(std::vformat(tr("a policy this version cannot read: {}", "本版本读不懂的策略：{}"),
                                           std::make_format_args(pol.error())));
        }
    }
    // A name: the file's, made unique.
    std::string base = file.stem().string();
    std::ranges::replace_if(base, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_'; }, '-');
    if (base.empty()) base = "imported";
    std::string name = base;
    for (int k = 2; std::ranges::find(taken, name) != taken.end() && k < 100; ++k) name = std::format("{}-{}", base, k);
    std::snprintf(p.name, sizeof p.name, "%s", name.c_str());
    return p;
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
    // Pacing: what it sent is shown with this frame; it may draw the next.
    if (!updates.empty()) d.send(opensal::FramePresented{});
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (a.texture.handle) {
        // Its device pixels at the screen's pixel density (1:1), smaller when it does not fit.
        const float density = static_cast<float>(a.pixel_density());
        const float scale = std::min({1.0f / density, avail.x / a.texture.width, avail.y / a.texture.height});
        const ImVec2 shown(a.texture.width * scale, a.texture.height * scale);
        const ImVec2 at(std::round(origin.x + (avail.x - shown.x) / 2), std::round(origin.y + (avail.y - shown.y) / 2));
        ImGui::SetCursorScreenPos(at);
        ImGui::Dummy(shown);
        // Drawn in tiles: SDL's software renderer (no GPU, as in a VM) draws
        // nothing of one triangle much bigger than about 1000x700.
        const auto id = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(a.texture.handle));
        constexpr int kTile = 512;
        for (int ty = 0; ty < a.texture.height; ty += kTile)
            for (int tx = 0; tx < a.texture.width; tx += kTile) {
                const int tw = std::min(kTile, a.texture.width - tx), th = std::min(kTile, a.texture.height - ty);
                const ImVec2 p0(at.x + tx * scale, at.y + ty * scale), p1(at.x + (tx + tw) * scale, at.y + (ty + th) * scale);
                const ImVec2 uv0(static_cast<float>(tx) / a.texture.width, static_cast<float>(ty) / a.texture.height);
                const ImVec2 uv1(static_cast<float>(tx + tw) / a.texture.width, static_cast<float>(ty + th) / a.texture.height);
                ImGui::GetWindowDrawList()->AddImage(id, p0, p1, uv0, uv1);
            }
        mark_(a, "desktop");
        // Over its picture the guest's own pointer is drawn into the frames
        // (the compositor's theme); elsewhere -- the bars around a picture
        // smaller than the window -- the host's arrow shows (design
        // 2026-10-16 L §13.9).
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        // Its caret: the host's input method writes there (in window pixels).
        if (auto t = d.take_text_input()) {
            int on = 0, x = 0, y = 0, w = 1, h = 1;
            std::istringstream in(*t);
            in >> on >> x >> y >> w >> h;
            a.desktop_text = on == 1;
            if (a.desktop_text)
                a.window->text_input(true, static_cast<int>(at.x + x * scale), static_cast<int>(at.y + y * scale),
                                     std::max(1, static_cast<int>(w * scale)), std::max(1, static_cast<int>(h * scale)));
            else a.window->text_input(false);
        }
        // What it copied is the host's clipboard (it is not pasted back).
        if (auto c = d.take_clipboard()) {
            a.window->set_clipboard(*c);
            a.pasted = *c;
        }
        // The host's clipboard into it, when it is granted: as it opens,
        // whenever it changes, and whenever the window comes back to it.
        auto paste = [&] {
            if (!d.clipboard_grant().paste) return;
            auto text = a.window->clipboard();
            if (text.empty() || text == a.pasted) return;
            a.pasted = text;
            d.send(opensal::ClipboardText{std::move(text)});
        };
        if (std::exchange(a.paste_on_open, false)) paste();
        if (!a.menu && !a.close_dialog) {
            for (const auto& e : a.window->events()) {
                if (const auto* m = std::get_if<opensal::PointerMotion>(&e)) {
                    d.send(opensal::PointerMotion{(m->x - at.x) / scale, (m->y - at.y) / scale});
                } else if (const auto* k = std::get_if<opensal::Key>(&e)) {
                    // While it takes text, a character is the input method's
                    // to give (as text), not a key's.
                    const bool chord = k->mods & (opensal::Ctrl | opensal::Alt | opensal::Super);
                    const bool character = (k->keysym >= 0x20 && k->keysym <= 0x7e) || (k->keysym >= 0xa0 && k->keysym <= 0xff)
                                           || k->keysym >= 0x01000000;
                    if (!(a.desktop_text && character && !chord)) d.send(e);
                } else if (std::holds_alternative<opensal::TextCommit>(e) || std::holds_alternative<opensal::Preedit>(e)) {
                    if (a.desktop_text) d.send(e);
                } else if (const auto* f = std::get_if<opensal::Focus>(&e)) {
                    d.send(e);
                    if (f->in) paste();
                } else if (std::holds_alternative<opensal::ClipboardText>(e)) {
                    paste();    // the host's clipboard changed
                } else if (std::holds_alternative<opensal::PointerButton>(e) || std::holds_alternative<opensal::Wheel>(e)) {
                    d.send(e);
                }
            }
        }
    } else {
        auto* dl = ImGui::GetWindowDrawList();
        const float cell = std::round(px(4));
        art::badge(dl, art::Badge::Craftsman, "", ImVec2(origin.x + (avail.x - 16 * cell) / 2, origin.y + avail.y * 0.32f), cell,
                   imkit::beat(2, 0.3));
        centred_(dl, avail.y * 0.32f + 16 * cell + px(20),
                 std::vformat(tr("Starting the desktop of {}…", "正在启动 {} 的桌面…"), std::make_format_args(a.current)), th_().text_dim, 16);
    }
    // The window's size is the desktop's, once it has held still a moment.
    // The screen's scale, said once and whenever it changes (a screen with
    // another scale); the size it wants, in device pixels.
    if (const double density = a.density(); std::abs(density - a.sent_density) > 0.01) {
        d.send(opensal::DensityChanged{density});
        a.sent_density = density;
    }
    const opensal::Size want = a.device_size_(static_cast<int>(avail.x), static_cast<int>(avail.y));
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
            a.back_home();
        }
    }
}

// ── management ──────────────────────────────────────────────────────────

// A setting's row: its name, the control, and the command that sets it.
struct Rows_ {
    App& a;
    ImDrawList* dl;
    ImVec2 o;
    float x, y, width;
    void label(std::string_view s) {
        imkit::text(dl, ImVec2(o.x + x, o.y + y + px(10)), s, th_().text_dim, 13, true);
    }
    void command(std::string_view s) {
        imkit::text(dl, ImVec2(o.x + x + px(200), o.y + y + px(42)), imkit::fit(s, width - px(200), 12), th_().text_faint, 12);
    }
    // A choice among words; the index chosen.
    int choice(std::string_view id, std::string_view name, const std::vector<std::string>& words, int now,
               std::string_view cmd = {}) {
        label(name);
        ImGui::SetCursorPos(ImVec2(x + px(200), y));
        const int chosen = imkit::segmented(id, words, now, std::min(width - px(200), px(420)) / imkit::scale());
        if (!cmd.empty()) command(cmd);
        y += cmd.empty() ? px(52) : px(66);
        return chosen;
    }
};

void check_system_(App& a, const std::string& name);

void system_pages_(App& a, ImVec2 o, float x0, float y0, float width, float height) {
    auto* dl = ImGui::GetWindowDrawList();
    if (a.admin_selected >= static_cast<int>(a.systems.size())) return;
    auto& e = a.systems[static_cast<std::size_t>(a.admin_selected)];
    const auto edition = App::edition_name(e);
    const bool on = a.is_running(e.name) || e.running;
    // Its head: badge, name, edition, state.
    const float cell = std::round(px(3));
    art::badge(dl, art::badge_of(edition), edition, ImVec2(o.x + x0, o.y + y0), cell, imkit::beat(2, 0.45));
    imkit::text(dl, ImVec2(o.x + x0 + 16 * cell + px(14), o.y + y0 + px(2)), imkit::fit(e.name, width - px(200), 24, true), th_().text, 24, true);
    imkit::text(dl, ImVec2(o.x + x0 + 16 * cell + px(14), o.y + y0 + px(30)),
                std::format("{} · {} {} · {}", edition_label_(e), e.packages, tr("packages", "个包"),
                            on ? tr("running", "运行中") : tr("stopped", "已停止")),
                th_().text_dim, 14);
    ImGui::SetCursorPos(ImVec2(x0 + width - px(120), y0 + px(4)));
    if (imkit::button("admin:enter", tr("Go in", "进入"), ImVec2(120, 40), imkit::Look::Primary, true, false, imkit::Icon::Run)) a.enter(e.name);
    mark_(a, "admin:enter");
    float y = y0 + 16 * cell + px(18);
    ImGui::SetCursorPos(ImVec2(x0, y));
    const std::vector<std::string> pages{std::string(tr("Overview", "概览")), std::string(tr("Versions", "版本")),
                                         std::string(tr("Settings", "设置")), std::string(tr("Export & remove", "导出与删除"))};
    a.admin_page = imkit::tabs("page", pages, a.admin_page, {"page:overview", "page:versions", "page:settings", "page:danger"});
    y = ImGui::GetCursorPosY() + px(16);
    Rows_ r{a, dl, o, x0, y, width};
    if (a.admin_page == 0) {
        auto fact = [&](std::string_view k, const std::string& v) {
            r.label(k);
            imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)), imkit::fit(v, width - px(200)), th_().text);
            r.y += px(34);
        };
        fact(tr("EDITION", "EDITION"), e.edition.empty() ? std::string("—") : e.edition);
        fact(tr("GENERATION", "代"), e.generation ? std::to_string(*e.generation) : std::string("—"));
        fact(tr("RUNS AS", "运行方式"), e.settings.mode);
        fact(tr("KEEPS", "保留"), e.settings.persist);
        if (!e.shell.empty()) fact(tr("SHELL", "SHELL"), e.shell);
        fact(tr("CARRIER", "载体"), e.carrier);
        // Its self-check (design 2026-10-16 L §11), asked here.
        r.y += px(8);
        r.label(tr("SELF-CHECK", "自检"));
        ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
        bool checking;
        {
            std::lock_guard cl(a.check_m);
            checking = a.checking.contains(e.name);
        }
        if (imkit::button("admin:check", checking ? tr("Checking…", "检查中…") : tr("Check it", "检查"), ImVec2(140, 36),
                          imkit::Look::Normal, !checking, false, imkit::Icon::Check))
            check_system_(a, e.name);
        mark_(a, "admin:check");
        r.y += px(48);
        std::lock_guard l(a.check_m);
        if (auto it = a.checks.find(e.name); it != a.checks.end() && it->second.is_object()) {
            for (const auto& item : it->second.value("items", Json::array())) {
                const bool ok = item.value("ok", true), critical = item.value("critical", false);
                imkit::icon(dl, ok ? imkit::Icon::Check : critical ? imkit::Icon::Cross : imkit::Icon::Warn,
                            ImVec2(o.x + x0 + px(200), o.y + r.y + px(3)), std::max(1.0f, std::round(px(1.25f))),
                            ok ? th_().good : critical ? th_().bad : th_().warn);
                std::string line = item.value("what", std::string{});
                if (!ok && item.contains("detail")) line += " — " + item.value("detail", std::string{});
                imkit::text(dl, ImVec2(o.x + x0 + px(226), o.y + r.y), imkit::fit(line, width - px(230), 14), ok ? th_().text_dim : th_().text, 14);
                r.y += px(24);
                if (r.y > y0 + height - px(24)) break;
            }
        } else if (it != a.checks.end()) {
            imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y), imkit::fit(it->second.dump(), width - px(200), 13), th_().warn, 13);
        }
    } else if (a.admin_page == 1) {
        if (a.history_for != e.name) {
            a.history_for = e.name;
            a.history = service::history(a.ctx, e.name).value;
            a.upgrade_plan.reset();
        }
        ImGui::SetCursorPos(ImVec2(x0, r.y));
        if (imkit::button("admin:upgrade-plan", tr("What would an upgrade do?", "升级会做什么？"), ImVec2(240, 38)))
            a.upgrade_plan = service::upgrade(a.ctx, e.name, "", true).value;
        mark_(a, "admin:upgrade-plan");
        ImGui::SetCursorPos(ImVec2(x0 + px(252), r.y));
        const auto name = e.name;
        if (imkit::button("admin:upgrade", tr("Upgrade", "升级"), ImVec2(140, 38), imkit::Look::Primary))
            a.start_job(std::format("{} {}", tr("Upgrading", "升级"), name),
                        [name](Job& j, service::Context c) { j.code = service::upgrade(c, name, "", false).code; },
                        [](App& app) { app.refresh(); app.history_for.clear(); app.view = View::Admin; });
        mark_(a, "admin:upgrade");
        r.y += px(54);
        if (a.upgrade_plan) {
            const auto said = a.upgrade_plan->value("up_to_date", false) ? std::string(tr("It is up to date.", "已是最新。"))
                                                                         : a.upgrade_plan->dump();
            r.y += paragraph_(dl, ImVec2(o.x + x0, o.y + r.y), imkit::fit(said, width * 3), width, th_().text_dim, 13) + px(10);
        }
        if (a.history)
            for (const auto& g : a.history->value("generations", Json::array())) {
                const int n = g.value("generation", 0);
                const bool current = g.value("current", false);
                imkit::text(dl, ImVec2(o.x + x0, o.y + r.y + px(8)), std::format("{} {:>3}", current ? "▶" : " ", n),
                            current ? th_().accent_hi : th_().text_dim, 15, true);
                imkit::text(dl, ImVec2(o.x + x0 + px(70), o.y + r.y + px(8)),
                            imkit::fit(std::format("{}  {}", g.value("created", std::string{}), g.value("reason", std::string{})), width - px(240), 14),
                            th_().text, 14);
                if (!current) {
                    ImGui::SetCursorPos(ImVec2(x0 + width - px(140), r.y));
                    const auto to = std::to_string(n);
                    if (imkit::button(std::format("admin:rollback:{}", n), tr("Go back to it", "回到这一代"), ImVec2(140, 34)))
                        a.start_job(std::format("{} {}", tr("Going back", "回滚"), name),
                                    [name, to](Job& j, service::Context c) { j.code = service::rollback(c, name, to).code; },
                                    [](App& app) { app.refresh(); app.history_for.clear(); app.view = View::Admin; });
                    mark_(a, std::format("admin:rollback:{}", n));
                }
                r.y += px(42);
                if (r.y > y0 + height - px(40)) break;
            }
    } else if (a.admin_page == 2) {
        // Applied now, the list read again after the frame: this page is
        // still drawing from it.
        auto set = [&](const char* key, const std::string& value) {
            (void)service::configure(a.ctx, e.name, {{key, value}});
            a.refresh_after_frame = true;
            a.page_cache_for.clear();
        };
        const std::vector<std::string> modes{"sandbox", "machine"}, keeps{"all", "home", "none"}, closes{"ask", "background", "poweroff"};
        auto index = [](const std::vector<std::string>& v, const std::string& now) {
            const auto it = std::ranges::find(v, now);
            return it == v.end() ? 0 : static_cast<int>(it - v.begin());
        };
        const auto name = e.name;
        int k = index(modes, e.settings.mode);
        if (int c = r.choice("setting:mode", tr("RUNS AS", "运行方式"), {std::string(tr("sandbox", "沙箱")), std::string(tr("machine", "机器"))}, k,
                             std::format("luban config {} --mode {}", name, modes[static_cast<std::size_t>(k)])); c != k)
            set("mode", modes[static_cast<std::size_t>(c)]);
        k = index(keeps, e.settings.persist);
        if (int c = r.choice("setting:persist", tr("KEEPS", "保留修改"), {std::string(tr("all", "全部")), std::string(tr("/home", "仅 /home")),
                                                                     std::string(tr("nothing", "不保留"))}, k,
                             std::format("luban config {} --persist {}", name, keeps[static_cast<std::size_t>(k)])); c != k)
            set("persist", keeps[static_cast<std::size_t>(c)]);
        k = index(closes, e.settings.on_close);
        if (int c = r.choice("setting:on-close", tr("CLOSING", "关窗口时"), {std::string(tr("ask", "询问")), std::string(tr("background", "后台运行")),
                                                                         std::string(tr("power off", "关机"))}, k,
                             std::format("luban config {} --on-close {}", name, closes[static_cast<std::size_t>(k)])); c != k)
            set("on-close", closes[static_cast<std::size_t>(c)]);
        if (a.page_cache_for != e.name || std::chrono::steady_clock::now() - a.page_at > std::chrono::seconds(2)) {
            a.page_cache_for = e.name;
            a.page_at = std::chrono::steady_clock::now();
            a.page_desktops.clear();
            if (const auto usr = service::usr_of(a.home, e.name))
                for (const auto& sess : opensal::wlr::sessions({*usr / "share" / "wayland-sessions"})) a.page_desktops.push_back(sess.id);
            a.page_grant = a.clipboard_grant(e.name);
        }
        {
            // The desktop: the sessions its root declares, or none (its console).
            std::vector<std::string> ids{"", "none"};
            for (const auto& id : a.page_desktops) ids.push_back(id);
            if (ids.size() > 2) {
                std::vector<std::string> words{std::string(tr("its own", "默认")), std::string(tr("none", "无"))};
                for (std::size_t i = 2; i < ids.size(); ++i) words.push_back(ids[i]);
                k = index(ids, e.settings.desktop);
                if (int c = r.choice("setting:desktop", tr("DESKTOP", "桌面"), words, k,
                                     std::format("luban config {} --desktop {}", name, ids[static_cast<std::size_t>(k)].empty() ? "''" : ids[static_cast<std::size_t>(k)]));
                    c != k)
                    set("desktop", ids[static_cast<std::size_t>(c)]);
            }
        }
        if (!e.shell.empty()) {
            const std::vector<std::string> shells{"fish", "bash", "sh"};
            const auto now = fs::path(e.shell).filename().string();
            k = index(shells, now);
            if (int c = r.choice("setting:shell", tr("SHELL", "SHELL"), shells, k, std::format("luban config {} --shell {}", name, now)); c != k)
                set("shell", shells[static_cast<std::size_t>(c)]);
        }
        {
            // What may cross the clipboard: its grants.
            bool copy = a.page_grant.copy, paste = a.page_grant.paste;
            r.label(tr("CLIPBOARD", "剪贴板"));
            ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y + px(4)));
            if (imkit::toggle("setting:clipboard", tr("copy out of it", "从中复制出来"), copy))
                set(copy ? "allow" : "disallow", "clipboard");
            mark_(a, "setting:clipboard");
            ImGui::SetCursorPos(ImVec2(x0 + px(200) + px(220), r.y + px(4)));
            if (imkit::toggle("setting:clipboard-paste", tr("paste into it", "粘贴进去"), paste))
                set(paste ? "allow" : "disallow", "clipboard-paste");
            mark_(a, "setting:clipboard-paste");
            r.y += px(52);
        }
        {
            r.label(tr("PROXY", "代理"));
            ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
            imkit::text_field("setting:proxy", a.proxy_edit, sizeof a.proxy_edit, std::min(px(300), width - px(340)) / imkit::scale(), 0,
                              tr("socks5h://host:port, or off", "socks5h://主机:端口，或 off"));
            mark_(a, "setting:proxy");
            ImGui::SetCursorPos(ImVec2(x0 + px(200) + std::min(px(300), width - px(340)) + px(12), r.y));
            if (imkit::button("setting:proxy:apply", tr("Apply", "应用"), ImVec2(110, 38), imkit::Look::Normal, a.proxy_edit[0] != 0))
                set("proxy", a.proxy_edit);
            mark_(a, "setting:proxy:apply");
            r.y += px(52);
        }
    } else {
        // Export, then the danger zone.
        r.label(tr("EXPORT TO", "导出到"));
        ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
        if (!a.export_path[0]) {
            const auto file = (fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / (e.name + ".luban")).string();
            std::snprintf(a.export_path, sizeof a.export_path, "%s", file.c_str());
        }
        const float fw = std::min(px(420), width - px(340));
        imkit::text_field("admin:export-path", a.export_path, sizeof a.export_path, fw / imkit::scale(), 0, "box.luban / box.img / box.iso");
        mark_(a, "admin:export-path");
        ImGui::SetCursorPos(ImVec2(x0 + px(200) + fw + px(12), r.y));
        const auto name = e.name;
        if (imkit::button("admin:export", tr("Export", "导出"), ImVec2(110, 38), imkit::Look::Normal, a.export_path[0] != 0)) {
            const fs::path file = a.export_path;
            a.start_job(std::format("{} {}", tr("Exporting", "导出"), name),
                        [name, file](Job& j, service::Context c) { j.code = service::export_environment(c, {.name = name, .file = file}).code; },
                        [](App& app) { app.view = View::Admin; app.notice = std::string(tr("Exported.", "已导出。")); });
        }
        mark_(a, "admin:export");
        r.command(std::format("luban export {} <file>  (.luban .tar .img .qcow2 .iso)", name));
        r.y += px(80);
        imkit::panel(dl, ImVec2(o.x + x0, o.y + r.y), ImVec2(o.x + x0 + width, o.y + r.y + px(120)), imkit::Bevel::Flat,
                     IM_COL32(60, 22, 30, 255));
        imkit::icon(dl, imkit::Icon::Warn, ImVec2(o.x + x0 + px(16), o.y + r.y + px(16)), std::round(px(1.5f)), th_().bad);
        imkit::text(dl, ImVec2(o.x + x0 + px(44), o.y + r.y + px(14)), tr("DANGER ZONE", "危险区"), th_().bad, 15, true);
        imkit::text(dl, ImVec2(o.x + x0 + px(16), o.y + r.y + px(44)),
                    imkit::fit(std::vformat(tr("Removing {} deletes its home and every file in it.", "删除 {} 会删除它的 home 以及其中所有文件。"),
                                            std::make_format_args(name)), width - px(200), 14),
                    th_().text_dim, 14);
        ImGui::SetCursorPos(ImVec2(x0 + width - px(160), r.y + px(66)));
        if (on && imkit::button("admin:poweroff", tr("Power off", "关机"), ImVec2(140, 38), imkit::Look::Normal, true, false, imkit::Icon::Power))
            a.power_off(name);
        ImGui::SetCursorPos(ImVec2(x0 + px(16), r.y + px(66)));
        if (imkit::button("admin:remove", tr("Remove…", "删除…"), ImVec2(140, 38), imkit::Look::Danger)) {
            a.confirm = Confirm{std::vformat(tr("Remove {}?", "删除 {}？"), std::make_format_args(name)),
                                std::string(tr("Its home and every file in it are deleted. This cannot be undone.",
                                               "它的 home 以及其中所有文件都会被删除，无法撤销。")),
                                std::string(tr("Remove", "删除")), true, [&a, name] {
                                    a.start_job(std::format("{} {}", tr("Removing", "删除"), name),
                                                [name](Job& j, service::Context c) { j.code = service::remove(c, name).code; },
                                                [](App& app) { app.refresh(); app.view = View::Admin; });
                                }};
        }
        mark_(a, "admin:remove");
    }
}

void check_system_(App& a, const std::string& name) {
    {
        std::lock_guard l(a.check_m);
        a.checking.insert(name);
    }
    auto* app = &a;
    auto* w = a.window.get();
    a.check_jobs.push_back(std::make_unique<std::jthread>([app, w, name, port = a.ctx.port] {
        auto report = port.json({"subos", "exec", name, "--", "/run/xlings/xlings", "subos", "__check", "--json"});
        {
            std::lock_guard l(app->check_m);
            app->checks[name] = report ? *report : Json(report.error());
            app->checking.erase(name);
        }
        w->wake();
    }));
}

void app_settings_(App& a, ImVec2 o, float x0, float y0, float width) {
    auto* dl = ImGui::GetWindowDrawList();
    Rows_ r{a, dl, o, x0, y0, width};
    const std::vector<std::string> langs{"auto", "en", "zh"};
    int k = static_cast<int>(std::ranges::find(langs, a.settings.language) - langs.begin());
    if (int c = r.choice("app:language", tr("LANGUAGE", "语言"), {std::string(tr("as the system", "跟随系统")), "English", "中文"}, k); c != k) {
        a.settings.language = langs[static_cast<std::size_t>(c)];
        set_language(a.settings.language);
        write_app_settings(a.home, a.settings);
    }
    const std::vector<float> sizes{14, 16, 18, 20};
    k = static_cast<int>(std::ranges::find(sizes, a.settings.console_size) - sizes.begin());
    if (int c = r.choice("app:console-size", tr("CONSOLE TEXT", "控制台字号"), {"14", "16", "18", "20"}, k); c != k) {
        a.settings.console_size = sizes[static_cast<std::size_t>(c)];
        write_app_settings(a.home, a.settings);
    }
    const std::vector<int> counts{0, 5, 10, 30};
    k = static_cast<int>(std::ranges::find(counts, a.settings.countdown) - counts.begin());
    if (int c = r.choice("app:countdown", tr("GO IN BY ITSELF AFTER", "自动进入"), {std::string(tr("never", "从不")), "5 s", "10 s", "30 s"}, k);
        c != k) {
        a.settings.countdown = counts[static_cast<std::size_t>(c)];
        write_app_settings(a.home, a.settings);
    }
    auto toggle = [&](const char* id, std::string_view label, bool& value) {
        ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
        if (imkit::toggle(id, label, value)) write_app_settings(a.home, a.settings);
        mark_(a, id);
        r.y += px(40);
    };
    r.label(tr("MOTION", "动效"));
    toggle("app:reduce-motion", tr("Reduce motion", "减少动效"), a.settings.reduce_motion);
    r.label(tr("TRAY", "状态栏"));
    toggle("app:minimize-to-tray", tr("Minimize to the tray", "最小化到状态栏"), a.settings.minimize_to_tray);
    toggle("app:keep-in-tray", tr("Always in the tray", "常驻状态栏"), a.settings.keep_in_tray);
    r.y += px(8);
    r.label(tr("DEFAULT SYSTEM", "默认系统"));
    imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)),
                a.settings.default_system.empty() ? std::string(tr("the one opened last", "最近打开的")) : a.settings.default_system,
                th_().text);
    r.y += px(40);
    r.label(tr("VERSION", "版本"));
    imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)), std::format("luban-os {} · {}", service::kVersion,
                                                                              tr("update: xlings self update", "更新：xlings self update")),
                th_().text_dim, 14);
    r.y += px(40);
    r.label(tr("FONTS", "字体"));
    imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)),
                imkit::fit(a.window->font_dir().empty() ? std::string(tr("this computer's (install luban-fonts for Luban OS's own)",
                                                                         "这台电脑的（安装 luban-fonts 使用 Luban OS 自带字体）"))
                                                        : a.window->font_dir().string(), width - px(200), 13),
                th_().text_dim, 13);
}

void activity_(App& a, ImVec2 o, float x0, float y0, float width) {
    auto* dl = ImGui::GetWindowDrawList();
    // The operations' logs, read every few seconds, not every frame.
    if (std::chrono::steady_clock::now() - a.activity_at > std::chrono::seconds(3)) {
        a.activity_at = std::chrono::steady_clock::now();
        a.activity_rows.clear();
        a.activity_refs.clear();
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(a.home / "logs" / "ops", ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".ndjson") files.push_back(it->path());
        std::ranges::sort(files, [](const auto& x, const auto& y) {
            std::error_code e1, e2;
            return fs::last_write_time(x, e1) > fs::last_write_time(y, e2);
        });
        for (std::size_t k = 0; k < std::min<std::size_t>(files.size(), 40); ++k) {
            std::ifstream in(files[k]);
            std::string first;
            std::getline(in, first);
            auto j = Json::parse(first, nullptr, false);
            // "2026-10-12T20:12:09Z" as a person reads it: the date and the time, in UTC.
            auto when = j.is_object() ? j.value("ts", std::string{}) : std::string{};
            if (when.size() >= 16 && when[10] == 'T') when = when.substr(0, 10) + "  " + when.substr(11, 5) + " UTC";
            std::string cmd = j.is_object() ? j.value("client", "") : "";
            if (j.is_object()) for (const auto& w : j.value("argv", Json::array())) if (w.is_string()) cmd += " " + w.get<std::string>();
            a.activity_rows.emplace_back(when, cmd);
            a.activity_refs.push_back(std::format("xlings self log {}", files[k].stem().string().substr(0, 12)));
        }
    }
    float y = y0;
    if (a.activity_rows.empty()) imkit::text(dl, ImVec2(o.x + x0, o.y + y), tr("Nothing done yet.", "还没有操作。"), th_().text_faint);
    for (std::size_t k = 0; k < a.activity_rows.size(); ++k) {
        const auto& [when, cmd] = a.activity_rows[k];
        if (k % 2 == 0) dl->AddRectFilled(ImVec2(o.x + x0, o.y + y), ImVec2(o.x + x0 + width, o.y + y + px(30)), th_().bg_deep);
        imkit::text(dl, ImVec2(o.x + x0 + px(8), o.y + y + px(6)), when, th_().text_dim, 13);
        imkit::text(dl, ImVec2(o.x + x0 + px(190), o.y + y + px(6)), imkit::fit(cmd, width - px(420), 13), th_().text, 13);
        const auto& ref = a.activity_refs[k];
        imkit::text(dl, ImVec2(o.x + x0 + width - imkit::text_size(ref, 12).x - px(8), o.y + y + px(7)), ref, th_().text_faint, 12);
        y += px(30);
        if (y > ImGui::GetWindowHeight() - px(40)) break;
    }
}

void admin_(App& a) {
    begin_full_("admin");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    if (page_bar_(a, tr("Manage", "管理"), true, "admin:back")) {
        a.view = a.display ? View::Desktop : a.console ? View::Console : View::Launcher;
        a.refresh();
        ImGui::End();
        return;
    }
    // The top tabs, at the bar's right.
    static const std::vector<std::string> keys{"systems", "activity", "settings"};
    int tab = static_cast<int>(std::ranges::find(keys, a.admin_tab) - keys.begin());
    ImGui::SetCursorPos(ImVec2(std::max(px(260), W - px(460)), px(22)));
    tab = imkit::tabs("top", {std::string(tr("Systems", "系统")), std::string(tr("Activity", "活动")), std::string(tr("App settings", "应用设置"))},
                      tab, {"tab:systems", "tab:activity", "tab:settings"});
    a.admin_tab = keys[static_cast<std::size_t>(std::clamp(tab, 0, 2))];
    const float top = px(84);
    if (a.admin_tab == "systems") {
        // The systems on the left; the chosen one's pages on the right.
        const float side = std::min(px(260), W * 0.3f);
        imkit::panel(dl, ImVec2(o.x + px(16), o.y + top), ImVec2(o.x + px(16) + side, o.y + H - px(16)), imkit::Bevel::Sunken, th_().bg_deep);
        ImGui::SetCursorPos(ImVec2(px(22), top + px(6)));
        ImGui::BeginChild("list", ImVec2(side - px(12), H - top - px(28)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        auto* ldl = ImGui::GetWindowDrawList();
        for (int i = 0; i < static_cast<int>(a.systems.size()); ++i) {
            const auto& e = a.systems[static_cast<std::size_t>(i)];
            if (imkit::list_row("admin:" + e.name, ImVec2(side - px(16), 46), a.admin_selected == i)) {
                a.admin_selected = i;
                a.history_for.clear();
                a.proxy_edit[0] = 0;
                a.export_path[0] = 0;
            }
            mark_(a, "admin:" + e.name);
            const auto mn = ImGui::GetItemRectMin();
            const auto ed = App::edition_name(e);
            const float cell = std::max(1.0f, std::round(px(1.75f)));
            art::badge(ldl, art::badge_of(ed), ed, ImVec2(mn.x + px(10), mn.y + px(23) - 8 * cell), cell);
            imkit::text(ldl, ImVec2(mn.x + px(10) + 16 * cell + px(10), mn.y + px(13)), imkit::fit(e.name, side - px(90), 15), th_().text, 15);
        }
        if (imkit::list_row("admin:new", ImVec2(side - px(16), 46), false)) a.view = View::QuickStart;
        mark_(a, "admin:new");
        imkit::icon(ldl, imkit::Icon::Plus, ImVec2(ImGui::GetItemRectMin().x + px(14), ImGui::GetItemRectMin().y + px(11)),
                    std::round(px(2)), th_().accent);
        imkit::text(ldl, ImVec2(ImGui::GetItemRectMin().x + px(48), ImGui::GetItemRectMin().y + px(13)), tr("New system", "新建系统"), th_().text_dim, 15);
        ImGui::EndChild();
        if (a.systems.empty())
            imkit::text(dl, ImVec2(o.x + side + px(48), o.y + top + px(16)), tr("No system yet.", "还没有系统。"), th_().text_dim);
        else
            system_pages_(a, o, side + px(48), top + px(8), W - side - px(80), H - top - px(24));
    } else if (a.admin_tab == "activity") {
        activity_(a, o, px(24), top + px(8), W - px(48));
    } else {
        app_settings_(a, o, px(24), top + px(8), std::min(W - px(48), px(900)));
    }
    if (!a.notice.empty()) imkit::text(dl, ImVec2(o.x + px(24), o.y + H - px(28)), imkit::fit(a.notice, W - px(48), 13), th_().warn, 13);
    ImGui::End();
}

// What a test asserts on: the view and its state, the window's title, and
// where each named control was drawn.
Json state_(const App& a) {
    static constexpr std::array<std::string_view, 9> names{"launcher", "quick-start", "job", "console", "off", "admin", "desktop", "import", "run"};
    Json j{{"view", std::string(names[static_cast<int>(a.view)])}, {"systems", Json::array()}};
    for (const auto& s : a.systems) j["systems"].push_back(s.name);
    j["recent"] = a.recent;
    if (a.countdown_until) j["countdown_to"] = a.countdown_target();
    j["selected"] = a.selected;
    j["filter"] = a.filter;
    j["menu"] = a.menu;
    j["close_dialog"] = a.close_dialog;
    j["close_choice"] = a.close_choice;
    j["confirm"] = a.confirm ? Json(a.confirm->title) : Json();
    j["notice"] = a.notice;
    j["current"] = a.current;
    j["title"] = a.window ? a.window->title() : std::string{};
    j["edition"] = std::string(kEditions[static_cast<std::size_t>(a.new_edition)].name);
    j["name"] = std::string(a.new_name);
    j["mode"] = a.new_mode == 1 ? "machine" : "sandbox";
    j["admin_tab"] = a.admin_tab;
    j["admin_page"] = a.admin_page;
    j["desktop_text"] = a.desktop_text;
    j["ended_why"] = a.ended_why;
    {
        auto ms = std::vector<double>(a.perf.frame_ms.begin(), a.perf.frame_ms.end());
        std::ranges::sort(ms);
        auto at = [&](double q) { return ms.empty() ? 0.0 : ms[std::min(ms.size() - 1, static_cast<std::size_t>(q * ms.size()))]; };
        j["perf"] = {{"frames", a.perf.frames}, {"frame_ms_p50", at(0.5)}, {"frame_ms_p95", at(0.95)},
                     {"frame_ms_max", ms.empty() ? 0.0 : ms.back()}, {"input_to_next_frame_ms", a.perf.input_to_next_frame_ms},
                     {"input_to_next_frame_ms_max", a.perf.input_to_next_frame_max}, {"wait_after_input_ms", a.perf.wait_after_input_ms},
                     {"idle_wait_ms", a.perf.idle_wait_ms}};
        auto pm = std::vector<double>(a.perf.present_ms.begin(), a.perf.present_ms.end());
        std::ranges::sort(pm);
        j["perf"]["present_ms_p50"] = pm.empty() ? 0.0 : pm[pm.size() / 2];
        // What the app itself spends (its layout and drawing, not the renderer's).
        std::vector<double> dm;
        for (std::size_t k = 0; k < a.perf.frame_ms.size(); ++k) dm.push_back(a.perf.frame_ms[k] - a.perf.present_ms[k]);
        std::ranges::sort(dm);
        j["perf"]["draw_ms_p95"] = dm.empty() ? 0.0 : dm[std::min(dm.size() - 1, dm.size() * 95 / 100)];
        j["perf"]["draw_ms_max"] = dm.empty() ? 0.0 : dm.back();
    }
    j["tray"] = {{"backend", a.tray ? std::string(a.tray->backend()) : std::string{}}, {"error", a.tray_error},
                 {"hidden", a.hidden}, {"said", a.tray_said}};
    j["scale"] = imkit::scale();
    j["unit"] = imkit::unit();
    if (a.window) j["fonts"] = a.window->font_dir().string();
    if (a.view == View::QuickStart) j["quick_start"] = {{"said", a.quick_said}, {"can_make", a.quick_can_make}};
    if (a.import)
        j["import"] = {{"file", a.import->file.string()}, {"error", a.import->error}, {"edition", a.import->edition},
                       {"packages", a.import->packages}, {"wider", a.import->wider}, {"name", std::string(a.import->name)},
                       {"can_go", a.import->error.empty() && (a.import->wider.empty() || a.import->confirmed) && can_run_(const_cast<App&>(a))}};
    {
        const bool can = can_run_(const_cast<App&>(a));
        std::lock_guard l(const_cast<App&>(a).available_m);
        j["host"] = {{"known", a.caps.has_value()}, {"can_run", can}};
        if (a.caps) j["host"]["report"] = service::to_json(*a.caps);
    }
    {
        std::lock_guard l(const_cast<App&>(a).available_m);
        for (const auto& [name, v] : a.available)
            j["available"][name] = v.known ? (!v.listed ? "unlisted" : !v.here ? "not-here" : "yes") : "checking";
    }
    j["clipboard"] = a.window ? a.window->clipboard() : std::string{};
    j["admin_selected"] = a.admin_selected < static_cast<int>(a.systems.size()) ? a.systems[static_cast<std::size_t>(a.admin_selected)].name : std::string{};
    if (a.console) {
        // What a console shows, as text: its lines, trailing blanks dropped.
        Json rows = Json::array();
        std::lock_guard l(a.console->lock());
        auto& screen = a.console->screen();
        for (int r = 0; r < screen.rows(); ++r) {
            std::string line;
            for (int c = 0; c < screen.cols(); ++c) {
                const auto& cell = screen.at(r, c);
                if (cell.continuation) continue;
                append_utf8_(line, cell.ch);
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(line);
        }
        while (!rows.empty() && rows.back().get<std::string>().empty()) rows.erase(rows.size() - 1);
        j["console"] = {{"rows", rows}, {"alive", a.console->alive()}, {"size", Json::array({screen.rows(), screen.cols()})},
                        {"cell", Json::array({a.grid.cell_w, a.grid.cell_h})}, {"font_size", a.console_size(a.current)},
                        {"scroll", a.console_scroll}};
    }
    if (a.view == View::Run) {
        Json lines = Json::array();
        std::lock_guard l(const_cast<App&>(a).run.m);
        for (const auto& line : a.run.lines) lines.push_back(line.text);
        j["run"] = {{"lines", lines}, {"busy", a.run.busy.load()}};
    }
    if (a.display) {
        j["desktop"] = {{"texture", Json::array({a.texture.width, a.texture.height})},
                        {"asked", Json::array({a.desktop_size.width, a.desktop_size.height})}};
    }
    if (a.job) {
        std::lock_guard l(a.job->m);
        Json phases = Json::array();
        for (const auto& ph : a.job->phases) phases.push_back(ph.text + ": " + ph.state);
        j["job"] = {{"title", a.job->title}, {"done", a.job->done.load()}, {"code", a.job->code}, {"current", a.job->current},
                    {"phases", phases}, {"errors", a.job->errors}, {"stage", a.job->stage}, {"percent", a.job->percent}};
    }
    j["anchors"] = Json::object();
    for (const auto& [k, r] : a.anchors) j["anchors"][k] = Json::array({r[0], r[1], r[2], r[3]});
    j["clipped"] = Json::array();
    for (const auto& k : a.clipped) j["clipped"].push_back(k);
    return j;
}

std::string dump_(const App& a) { return state_(a).dump(); }

// A screenshot: .bmp (24-bit, for people) or PPM (for tests to read).
bool write_screenshot_(const host::Window& w, const fs::path& file) {
    int width = 0, height = 0;
    const auto px = w.read_pixels(width, height);
    if (px.empty()) return false;
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (file.extension() == ".bmp") {
        const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u, size = row * static_cast<std::uint32_t>(height);
        auto u16 = [&](std::uint16_t v) { out.put(static_cast<char>(v & 0xff)); out.put(static_cast<char>(v >> 8)); };
        auto u32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) out.put(static_cast<char>((v >> (8 * k)) & 0xff)); };
        out << "BM";
        u32(54 + size); u32(0); u32(54);
        u32(40); u32(static_cast<std::uint32_t>(width)); u32(static_cast<std::uint32_t>(height)); u16(1); u16(24);
        u32(0); u32(size); u32(2835); u32(2835); u32(0); u32(0);
        for (int y = height - 1; y >= 0; --y) {
            for (int x = 0; x < width; ++x) {
                const auto* p = px.data() + (std::size_t(y) * width + x) * 4;
                out.put(static_cast<char>(p[0])); out.put(static_cast<char>(p[1])); out.put(static_cast<char>(p[2]));
            }
            for (std::uint32_t k = static_cast<std::uint32_t>(width) * 3; k < row; ++k) out.put(0);
        }
    } else {
        out << std::format("P6\n{} {}\n255\n", width, height);
        for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            out << static_cast<char>(px[i + 2]) << static_cast<char>(px[i + 1]) << static_cast<char>(px[i]);
    }
    return static_cast<bool>(out);
}

// Tests: a script of input and checks (--script FILE), one line a frame,
// run after the frame is drawn. Input is injected as SDL delivers a person's.
//
//   wait <view> [seconds]           waitfor <path> <value> [seconds]
//   key <Key>[+<Key>...]            held in order, released in reverse
//   type <text>                     click <anchor> | click <x> <y>
//   move <x> <y>                    frames <n>        sleep <ms>
//   resize <w> <h>                  close             quit
//   screenshot <file>               state <file>
//   see [<n>s] <text>               until the text is shown (a console's lines,
//                                    a job, a notice; default 60 s)
//   commit <text>                   as the input method commits it
//   drop <file>                     dropped on the window
//   clipboard <text>                the host's clipboard
//   expect <path> <value>           (path: dotted, into the state; a string
//                                    compares as it is, anything else as JSON;
//                                    `present` / `absent` ask whether it is there)
struct Script {
    std::vector<std::string> lines;
    std::size_t at { 0 };
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::deque<std::function<void(App&)>> pending;      // one a frame
    std::string failure;
    bool finished() const { return at >= lines.size() && pending.empty(); }
};

std::optional<Script> read_script_(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    Script sc;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        sc.lines.push_back(line);
    }
    return sc;
}

std::optional<Json> path_(const Json& root, std::string_view path) {
    const Json* node = &root;
    while (!path.empty()) {
        const auto dot = path.find('.');
        const auto part = path.substr(0, dot);
        if (node->is_object()) {
            auto it = node->find(std::string(part));
            if (it == node->end()) return std::nullopt;
            node = &*it;
        } else if (node->is_array()) {
            std::size_t k = 0;
            if (std::from_chars(part.data(), part.data() + part.size(), k).ec != std::errc{} || k >= node->size()) return std::nullopt;
            node = &(*node)[k];
        } else {
            return std::nullopt;
        }
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    return std::optional<Json>{std::in_place, *node};
}

bool matches_(const std::optional<Json>& node, std::string_view want) {
    if (want == "present") return node.has_value();
    if (!node) return want == "absent";
    return node->is_string() ? node->get<std::string>() == want : node->dump() == want;
}

void script_step_(App& a, Script& sc) {
    if (!sc.pending.empty()) {
        auto act = std::move(sc.pending.front());
        sc.pending.pop_front();
        act(a);
        return;
    }
    if (sc.at >= sc.lines.size()) return;
    const auto& line = sc.lines[sc.at];
    std::istringstream words(line);
    std::string cmd;
    words >> cmd;
    auto fail = [&](std::string why) {
        sc.failure = std::format("line {} (`{}`): {}", sc.at + 1, line, why);
        a.quit = true;
    };
    auto next = [&] { ++sc.at; sc.deadline.reset(); };
    auto waiting = [&](double seconds) {
        if (!sc.deadline) sc.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
        return std::chrono::steady_clock::now() < *sc.deadline;
    };
    if (cmd.empty() || cmd.starts_with('#')) { next(); return; }
    if (cmd == "wait" || cmd == "waitfor") {
        std::string path = "view", want;
        if (cmd == "waitfor") words >> path;
        words >> want;
        double seconds = 30;
        words >> seconds;
        const auto got = path_(state_(a), path);
        if (matches_(got, want)) next();
        else if (!waiting(seconds)) fail(std::format("{} is {}, not {}", path, got ? got->dump() : "absent", want));
    } else if (cmd == "see") {
        std::string text;
        std::getline(words >> std::ws, text);
        double seconds = 60;
        if (const auto sp = text.find(' '); sp != std::string::npos && sp > 1 && text[sp - 1] == 's'
            && std::ranges::all_of(text.substr(0, sp - 1), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            seconds = std::stod(text.substr(0, sp - 1));
            text = text.substr(sp + 1);
        }
        const auto st = state_(a);
        std::string shown;
        if (st.contains("console"))
            for (const auto& row : st["console"]["rows"])
                if (row.is_string()) shown += row.get<std::string>() + "\n";
        if (st.contains("job")) shown += st["job"].dump();
        shown += st.value("notice", std::string{});
        if (shown.find(text) != std::string::npos) next();
        else if (!waiting(seconds)) fail(std::format("`{}` is not shown; shown:\n{}", text, shown));
    } else if (cmd == "expect") {
        std::string path, want;
        words >> path;
        std::getline(words >> std::ws, want);
        const auto got = path_(state_(a), path);
        if (!matches_(got, want)) fail(std::format("{} is {}, not {}", path, got ? got->dump() : "absent", want));
        else next();
    } else if (cmd == "key") {
        std::string spec;
        words >> spec;
        std::vector<std::string> keys;
        for (std::size_t from = 0; from <= spec.size();) {
            const auto plus = spec.find('+', from);
            auto k = spec.substr(from, plus == std::string::npos ? std::string::npos : plus - from);
            if (!k.empty()) {
                k[0] = static_cast<char>(k.size() > 1 ? std::toupper(static_cast<unsigned char>(k[0])) : k[0]);
                keys.push_back(k);
            }
            if (plus == std::string::npos) break;
            from = plus + 1;
        }
        for (const auto& k : keys)
            if (!a.window->inject_key(k, true)) { fail("no key " + k); return; }
        sc.pending.push_back([keys](App& app) {
            for (auto it = keys.rbegin(); it != keys.rend(); ++it) app.window->inject_key(*it, false);
        });
        next();
    } else if (cmd == "drop") {
        // A file dropped on the window, as a file manager drops it.
        std::string file;
        std::getline(words >> std::ws, file);
        sc.pending.push_back([file](App& app) { app.window->inject_drop(file); });
        next();
    } else if (cmd == "commit" || cmd == "clipboard") {
        // Text as the host's input method commits it; the host's clipboard.
        std::string text;
        std::getline(words >> std::ws, text);
        if (cmd == "commit") sc.pending.push_back([text](App& app) { app.window->inject_text(text); });
        else sc.pending.push_back([text](App& app) { app.window->set_clipboard(text); });
        next();
    } else if (cmd == "type") {
        std::string text;
        std::getline(words >> std::ws, text);
        for (char ch : text)
            sc.pending.push_back([ch](App& app) {
                const std::string one(1, ch);
                const bool upper = std::isupper(static_cast<unsigned char>(ch)) != 0;
                if (upper) app.window->inject_key("Shift", true);
                app.window->inject_key(ch == ' ' ? std::string("Space") : one, true);
                app.window->inject_text(one);
                app.window->inject_key(ch == ' ' ? std::string("Space") : one, false);
                if (upper) app.window->inject_key("Shift", false);
            });
        next();
    } else if (cmd == "click" || cmd == "move") {
        std::string first;
        words >> first;
        float x = 0, y = 0;
        if (first.empty()) { fail("where?"); return; }
        if (std::isdigit(static_cast<unsigned char>(first[0]))) {
            x = std::stof(first);
            words >> y;
        } else {
            // Clicked once it holds still -- laid out, not moving -- as a person would.
            auto it = a.anchors.find(first);
            auto before = a.anchors_before.find(first);
            if (it == a.anchors.end() || before == a.anchors_before.end() || before->second != it->second) {
                if (!waiting(10)) fail("no control " + first + " holding still on screen");
                return;
            }
            x = (it->second[0] + it->second[2]) / 2;
            y = (it->second[1] + it->second[3]) / 2;
        }
        sc.pending.push_back([x, y](App& app) { app.window->inject_pointer(x, y); });
        if (cmd == "click") {
            sc.pending.push_back([x, y](App& app) { app.window->inject_button(x, y, 1, true); });
            sc.pending.push_back([x, y](App& app) { app.window->inject_button(x, y, 1, false); });
        }
        next();
    } else if (cmd == "frames") {
        int n = 1;
        words >> n;
        for (int k = 0; k < n; ++k) sc.pending.push_back([](App&) {});
        next();
    } else if (cmd == "sleep") {
        double ms = 0;
        words >> ms;
        if (!waiting(ms / 1000.0)) next();
    } else if (cmd == "resize") {
        int w = 0, h = 0;
        words >> w >> h;
        a.window->set_size(w, h);
        next();
    } else if (cmd == "perf") {
        // `perf reset`: what follows is measured alone.
        a.perf = {};
        next();
    } else if (cmd == "tray") {
        // As if chosen at the status-area icon.
        std::string what;
        words >> what;
        a.tray_injected.push_back(what);
        next();
    } else if (cmd == "close") {
        a.window->inject_close();
        next();
    } else if (cmd == "quit") {
        a.quit = true;
        next();
    } else if (cmd == "screenshot") {
        std::string file;
        words >> file;
        if (!write_screenshot_(*a.window, file)) fail("cannot write " + file);
        else next();
    } else if (cmd == "state") {
        std::string file;
        words >> file;
        std::ofstream(file) << state_(a).dump(2) << "\n";
        next();
    } else {
        fail("not a script command");
    }
}

// ── the status-area icon (design 2026-10-16 L §13.12) ──────────────────

// The systems running now: a machine, or a sandbox's session.
std::vector<const service::Environment*> running_(const App& a) {
    std::vector<const service::Environment*> out;
    for (const auto& e : a.systems)
        if (e.running || const_cast<App&>(a).is_running(e.name)) out.push_back(&e);
    return out;
}

// The icon, made once it is wanted; where this desktop has none, why.
bool ensure_tray_(App& a) {
    if (a.tray) return true;
    if (!a.tray_error.empty()) return false;
    auto t = tray::Icon::create("luban-os", "Luban OS");
    if (!t) { a.tray_error = t.error(); return false; }
    a.tray = std::move(*t);
    auto* w = a.window.get();
    a.tray->on_wake([w] { w->wake(); });
    a.tray_said.clear();
    return true;
}

// What the icon shows: the systems that run, what is being done, a menu of both.
void tray_update_(App& a) {
    if (!a.tray) return;
    const auto on = running_(a);
    const bool busy = a.job && !a.job->done;
    const bool failed = a.job && a.job->done && a.job->code != 0;
    std::string body = on.empty() ? std::string(tr("Nothing running", "没有运行中的系统"))
                                  : [&] { const auto n = on.size(); return std::vformat(tr("{} running", "{} 个系统运行中"), std::make_format_args(n)); }();
    if (busy) {
        std::lock_guard l(a.job->m);
        body += " · " + a.job->title + (a.job->percent >= 0 ? std::format(" {:.0f} %", a.job->percent * 100) : std::string{});
    }
    tray::Status s{.title = "Luban OS", .body = body, .icons = art::tray_images(static_cast<int>(on.size()), busy, failed),
                   .attention = failed};
    for (const auto* e : on) {
        const bool machine = a.is_running(e->name);
        tray::MenuItem item{.id = "sys:" + e->name, .label = std::format("{}  ({})", e->name, machine ? tr("machine", "机器") : tr("sandbox", "沙箱"))};
        item.children.push_back({.id = "sys:" + e->name + ":open", .label = std::string(tr("Open", "打开"))});
        if (machine) {
            item.children.push_back({.id = "sys:" + e->name + ":console", .label = std::string(tr("Open its console", "打开控制台"))});
            item.children.push_back({.id = "sys:" + e->name + ":off", .label = std::string(tr("Power off", "关机"))});
        }
        item.children.push_back({.id = "sys:" + e->name + ":manage", .label = std::string(tr("Manage", "管理"))});
        s.menu.push_back(std::move(item));
    }
    if (!on.empty()) s.menu.push_back({.separator = true});
    s.menu.push_back({.id = "new", .label = std::string(tr("New system…", "新建系统…"))});
    s.menu.push_back({.id = "open", .label = std::string(tr("Open Luban OS", "打开 Luban OS"))});
    s.menu.push_back({.id = "settings", .label = std::string(tr("Settings", "设置"))});
    s.menu.push_back({.separator = true});
    s.menu.push_back({.id = "quit", .label = std::string(tr("Quit", "退出"))});
    // Told only what changed.
    std::string said = body + (failed ? "!" : "");
    for (const auto& m : s.menu) said += "|" + m.id;
    if (said == a.tray_said) return;
    a.tray_said = said;
    a.tray->update(s);
}

void show_(App& a) {
    a.hidden = false;
    a.window->show();
}

// The window goes, the app stays: in the tray where there is one, else
// minimized (design 2026-10-16 L §13.11-12).
void to_background_(App& a) {
    a.console.reset();
    a.close_display();
    a.view = View::Launcher;
    a.refresh();
    a.window->set_title("Luban OS");
    a.window->clear_close_request();
    if (ensure_tray_(a)) {
        a.hidden = true;
        a.window->hide();
        tray_update_(a);
        static bool told = false;
        if (!std::exchange(told, true))
            a.tray->notify("Luban OS", std::string(tr("Still here: your systems keep running. The tray icon opens it again.",
                                                      "仍在运行：系统继续运行，点状态栏图标可再次打开。")), "open");
    } else {
        a.window->minimize();
    }
}

// Leaving the app: with systems running, asked whether they go too.
void quit_(App& a) {
    const auto on = running_(a);
    if (on.empty()) { a.quit = true; return; }
    const auto count = on.size();
    show_(a);
    a.confirm = Confirm{std::string(tr("Quit Luban OS?", "退出 Luban OS？")),
                        std::vformat(tr("{} systems are running. They can keep running without the window, or be powered off.",
                                        "{} 个系统正在运行。可以让它们在没有窗口时继续运行，或全部关机。"), std::make_format_args(count)),
                        std::string(tr("Keep them running", "保持运行")), false, [&a] { a.quit = true; },
                        std::string(tr("Power off all", "全部关机")), [&a] {
                            for (const auto* e : running_(a))
                                if (vm::running(a.home, e->name)) (void)vm::stop(a.ctx, e->name);
                            a.quit = true;
                        }};
}

// What was chosen at the icon.
void tray_take_(App& a) {
    std::vector<std::string> chosen = std::exchange(a.tray_injected, {});
    if (a.tray) {
        a.tray->pump();
        for (auto& t : a.tray->take()) chosen.push_back(std::move(t));
    }
    for (const auto& what : chosen) {
        if (what == "activate" || what == "open" || what == "notification:open") {
            show_(a);
            const auto on = running_(a);
            if (what == "activate" && on.size() == 1 && a.view == View::Launcher) a.enter(on.front()->name);
        } else if (what == "new") { show_(a); a.view = View::QuickStart; }
        else if (what == "settings") { show_(a); a.before_admin = a.view; a.view = View::Admin; a.admin_tab = "settings"; }
        else if (what == "quit") quit_(a);
        else if (what.starts_with("notification:made:")) { show_(a); a.enter(what.substr(18)); }
        else if (what.starts_with("notification:")) show_(a);
        else if (what.starts_with("sys:")) {
            const auto rest = what.substr(4);
            const auto colon = rest.rfind(':');
            const auto name = rest.substr(0, colon), act = rest.substr(colon + 1);
            if (act == "open") { show_(a); a.enter(name); }
            else if (act == "console") { show_(a); a.current = name; a.open_console(name, true); }
            else if (act == "manage") {
                show_(a);
                a.before_admin = a.view;
                a.view = View::Admin;
                a.admin_tab = "systems";
                for (int i = 0; i < static_cast<int>(a.systems.size()); ++i)
                    if (a.systems[static_cast<std::size_t>(i)].name == name) a.admin_selected = i;
            } else if (act == "off") {
                auto* w = a.window.get();
                std::thread([ctx = a.ctx, name, w] { (void)vm::stop(ctx, name); w->wake(); }).detach();
            }
        }
        a.tray_said.clear();
    }
}

// A file dropped on a system's window (design 2026-10-16 L §13.9): copied into
// its login user's ~/Downloads -- the system's tree, which a sandbox shows and
// a machine shares (virtiofs) -- and said. Where it went, or why not.
std::string drop_into_(App& a, const fs::path& file) {
    const auto root = service::root_of(a.home, a.current);
    if (!root) return std::string(tr("this system has no tree of its own to put files in", "这个系统没有可以放文件的目录树"));
    const auto user = xlings::subos::root_etc::login_user(*root / "etc");
    const auto who = xlings::subos::root_etc::find_user(*root / "etc", user);
    const fs::path inside = who && !who->home.empty() ? fs::path(who->home) : fs::path("/root");
    const auto dir = *root / inside.relative_path() / "Downloads";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto target = dir / file.filename();
    for (int k = 2; fs::exists(target, ec) && k < 100; ++k)
        target = dir / std::format("{} ({}){}", file.stem().string(), k, file.extension().string());
    fs::copy(file, target, fs::copy_options::recursive, ec);
    if (ec) return std::format("{}: {}", file.filename().string(), ec.message());
    const auto shown = file.filename().string(), where = (inside / "Downloads").generic_string();
    return std::vformat(tr("{} is in {} (~/Downloads)", "{} 已放到 {}（~/Downloads）"), std::make_format_args(shown, where));
}

}  // namespace

int run(int argc, char* argv[]) {
    App a;
    host::Options o;
    int frames = -1;
    std::string screenshot;
    std::optional<Script> script;
    std::vector<std::string> desktop_exec;   // tests: a desktop from any program speaking opensal.wire
    std::vector<std::string> console_exec;   // tests: a console of any program, on a pseudo-terminal
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--attach" && i + 1 < argc) a.attach = argv[++i];
        else if (arg == "--headless") {
            o.hidden = true;
            platform::set_env_variable("SDL_VIDEODRIVER", "offscreen");
        }
        else if (arg == "--density" && i + 1 < argc) a.density_override = std::atof(argv[++i]);   // tests: a screen's density
        else if (arg == "--scale" && i + 1 < argc) o.scale = static_cast<float>(std::atof(argv[++i]));   // tests: a display's scale
        else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (arg == "--dump-state") a.dump_state = true;
        else if (arg == "--input" && i + 1 < argc) a.input = argv[++i];
        else if (arg == "--script" && i + 1 < argc) {
            script = read_script_(argv[++i]);
            if (!script) { std::println(std::cerr, "luban-os: cannot read the script {}", argv[i]); return 2; }
        }
        else if (arg == "--desktop-exec" && i + 1 < argc) { desktop_exec.assign(argv + i + 1, argv + argc); break; }
        else if (arg == "--console-exec" && i + 1 < argc) { console_exec.assign(argv + i + 1, argv + argc); break; }
        else if (arg == "--view" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (v == "admin") { a.before_admin = View::Launcher; a.view = View::Admin; }
            else if (v == "quick-start") a.view = View::QuickStart;
            else if (v == "settings") { a.before_admin = View::Launcher; a.view = View::Admin; a.admin_tab = "settings"; }
        }
        else if (arg == "--version") { std::println("luban-os {}", service::kVersion); return 0; }
        else if (arg == "--help" || arg == "-h") {
            std::println("luban-os [--attach <name>]   Luban OS: the systems of this home, in a window");
            return 0;
        }
    }
    set_language(a.settings.language);
    auto window = host::Window::open(o);
    if (!window) {
        std::println(std::cerr, "luban-os: {}", window.error());
        return 1;
    }
    a.window = std::move(*window);
    a.window->set_title("Luban OS");
    // The app's look: its theme on imkit, its background behind every page.
    imkit::set_theme(art::theme(a.window->ui_font(), a.window->title_font()));
    {
        auto& colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(imkit::theme().bg);
        colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(imkit::theme().accent_lo);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(imkit::theme().panel);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImGui::ColorConvertU32ToFloat4(imkit::theme().panel_hi);
        colors[ImGuiCol_ScrollbarGrabActive] = ImGui::ColorConvertU32ToFloat4(imkit::theme().accent_lo);
        auto& style = ImGui::GetStyle();
        style.ScrollbarSize = std::round(8 * a.window->ui_scale());
        style.ScrollbarRounding = 0;
    }
    imkit::on_anchor([&a](std::string_view name, ImVec2 mn, ImVec2 mx) { a.anchors[std::string(name)] = {mn.x, mn.y, mx.x, mx.y}; },
                     [&a](std::string_view name) { a.clipped.emplace(name); });
    if (auto refused = a.ctx.port.handshake()) a.notice = *refused;
    a.refresh();
    if (!console_exec.empty()) {
        std::string error;
        auto* win = a.window.get();
        a.current = "console";
        a.console = Console::process(console_exec, platform::environment(), a.current, [win] { win->wake(); }, error);
        if (!a.console) { std::println(std::cerr, "luban-os: {}", error); return 1; }
        a.view = View::Console;
        a.window->text_input(true);
    } else if (!desktop_exec.empty()) {
        std::string error;
        const auto [w, h] = a.window->size();
        a.desktop_size = a.device_size_(w, h);
        auto* win = a.window.get();
        a.current = "desktop";
        a.display = Display::process(desktop_exec, platform::environment(), a.home / "logs" / "luban-os.log", a.current,
                                     a.desktop_size, {.copy = true, .paste = true}, [win] { win->wake(); }, error);
        if (!a.display) { std::println(std::cerr, "luban-os: {}", error); return 1; }
        a.view = View::Desktop;
    } else if (!a.attach.empty()) a.enter(a.attach);
    else if (a.view == View::Admin || a.view == View::QuickStart) {}
    else if (a.systems.empty()) a.view = View::QuickStart;
    else if (a.settings.countdown > 0) a.countdown_until = std::chrono::steady_clock::now() + std::chrono::seconds(a.settings.countdown);
    const auto started = std::chrono::steady_clock::now();
    // Drawn while anything happens (design 2026-10-16 L §13.3): after any
    // input a few frames more, while something animates or runs at 60 fps;
    // only a still window waits long.
    int pending = 3;
    double wake = -1;   // when the last frame's animations next change
    for (int frame = 0; (frames < 0 || frame < frames) && !a.quit; ++frame) {
        if (script && script->finished()) break;
        if (script && std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
            script->failure = "the script did not finish in 10 minutes";
            break;
        }
        const bool busy = a.countdown_until || (a.job && !a.job->done) || a.resize_at || a.run.busy;
        // What the scheduler chooses (tests run their own beat, and are told this).
        // Still: until the next step of an animation, at most a second.
        int scheduled = (pending > 0 || busy || (wake >= 0 && wake < 0.016)) ? 16
                        : wake < 0 ? 1000 : std::clamp(static_cast<int>(wake * 1000), 16, 1000);
        if (a.notice_until)   // a notice over a system goes when its time is up
            scheduled = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       *a.notice_until - std::chrono::steady_clock::now()).count()) + 1, 16, scheduled);
        const int wait_ms = script ? 30 : frames >= 0 ? 0 : scheduled;
        if (a.perf.input_at && a.perf.frames_since_input == 1) a.perf.wait_after_input_ms = scheduled;
        if (pending == 0 && !busy) a.perf.idle_wait_ms = scheduled;
        if (!a.window->begin_frame(wait_ms) && !a.close_dialog) {
            // The window is to close: a running machine's on-close decides.
            // A sandbox's session ends with its window; only a machine has a choice.
            if ((a.view == View::Console && a.console && a.console->is_machine())
                || (a.view == View::Desktop && a.display && a.display->is_machine())) {
                auto env = service::describe(a.ctx, a.current);
                const auto on_close = env ? env->settings.on_close : std::string("ask");
                if (on_close == "ask") {
                    a.close_dialog = true;
                    a.close_temp = env && env->settings.persist == "none";
                    a.close_choice = a.settings.close_choice;
                    a.close_remember = false;
                } else if (on_close == "poweroff") {
                    (void)vm::stop(a.ctx, a.current);
                    break;
                } else {
                    to_background_(a);
                }
            } else if (a.settings.keep_in_tray || !running_(a).empty()) {
                // Something still runs, or the app is to stay: the tray.
                to_background_(a);
            } else {
                break;
            }
        }
        if (a.settings.keep_in_tray && !a.tray && a.tray_error.empty()) ensure_tray_(a);
        tray_take_(a);
        if (a.tray && std::chrono::steady_clock::now() - a.tray_checked > std::chrono::seconds(2)) {
            a.tray_checked = std::chrono::steady_clock::now();
            if (a.hidden) a.refresh();
            tray_update_(a);
        }
        // An operation that ended while nobody looked: said by the system.
        if (a.job && a.job->done && !std::exchange(a.job_told, true) && (a.hidden || !a.window->focused()) && ensure_tray_(a)) {
            std::lock_guard l(a.job->m);
            a.tray->notify(a.job->title, a.job->code == 0 ? std::string(tr("Done.", "完成。")) : (a.job->errors.empty() ? std::string(tr("Failed.", "失败。")) : a.job->errors.front()),
                           a.job->code == 0 && !a.job_subject.empty() ? "made:" + a.job_subject : "open");
        }
        const auto frame_start = std::chrono::steady_clock::now();
        const bool input = !a.window->events().empty() || !a.window->keys().empty();
        if (input) {
            pending = 3;
            a.perf.input_at = frame_start;
            a.perf.frames_since_input = 0;
        } else if (pending > 0) --pending;
        imkit::begin_frame(a.window->ui_scale(), ImGui::GetTime(), a.settings.reduce_motion);
        // Any input cancels the countdown.
        if (a.countdown_until) {
            const bool input = std::ranges::any_of(a.window->events(), [](const auto& e) {
                return std::holds_alternative<opensal::Key>(e) || std::holds_alternative<opensal::PointerButton>(e)
                    || std::holds_alternative<opensal::PointerMotion>(e);
            });
            if (input) a.countdown_until.reset();
            else if (std::chrono::steady_clock::now() >= *a.countdown_until && !a.systems.empty()) a.enter(a.countdown_target());
        }
        if (a.window->host_key() && (a.view == View::Console || a.view == View::Off || a.view == View::Desktop || a.view == View::Run))
            a.menu = !a.menu, a.menu_focus = 0;
        const bool overlay = a.menu || a.close_dialog || a.confirm;
        const bool typing = ImGui::GetIO().WantTextInput;
        for (const auto& k : a.window->keys())
            if (k.name == "F2" && !overlay && a.view != View::Console && a.view != View::Desktop && a.view != View::Run) {
                a.before_admin = a.view;
                a.view = View::Admin;
                if (a.selected < static_cast<int>(a.systems.size())) a.admin_selected = a.selected;
            }
        if (a.view == View::Launcher && !overlay) {
            const auto shown = shown_(a);
            const int rows = static_cast<int>(shown.size()) + 1;
            for (const auto& k : a.window->keys()) {
                const bool ctrl = k.key.mods & opensal::Ctrl;
                if (k.name == "Down") { a.selected = (a.selected + 1) % rows; a.keyboard = a.follow = true; }
                else if (k.name == "Up") { a.selected = (a.selected + rows - 1) % rows; a.keyboard = a.follow = true; }
                else if (k.name == "Enter") {
                    if (a.selected >= static_cast<int>(shown.size())) a.view = View::QuickStart;
                    else a.enter(a.systems[static_cast<std::size_t>(shown[static_cast<std::size_t>(a.selected)])].name);
                } else if (k.name == "Delete" && a.selected < static_cast<int>(shown.size())) {
                    const auto name = a.systems[static_cast<std::size_t>(shown[static_cast<std::size_t>(a.selected)])].name;
                    a.confirm = Confirm{std::vformat(tr("Remove {}?", "删除 {}？"), std::make_format_args(name)),
                                        std::string(tr("Its home and every file in it are deleted. This cannot be undone.",
                                                       "它的 home 以及其中所有文件都会被删除，无法撤销。")),
                                        std::string(tr("Remove", "删除")), true, [&a, name] {
                                            a.start_job(std::format("{} {}", tr("Removing", "删除"), name),
                                                        [name](Job& j, service::Context c) { j.code = service::remove(c, name).code; },
                                                        [](App& app) { app.refresh(); app.view = View::Launcher; });
                                        }};
                } else if (k.name == "Escape") { a.filter.clear(); a.selected = 0; }
                else if (k.name == "Backspace" && !a.filter.empty()) {
                    a.filter.pop_back();
                    while (!a.filter.empty() && (static_cast<unsigned char>(a.filter.back()) & 0xC0) == 0x80) a.filter.pop_back();
                    if ((static_cast<unsigned char>(a.filter.empty() ? 0 : a.filter.back()) & 0x80) && !a.filter.empty()) a.filter.pop_back();
                    a.selected = 0;
                } else if (ctrl && (k.name == "o" || k.name == "O"))
                    a.window->pick_file(std::string(tr("Import a Luban system", "导入 Luban 系统")), "Luban", "luban");
            }
            // Typed on the home: the names shown.
            for (const auto& e : a.window->events())
                if (const auto* t = std::get_if<opensal::TextCommit>(&e); t && !t->text.empty() && t->text != " ") {
                    a.filter += t->text;
                    a.selected = 0;
                    a.countdown_until.reset();
                }
        }
        if (a.view == View::QuickStart && !overlay && !typing) {
            for (const auto& k : a.window->keys()) {
                const auto& cur = kEditions[static_cast<std::size_t>(a.new_edition)];
                auto in_group = [&](bool desktop) {
                    std::vector<int> v;
                    for (std::size_t i = 0; i < kEditions.size(); ++i) if (kEditions[i].desktop == desktop) v.push_back(static_cast<int>(i));
                    return v;
                };
                const auto group = in_group(cur.desktop);
                const auto at = std::ranges::find(group, a.new_edition) - group.begin();
                if (k.name == "Right" || k.name == "Left") {
                    const auto n = static_cast<long>(group.size());
                    a.new_edition = group[static_cast<std::size_t>((at + (k.name == "Right" ? 1 : n - 1)) % n)];
                    a.keyboard = true;
                } else if (k.name == "Down" || k.name == "Up") {
                    const auto other = in_group(!cur.desktop);
                    a.new_edition = other[static_cast<std::size_t>(std::min<long>(at, static_cast<long>(other.size()) - 1))];
                    a.keyboard = true;
                } else if (k.name == "Enter" && a.quick_can_make && a.new_name[0]) make_(a);
                else if (k.name == "Escape" && !a.systems.empty()) a.view = View::Launcher;
            }
        }
        if (a.view == View::Admin && !overlay && !typing)
            for (const auto& k : a.window->keys())
                if (k.name == "Escape") { a.view = a.display ? View::Desktop : a.console ? View::Console : View::Launcher; a.refresh(); }
        if (a.view == View::Job && a.job && !a.job->done && !overlay)
            for (const auto& k : a.window->keys())
                if (k.name == "Escape" && a.job->cancel) a.job->cancel->store(true);
        // A pointer that moved: its focus is drawn no more until the keys move it.
        for (const auto& e : a.window->events())
            if (std::holds_alternative<opensal::PointerButton>(e)) a.keyboard = false;
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
        // A .luban file -- picked in the dialog, or dropped on the window --
        // opens its preview (not over a system in use); any other file
        // dropped on a system goes into it.
        {
            std::optional<fs::path> file = a.window->picked();
            if (const auto why = a.window->pick_error(); !why.empty())
                a.notice = std::vformat(tr("No file dialog here ({}): drop the .luban file on the window instead.",
                                           "这里没有文件对话框（{}）：请把 .luban 文件拖进窗口。"), std::make_format_args(why));
            if (file && file->empty()) file.reset();   // the dialog was cancelled
            const bool in_system = a.view == View::Console || a.view == View::Desktop || a.view == View::Run;
            for (const auto& d : a.window->dropped()) {
                if (d.extension() == ".luban" && !in_system) file = d;
                else if (in_system) {
                    a.notice = drop_into_(a, d);
                    a.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
            }
            const bool free = a.view == View::Launcher || a.view == View::QuickStart || a.view == View::Admin || a.view == View::Import;
            if (file && free) {
                std::vector<std::string> taken;
                for (const auto& sys : a.systems) taken.push_back(sys.name);
                a.import = preview_import_(*file, taken);
                a.countdown_until.reset();
                a.view = View::Import;
            }
        }
        if (a.view == View::Job && a.job && a.job->done && a.job->code == 0 && a.after_job) {
            auto then = std::move(a.after_job);
            a.after_job = nullptr;
            then(a);
        }
        a.anchors_before = std::move(a.anchors);
        a.anchors.clear();
        a.clipped.clear();
        switch (a.view) {
        case View::Launcher: launcher_(a); break;
        case View::QuickStart: quick_start_(a); break;
        case View::Job: job_(a); break;
        case View::Console: console_view_(a); break;
        case View::Off: off_(a); break;
        case View::Admin: admin_(a); break;
        case View::Desktop: desktop_view_(a); break;
        case View::Import: import_view_(a); break;
        case View::Run: run_view_(a); break;
        }
        // A notice over a system: a line at the bottom, for a moment.
        if (a.notice_until && (a.view == View::Console || a.view == View::Desktop || a.view == View::Run)) {
            if (std::chrono::steady_clock::now() < *a.notice_until) {
                auto* dl = ImGui::GetForegroundDrawList();
                const auto* vp = ImGui::GetMainViewport();
                const auto ts = imkit::text_size(a.notice, 14);
                const ImVec2 p(std::round(vp->Pos.x + (vp->Size.x - ts.x) / 2 - px(12)), std::round(vp->Pos.y + vp->Size.y - ts.y - px(28)));
                imkit::panel(dl, p, ImVec2(p.x + ts.x + px(24), p.y + ts.y + px(12)), imkit::Bevel::Raised, imkit::theme().panel);
                imkit::text(dl, ImVec2(p.x + px(12), p.y + px(6)), a.notice, imkit::theme().text, 14);
            } else {
                a.notice_until.reset();
                a.notice.clear();
            }
        }
        if (a.menu) host_menu_(a);
        if (a.close_dialog) close_dialog_(a);
        if (a.confirm) confirm_(a);
        wake = imkit::wake_in();
        const auto drawn = std::chrono::steady_clock::now();
        a.window->end_frame();
        {
            // This frame's time; and, the frame after an input, how long until
            // what the input did is on the screen (the input is taken in one
            // frame and shown by the next).
            const auto end = std::chrono::steady_clock::now();
            a.perf.frame_ms.push_back(std::chrono::duration<double, std::milli>(end - frame_start).count());
            a.perf.present_ms.push_back(std::chrono::duration<double, std::milli>(end - drawn).count());
            if (a.perf.frame_ms.size() > 600) { a.perf.frame_ms.pop_front(); a.perf.present_ms.pop_front(); }
            ++a.perf.frames;
            if (a.perf.input_at && ++a.perf.frames_since_input == 2) {
                a.perf.input_to_next_frame_ms = std::chrono::duration<double, std::milli>(end - *a.perf.input_at).count();
                a.perf.input_to_next_frame_max = std::max(a.perf.input_to_next_frame_max, a.perf.input_to_next_frame_ms);
            }
        }
        if (auto later = std::exchange(a.later, nullptr)) later();
        if (std::exchange(a.refresh_after_frame, false)) a.refresh();
        if (script) script_step_(a, *script);
    }
    if (a.dump_state) std::println("{}", dump_(a));
    if (!screenshot.empty()) write_screenshot_(*a.window, screenshot);
    if (script && !script->failure.empty()) {
        std::println(std::cerr, "luban-os script: {}", script->failure);
        return 3;
    }
    return 0;
}

}  // namespace luban::os
