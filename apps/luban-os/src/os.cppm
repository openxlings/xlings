// Luban OS, the app (Luban OS architecture design §14).
//
// A window is a display. It shows one system at a time: the launcher (a list,
// a countdown to the default), then the system itself -- its console here;
// a desktop is opensal's surface -- and when it is off, that it is off. The
// management panel (F2, the gear, or the Host key's menu) is where everything
// else is. More windows are more processes: `luban-os` again, or the menu's
// "new window".
export module luban.os;

import std;
import xlings.libs.json;
import imkit;
import tray;
import luban.service;
import luban.vm;
import xdesk.term;
import opensal;
import opensal.wire;

export namespace luban::os {

int run(int argc, char* argv[]);

}  // namespace luban::os

namespace luban::os {

namespace fs = std::filesystem;
namespace term = xdesk::term;
namespace service = luban::service;
namespace vm = luban::vm;

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


}  // namespace luban::os
