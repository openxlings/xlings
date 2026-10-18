// What the Luban OS app's files share (luban.os): the window's state (App),
// the views' types, and what one file calls in another. Nothing here is
// exported: the module's one export is run() (os.cppm).
export module luban.os:core;

import std;
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

namespace luban::os {

namespace fs = std::filesystem;
namespace term = xdesk::term;
namespace service = luban::service;
namespace vm = luban::vm;
namespace platform = xlings::platform;
namespace host = luban::desktop::host;
using Json = nlohmann::json;

// The words of the interface, English or Chinese: the app's language
// setting, else LUBAN_OS_LANG and the locale.
std::string_view tr(std::string_view en, std::string_view zh);
// "auto", "en" or "zh".
void set_language(std::string_view lang);

// What the app keeps of its own (<home>/config/luban-os.json): the default
// system, the countdown, when each was last used, how it looks and where it
// goes when closed. An environment's own settings (mode, on-close) are the
// environment's (machine.json).
struct AppSettings {
    std::string default_system;
    int countdown { 10 };
    std::map<std::string, std::string> last_used;   // name -> ISO time
    std::string language { "auto" };                // auto | en | zh
    bool reduce_motion { false };
    float console_size { 16 };                      // the console's default font size (nominal)
    std::map<std::string, float> console_sizes;     // per system, Ctrl+= / Ctrl+- / Ctrl+0
    bool minimize_to_tray { true };
    bool keep_in_tray { false };                    // the tray icon even with nothing running
    int close_choice { 0 };                         // the close dialog's last choice: 0 background, 1 power off
};
AppSettings read_app_settings(const fs::path& home);
void write_app_settings(const fs::path& home, const AppSettings& s);

// An operation running beside the window (creating, starting): its events,
// as phases a person reads.
struct Job {
    struct Phase { std::string name, text, state; };
    std::string title;
    std::mutex m;
    std::vector<Phase> phases;
    std::vector<std::string> log;
    std::string current;
    double percent { -1 };
    std::atomic<bool> done { false };
    std::string stage;              // the last progress phase (download, extract, configure, check)
    std::shared_ptr<std::atomic<bool>> cancel;   // set: what runs is stopped
    std::function<void()> retry;    // the same operation again
    int code { 0 };
    nlohmann::json value;
    std::vector<std::string> errors;
    std::thread worker;
    void take(const service::Event& e);
    ~Job() { if (worker.joinable()) worker.join(); }
};

// A console: a machine's session or a sandbox's shell on a local pty, and
// the screen it paints.
class Console {
public:
    // `greeting` is on the screen before anything the session writes (the
    // environment's welcome).
    static std::unique_ptr<Console> machine(const fs::path& home, const std::string& name, std::function<void()> wake,
                                            std::string& error, const std::string& greeting = {});
    static std::unique_ptr<Console> sandbox(const fs::path& xlings, const fs::path& home, const std::string& name,
                                            const std::string& persist, std::function<void()> wake, std::string& error,
                                            const std::string& greeting = {});
    // Any program on a pseudo-terminal (what a sandbox's shell is; tests run
    // a program of their own the same way).
    static std::unique_ptr<Console> process(const std::vector<std::string>& argv, std::map<std::string, std::string> env,
                                            const std::string& name, std::function<void()> wake, std::string& error,
                                            const std::string& greeting = {});
    ~Console();
    void write(std::string_view bytes);
    void resize(int rows, int cols);
    bool alive() const { return alive_; }
    int exit_code() const { return code_; }
    bool is_machine() const { return machine_; }
    std::mutex& lock() { return m_; }
    term::Screen& screen() { return screen_; }
    const std::string& name() const { return name_; }

private:
    Console() : screen_(24, 80) {}
    // A session on a channel of `client` (a machine's supervisor, a PC's host
    // machine agent), opened by `open` at the console's size.
    static std::unique_ptr<Console> over_(vm::Client client, const std::string& name, bool machine, std::function<void()> wake,
                                          std::string& error, const std::string& greeting,
                                          const std::function<bool(vm::Client&, int, int)>& open);
    std::string name_;
    bool machine_ { false };
    std::mutex m_;
    term::Screen screen_;
    std::thread io_;
    std::atomic<bool> alive_ { true };
    std::atomic<bool> stop_ { false };
    int code_ { 0 };
    std::optional<vm::Client> client_;
    int master_ { -1 };
    int pid_ { -1 };
    std::function<void()> wake_;
    int rows_ { 24 }, cols_ { 80 };
};

// A machine's desktop on this window: opensal.wire on one of its channels
// (the guest bridges its compositor, opensal-wlr). This side is the
// provider: it says the size it shows the surface at and sends the input;
// the pixels come back as damaged rectangles, applied on the window's thread.
// What may cross the clipboard between the window and a desktop: the
// environment's grants (`clipboard` out of it, `clipboard-paste` into it).
struct ClipboardGrant {
    bool copy { false };
    bool paste { false };
};

class Display {
public:
    static std::unique_ptr<Display> machine(const fs::path& home, const std::string& name, const std::string& session,
                                            opensal::Size size, ClipboardGrant clip, std::function<void()> wake, std::string& error);
    // In a sandbox: `xlings subos use <name> --cmd 'luban __desktop <session>'`,
    // opensal.wire on its standard input and output.
    static std::unique_ptr<Display> sandbox(const fs::path& xlings, const fs::path& luban, const fs::path& home,
                                            const std::string& name, const std::string& session, const std::string& persist,
                                            opensal::Size size, ClipboardGrant clip, std::function<void()> wake, std::string& error);
    // Any program whose standard input and output are opensal.wire (what a
    // sandbox runs; tests run a desktop of their own the same way).
    static std::unique_ptr<Display> process(const std::vector<std::string>& argv, std::map<std::string, std::string> env,
                                            const fs::path& log, const std::string& name, opensal::Size size,
                                            ClipboardGrant clip, std::function<void()> wake, std::string& error);
    ~Display();
    void send(const opensal::Event& e);
    bool alive() const { return alive_; }
    // Why it ended, when it could not start.
    std::string error() const { std::lock_guard l(m_); return error_; }
    const std::string& name() const { return name_; }
    bool is_machine() const { return machine_; }

    // The desktop's caret for the input method ("1 x y w h" in its pixels,
    // or "0"), and its selection for the window's clipboard (granted copy
    // only): what it said since the last call.
    std::optional<std::string> take_text_input();
    std::optional<std::string> take_clipboard();
    const ClipboardGrant& clipboard_grant() const { return clip_; }

    struct Update { opensal::Rect rect; std::string rows; };
    // The surface's size (0 until the first frame) and the rectangles since the last call.
    opensal::Size take(std::vector<Update>& out);

private:
    Display() = default;
    // A desktop on a channel of `client` (a machine's supervisor, a PC's host
    // machine agent), opened by `open`.
    static std::unique_ptr<Display> over_(vm::Client client, const std::string& name, bool machine, opensal::Size size,
                                          ClipboardGrant clip, std::function<void()> wake, std::string& error,
                                          const std::function<bool(vm::Client&)>& open, const fs::path& log);
    bool machine_ { false };
    fs::path log_;                    // a relayed desktop's standard error
    void provider_(std::string_view bytes);
    std::string name_;
    mutable std::mutex m_;
    bool send_(std::string_view wire_bytes);
    std::optional<vm::Client> client_;
    int to_ { -1 }, from_ { -1 }, pid_ { -1 };   // a sandbox's process
    std::mutex sending_;
    std::thread io_;
    std::atomic<bool> alive_ { true };
    std::atomic<bool> stop_ { false };
    std::function<void()> wake_;
    opensal::Size wanted_;
    double density_ { 1 };
    opensal::Size size_;
    ClipboardGrant clip_;
    std::optional<std::string> text_input_, clipboard_;
    std::vector<Update> updates_;
    std::string error_;
    opensal::wire::Decoder decoder_;
};

// The desktop session `name` shows when it is entered: its setting
// (`luban config <n> --desktop`), else the first its root declares; nothing
// when it declares none or the setting is `none`.
// The desktop session a system starts: its setting (`none`: no desktop), else
// the first its root declares (Environment::desktops).
std::optional<std::string> desktop_session(const service::Environment& e);

// The app's art and colours, drawn with imkit (art.cpp).
namespace art {
enum class Badge { Nano, Tiny, TinyMusl, Core, Agent, Desktop, DesktopXdesk, Unknown, Craftsman };
imkit::Theme theme(void* ui_font, void* title_font);
// An edition's colour: -1 dark, 0 its own, 1 light.
ImU32 edition_color(std::string_view edition, int shade = 0);
Badge badge_of(std::string_view edition);
// A badge (16 x 16 art pixels), `cell` window pixels each; `frame` animates it.
void badge(ImDrawList* dl, Badge b, std::string_view edition, ImVec2 at, float cell, int frame = 0);
imkit::Picture picture(std::string_view edition, int frame = 0);
// The status-area icon at its sizes: `running` systems, at work, or needing the person.
std::vector<tray::Image> tray_images(int running, bool busy, bool error);
}  // namespace art

// How a console is drawn (design 2026-10-16 L §13.7): one effective font size
// for measuring and drawing, an integer cell grid, the console's own colours.
struct ConsoleLook {
    void* font { nullptr };         // regular
    void* bold { nullptr };         // a real bold weight (the regular when there is none)
    float size { 16 };              // nominal pixels; the window's scale applies
    float padding { 3 };            // around the grid, in the window's pixels
    bool focused { true };
    int scroll { 0 };               // lines scrolled back into history (0: the screen)
    // A selection, in cells of what is shown: from (row, col) to (row, col).
    std::optional<std::array<int, 4>> selection;
};
// What was drawn: the grid it fits, and where its cells are (for the pointer).
struct ConsoleGrid {
    int rows { 0 }, cols { 0 };
    float x { 0 }, y { 0 }, cell_w { 1 }, cell_h { 1 };
};
// Draws `c` filling the current imgui window.
ConsoleGrid draw_console(Console& c, const ConsoleLook& look);
// The text of a selection (cells, inclusive), lines joined with \n.
std::string console_text(Console& c, int scroll, std::array<int, 4> selection);

// ── the window's state ──────────────────────────────────────────────────


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
        int presented { 0 };                          // of which drawn and shown (the rest changed nothing)
        std::string renderer;                         // SDL's: "software" is no GPU
        std::optional<std::chrono::steady_clock::time_point> input_at;
        int frames_since_input { 0 };
    } perf;
    bool job_told { true };            // the last job's end was said (a notification when unseen)
    std::string job_subject;           // the system the last job made (its notification goes in)
    std::optional<std::chrono::steady_clock::time_point> notice_until;   // a notice over a system, until
    bool desktop_changed { true };     // this frame's desktop picture differs from the last one shown
    // The desktop's pacing: when it was last told a frame was shown, and when
    // it is to be told next (software rendering: at most 30 a second).
    std::chrono::steady_clock::time_point presented_at;
    std::optional<std::chrono::steady_clock::time_point> present_due;
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
        // A desktop set and not there is said over the console (E20).
        if (env) if (const auto plan = service::enter_plan(*env); !plan.why.empty()) notice = plan.why;
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
            if (env) if (const auto plan = service::enter_plan(*env); !plan.why.empty()) notice = plan.why;
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
        // Why it is the console and not the desktop, said over it for a while
        // (E7: the reason was cleared as the console opened).
        if (!notice.empty()) notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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

using imkit::px;

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

// ── what one file calls in another ──────────────────────────────────────

void begin_full_(const char* id);
void mark_(App& a, std::string name);
const imkit::Theme& th_();
void centred_(ImDrawList* dl, float y, std::string_view s, ImU32 color, float size = 0, bool title = false);
float paragraph_(ImDrawList* dl, ImVec2 at, std::string_view s, float width, ImU32 color, float size = 0);
bool page_bar_(App& a, std::string_view title, bool back, std::string_view back_id = "back");
std::string ago_(const std::string& iso);
std::string edition_label_(const service::Environment& e);
std::vector<int> shown_(const App& a);
void chip_(ImDrawList* dl, ImVec2 at, std::string_view s, ImU32 fg, ImU32 bg);
void launcher_(App& a);
void ask_availability_(App& a);
bool can_run_(App& a);
std::string cannot_run_(App& a);
std::string unavailable_(App& a, const Edition& e);
void make_(App& a);
float new_panel_(App& a, float x, float y, float w, float min_h);
void quick_start_(App& a);
void import_view_(App& a);
int stage_of_(const Job& j);
void job_(App& a);
void off_(App& a);
ImVec2 overlay_(ImDrawList* dl, ImVec2 size);
void begin_overlay_(const char* id);
void close_dialog_(App& a);
void confirm_(App& a);
void host_menu_(App& a);
float system_bar_(App& a, std::string_view mode);
void console_view_(App& a);
void run_view_(App& a);
ImportPreview preview_import_(const fs::path& file, const std::vector<std::string>& taken);
std::string type_desktop_(Display& d, std::string_view input);
void desktop_view_(App& a);
void system_pages_(App& a, ImVec2 o, float x0, float y0, float width, float height);
void check_system_(App& a, const std::string& name);
void app_settings_(App& a, ImVec2 o, float x0, float y0, float width);
void activity_(App& a, ImVec2 o, float x0, float y0, float width);
void admin_(App& a);
Json state_(const App& a);
std::string dump_(const App& a);
bool write_screenshot_(const host::Window& w, const fs::path& file);
std::optional<Script> read_script_(const fs::path& file);
std::optional<Json> path_(const Json& root, std::string_view path);
bool matches_(const std::optional<Json>& node, std::string_view want);
void script_step_(App& a, Script& sc);
std::vector<const service::Environment*> running_(const App& a);
bool ensure_tray_(App& a);
void tray_update_(App& a);
void show_(App& a);
void to_background_(App& a);
void quit_(App& a);
void tray_take_(App& a);
std::string drop_into_(App& a, const fs::path& file);

}  // namespace luban::os
