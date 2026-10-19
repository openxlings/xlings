// What the Luban OS app's files share (luban.os): the window's state (App),
// the views' types, and what one file calls in another. Nothing here is
// exported: the module's one export is run() (os.cppm).
export module luban.os:core;

import std;
import xlings.libs.json;
import xlings.i18n;
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

// The words of the interface, by key (apps/luban-os/src/i18n, the domain
// `luban.os`), through the shared mechanism (xlings.i18n): the app's
// language setting, else LUBAN_OS_LANG and the system's.
inline std::string_view tr(std::string_view key) { return xlings::i18n::tr(key); }
// "auto", "en" or "zh".
void set_language(std::string_view lang);
// The app's words and glossary, registered first thing in run().
void register_words();
std::span<const xlings::i18n::Entry> words_en();
std::span<const xlings::i18n::Entry> words_zh();

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
    bool lock_keyboard { true };                    // a desktop entered holds the keyboard (Ctrl+Alt+K lets it go)
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
    double overall { -1 };          // the whole operation's progress, when it has one (never back)
    std::atomic<bool> done { false };
    std::string stage;              // the stage it is in (prepare .. check), or the last progress phase
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
                                            ClipboardGrant clip, std::function<void()> wake, std::string& error,
                                            const std::string& session = {});
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
    // A desktop without a shell of its own asks the window to be it
    // (opensal Shell): its applications to start and its windows to bring forward.
    bool shell() const { return shell_; }
    std::vector<opensal::Application> applications() const { std::lock_guard l(m_); return applications_; }
    std::vector<opensal::Window> windows() const { std::lock_guard l(m_); return windows_; }

    // The pointer over its picture (design 2026-10-19 §15): what it says
    // ('c' a shape, 'C' an image) when it asked for Cursor; else the host's
    // arrow for an xdesk that cannot draw one (luban.compat), or none -- it
    // draws its own into the frames.
    struct Pointer {
        std::string shape { "none" };                 // kCursors, or "custom" with `image`
        std::optional<opensal::CursorImage> image;
        bool host_arrow { false };
    };
    Pointer pointer() const { std::lock_guard l(m_); return pointer_; }
    // It asked to hold the pointer (opensal PointerLock: a game) or let it
    // go: what it said since the last call.
    std::optional<bool> take_pointer_lock();

    struct Update { opensal::Rect rect; std::string rows; };
    // The surface's size (0 until the first frame) and the rectangles since the last call.
    opensal::Size take(std::vector<Update>& out);

private:
    Display() = default;
    // A desktop on a channel of `client` (a machine's supervisor, a PC's host
    // machine agent), opened by `open`.
    static std::unique_ptr<Display> over_(vm::Client client, const std::string& name, bool machine, opensal::Size size,
                                          ClipboardGrant clip, std::function<void()> wake, std::string& error,
                                          const std::function<bool(vm::Client&)>& open, const fs::path& log,
                                          const std::string& session);
    bool machine_ { false };
    fs::path log_;                    // a relayed desktop's standard error
    void provider_(std::string_view bytes);
    std::string name_;
    std::string session_;             // the desktop session it started (empty: unknown)
    Pointer pointer_;
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
    std::optional<bool> pointer_lock_;
    std::atomic<bool> shell_ { false };
    std::vector<opensal::Application> applications_;
    std::vector<opensal::Window> windows_;
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
std::array<int, 4> console_word(Console& c, int scroll, int row, int col);

// ── the window's state ──────────────────────────────────────────────────


fs::path home_dir_();

// ── the tray process (design 2026-10-19 §3) ──────────────────────────────
// One per home, `luban-os __tray`: the status-area icon, its menu, the
// notifications; every window its own process, speaking to it over
// <home>/run/luban-os/tray.sock in lines of JSON. tray.lock makes it one;
// each window holds windows/<pid>.lock while it lives (never a pid alone).
inline constexpr int kTrayProtocol = 1;
fs::path tray_dir(const fs::path& home);
int run_tray(int argc, char* argv[]);

// A window's line to the tray (tray/link.cpp): connects -- starting the tray
// when nobody holds tray.lock -- says who it is, keeps what it last said of
// itself (register, job) to say again after a reconnect, and hands what the
// tray asks to the window, waking it.
class TrayLink {
public:
    TrayLink(fs::path home, std::function<void()> wake);
    ~TrayLink();
    TrayLink(const TrayLink&) = delete;
    TrayLink& operator=(const TrayLink&) = delete;
    // A message; `register` and `job` are kept and said again after a reconnect.
    void send(const nlohmann::json& message);
    // What the tray said since the last call.
    std::vector<nlohmann::json> take();
    // The tray's own word on itself (its pid, its icon's backend, why it has
    // no icon), once connected; waits up to `wait` for it.
    std::optional<nlohmann::json> status(std::chrono::milliseconds wait = std::chrono::milliseconds(0));
    // Why there is no tray: it kept ending (with where its log is).
    std::string trouble() const;

private:
    struct State;
    std::shared_ptr<State> s_;
    std::jthread io_;
};

enum class View { Launcher, QuickStart, Job, Console, Off, Admin, Desktop, Import, Run };

// The editions one can make, in two groups. What each is ships with Luban;
// whether it can be had here (published, for this machine) the index answers.
struct Edition {
    std::string_view name;                  // `luban new --edition`
    bool desktop;                           // the desktop group
    // Its words, by key (apps/luban-os/src/i18n): its title, one line about
    // it, and what is in it and for whom.
    std::string_view title, about, detail;
    std::string_view size;                  // about what it downloads
    std::string_view contents;              // its first few programs
    bool goes_in;                           // there is something to log into
    // Made as another edition with its desktop chosen (design 2026-10-18
    // §6.1: desktop-xdesk is frozen, the desktop edition on xdesk's canvas).
    std::string_view makes {};
    std::string_view session {};
    std::string_view edition() const { return makes.empty() ? name : makes; }
};

constexpr std::array<Edition, 7> kEditions{{
    {"nano", false, "luban.os.edition.nano.title", "luban.os.edition.nano.about", "luban.os.edition.nano.detail", "~15 MB", "xlings · luban", false},
    {"tiny", false, "luban.os.edition.tiny.title", "luban.os.edition.tiny.about", "luban.os.edition.tiny.detail", "~25 MB", "busybox · xlings · luban", true},
    {"tiny-musl", false, "luban.os.edition.tiny_musl.title", "luban.os.edition.tiny_musl.about", "luban.os.edition.tiny_musl.detail", "~25 MB", "busybox · musl · xlings", true},
    {"core", false, "luban.os.edition.core.title", "luban.os.edition.core.about", "luban.os.edition.core.detail", "~400 MB", "fish · gcc · git · mcpp", true},
    {"agent-workspace", false, "luban.os.edition.agent_workspace.title", "luban.os.edition.agent_workspace.about", "luban.os.edition.agent_workspace.detail", "~700 MB", "core · node · python · agents", true},
    {"desktop", true, "luban.os.edition.desktop.title", "luban.os.edition.desktop.about", "luban.os.edition.desktop.detail", "~1.2 GB", "xdesk · foot · nvim · node", true},
    {"desktop-xdesk", true, "luban.os.edition.desktop_xdesk.title", "luban.os.edition.desktop_xdesk.about", "luban.os.edition.desktop_xdesk.detail", "~1.2 GB", "xdesk · foot · nvim · node", true, "desktop", "xdesk-canvas"},
}};

int edition_index_(std::string_view name);

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
void append_utf8_(std::string& out, char32_t ch);

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
    bool shell_apps { false };         // the shell's list of applications is open
    std::uint32_t desktop_buttons { 0 };   // pointer buttons pressed on the desktop (1 << Button)
    double sent_density { 1 };         // the density the desktop was told
    std::string ended_why;             // a failed session's last lines (the off screen shows them)
    int ended_code { 0 };
    bool paste_on_open { true };       // the host's clipboard, given once it shows
    std::string pasted;                // what crossed last (not sent back)
    std::optional<std::chrono::steady_clock::time_point> resize_at;
    std::string current;               // the system on screen
    bool menu { false };               // the Host key's menu
    // The keyboard and the pointer held for a desktop (design 2026-10-19
    // §16.1): wanted from the setting as it is entered, Ctrl+Alt+K flips it;
    // held only while the window has the focus.
    bool keyboard_wanted { false };
    host::Window::Grab keyboard_shown { host::Window::Grab::Released };   // what the title and the toast said
    bool keyboard_told { false };          // Ctrl+Alt+K or the menu asked: its answer is said
    std::string toast_text;                // a moment's word at the top
    std::optional<std::chrono::steady_clock::time_point> toast_until;
    void toast(std::string text) {
        toast_text = std::move(text);
        toast_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    }
    bool was_in_desktop { false };
    bool focus_out { false };
    bool pointer_lock_asked { false };
    int menu_focus { 0 };
    bool close_dialog { false };
    int close_choice { 0 };            // 0 background, 1 power off
    bool close_remember { false };
    std::optional<Confirm> confirm;
    std::string notice;
    std::function<void()> later;       // done after the frame (what replaces what the frame drew)
    // The status-area icon, in the tray process (design 2026-10-19 §3):
    // this window's line to it, once it is wanted.
    std::unique_ptr<TrayLink> tray;
    std::string tray_error;            // why there is none here
    std::string tray_backend;
    int tray_pid { 0 };
    platform::FileLock alive;          // windows/<pid>.lock: held while this window lives
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
        int desktop_frames { 0 };                     // a desktop's frames received (rectangles)
        std::string renderer;                         // SDL's: "software" is no GPU
        std::optional<std::chrono::steady_clock::time_point> input_at;
        int frames_since_input { 0 };
    } perf;
    // Interactions measured (design 2026-10-19 §4.2): what each took, in ms;
    // a pending one is closed by the next frame shown (what it started, on the screen).
    std::map<std::string, std::vector<double>> latency;
    std::map<std::string, std::chrono::steady_clock::time_point> latency_pending;
    std::optional<std::chrono::steady_clock::time_point> tray_chose_at, quick_opened_at;
    std::optional<View> view_drawn;    // the view the last frame drew
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
    std::optional<service::QuickStartOffers> offers;   // what the indexes offer (design 2026-10-18 §6.4), once asked
    std::unique_ptr<std::jthread> availability_job;
    std::optional<ImportPreview> import;
    std::string quick_said;            // the quick start's line about the chosen edition
    bool quick_can_make { true };
    std::string auto_name;             // what the name field was given, until the person types
    int new_edition { 3 };             // core
    std::string new_offer;             // a system from the indexes' (its ref), chosen over new_edition
    std::string new_trad;              // a traditional desktop (its ref), made on kBases[new_base]
    int new_base { 1 };                // tiny, core, desktop
    int new_desktop { -1 };            // the desktop it is made with (offers->desktops); -1: its own
    int new_mode { 0 };                // 0 sandbox, 1 machine
    std::string new_mode_for;          // the card the mode was defaulted for
    int new_persist { 0 };             // all, home, none
    float panel_h { 0 };               // the quick start's panel, as tall as it was last drawn
    float panel_actions_h { 0 };       // its foot (making it), likewise
    bool more { false };
    // Admin
    int admin_selected { 0 };
    std::string admin_tab { "systems" };
    int admin_page { 0 };              // overview, versions, settings, export & remove
    std::optional<Json> history;       // the selected system's generations
    std::string history_for;
    std::optional<Json> upgrade_plan;
    std::string attach;                // --attach
    // A desktop opened this once (--desktop, the Host key's menu), or the
    // console instead of its desktop (design 2026-10-18 §3): its settings stay.
    std::string once_desktop;
    bool once_console { false };
    // One program alone in this window, the window its (`luban run
    // --display window`, design 2026-10-18 §7 T0): it closes when the program ends.
    std::string once_app;
    std::vector<std::string> menu_desktops;   // the current system's, read when the menu opens
    std::string shown_desktop;                // the session on screen (empty: none)
    // The session to show for `e` now: this once's, else its plan's.
    std::optional<std::string> session_now(const service::Environment& e) const;
    bool dump_state { false };
    std::string input;                 // tests: typed into the console (or desktop) once it shows something
    std::optional<std::chrono::steady_clock::time_point> input_at;
    bool quit { false };
    int exit_code { 0 };               // the app's, when it quits
    // The console: scrolled back, a selection being made.
    int console_scroll { 0 };
    std::optional<std::array<int, 4>> selection;
    bool selecting { false };
    // The console's pointer (design 2026-10-18 §12, E16): the cell last
    // reported to a program that takes the mouse and the button held there;
    // clicks counted for a word (two) or a line (three); what was selected
    // last, which a middle click pastes; the right button's menu.
    std::pair<int, int> mouse_cell { -1, -1 };
    int mouse_held { -1 };
    int clicks { 0 };
    std::chrono::steady_clock::time_point clicked_at;
    int clicked_frame = -10;
    std::pair<int, int> clicked_cell { -1, -1 };
    std::string primary;
    std::optional<ImVec2> console_menu;
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
    void refresh();

    // What the countdown goes into: the default system, else the one opened last, else the first.
    std::string countdown_target() const;

    std::string title_for(std::string_view what) const;

    void start_job(std::string title, std::function<void(Job&, service::Context)> body, std::function<void(App&)> then);

    // The edition of a system, as a name ("core").
    static std::string edition_name(const service::Environment& e);

    void enter(const std::string& name);

    void open_run(const std::string& name);

    // A machine shows its desktop when its root declares one (and its
    // setting is not `none`), else its console.
    void open_machine(const std::string& name);

    // The screen's density, as a remote desktop is told it; tests set it (--density).
    double density_override { 0 };
    double density() const;
    // Device pixels per unit of the window's coordinates.
    double pixel_density() const;

    // A desktop's size for a window of w x h: in its device pixels, so that
    // what the desktop draws is as sharp as the screen.
    opensal::Size device_size_(int w, int h) const;

    // What the environment lets cross the clipboard: its grants; an
    // environment without a policy file is as open as it always was, and a
    // policy that cannot be read lets nothing cross.
    ClipboardGrant clipboard_grant(const std::string& name) const;

    void close_display();

    // The luban a sandbox runs for its desktop, as the sandbox sees it. A
    // system in a prefix domain sees its own home (/xlings), not this one:
    // the luban its domain carries. Elsewhere, the one beside this app, else
    // the home's.
    fs::path luban_binary(const std::string& name) const;

    void open_console(const std::string& name, bool machine);

    void power_off(const std::string& name);
    void app_failed_(const std::string& why);

    bool is_running(const std::string& name);

    float console_size(const std::string& name) const;

    void back_home();
};

using imkit::px;

// Tests: a script of input and checks (--script FILE), one line a frame,
// run after the frame is drawn. Input is injected as SDL delivers a person's.
//
//   wait <view> [seconds]           waitfor <path> <value> [seconds]
//   key <Key>[+<Key>...]            held in order, released in reverse
//   type <text>                     click <anchor> | click <x> <y>
//   move <x> <y>                    frames <n>        sleep <ms>
//   click <x> <y> [right|middle]    keydown <Key>     keyup <Key>
//   press <x> <y>                   release <x> <y>   (a drag between them)
//   resize <w> <h>                  close             quit
//   screenshot <file>               state <file>
//   see [<n>s] <text>               until the text is shown (a console's lines,
//                                    a job, a notice; default 60 s)
//   commit <text>                   as the input method commits it
//   drop <file>                     dropped on the window
//   clipboard <text>                the host's clipboard
//   cell <row> <col> [left|middle|right|double|triple]
//                                   a click on the console's cell
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

void begin_full_(const char* id, bool transparent = false);
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
void make_(App& a, bool agreed = false);
float new_panel_(App& a, float x, float y, float w, float min_h);
float panel_body_(App& a, float x, float y, float w);
float panel_actions_(App& a, float x, float y, float w);
void quick_start_(App& a);
// The quick start's cards in their groups (design 2026-10-19 §5.1):
// console, xdesk, desktops (traditional), more (systems from the indexes).
struct QuickCard {
    enum class Kind { Edition, Desktop, System };
    Kind kind { Kind::Edition };
    int edition { -1 };                 // kEditions
    service::Offer offer;               // a desktop or a system the index offers
    std::string id;                     // "card:<edition>" / "card:<ref>"
};
struct QuickGroup {
    std::string key;
    std::vector<QuickCard> cards;
};
std::vector<QuickGroup> quick_groups_(App& a);
std::string chosen_card_(const App& a);
void choose_card_(App& a, const QuickCard& c);
void quick_keys_(App& a);
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
