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
import luban.service;
import luban.vm;
import luban.desktop.term;
import opensal;
import opensal.wire;

export namespace luban::os {

int run(int argc, char* argv[]);

}  // namespace luban::os

namespace luban::os {

namespace fs = std::filesystem;
namespace term = luban::desktop::term;
namespace service = luban::service;
namespace vm = luban::vm;

// The words of the interface, English or Chinese by LANG.
std::string_view tr(std::string_view en, std::string_view zh);

// What the app keeps of its own (<home>/config/luban-os.json): the default
// system, the countdown, when each was last used. An environment's own
// settings (mode, on-close) are the environment's (machine.json).
struct AppSettings {
    std::string default_system;
    int countdown { 10 };
    std::map<std::string, std::string> last_used;   // name -> ISO time
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
    static std::unique_ptr<Console> machine(const fs::path& home, const std::string& name, std::function<void()> wake,
                                            std::string& error);
    static std::unique_ptr<Console> sandbox(const fs::path& xlings, const fs::path& home, const std::string& name,
                                            const std::string& persist, std::function<void()> wake, std::string& error);
    // Any program on a pseudo-terminal (what a sandbox's shell is; tests run
    // a program of their own the same way).
    static std::unique_ptr<Console> process(const std::vector<std::string>& argv, std::map<std::string, std::string> env,
                                            const std::string& name, std::function<void()> wake, std::string& error);
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
class Display {
public:
    static std::unique_ptr<Display> machine(const fs::path& home, const std::string& name, const std::string& session,
                                            opensal::Size size, std::function<void()> wake, std::string& error);
    // In a sandbox: `xlings subos use <name> --cmd 'luban __desktop <session>'`,
    // opensal.wire on its standard input and output.
    static std::unique_ptr<Display> sandbox(const fs::path& xlings, const fs::path& luban, const fs::path& home,
                                            const std::string& name, const std::string& session, const std::string& persist,
                                            opensal::Size size, std::function<void()> wake, std::string& error);
    // Any program whose standard input and output are opensal.wire (what a
    // sandbox runs; tests run a desktop of their own the same way).
    static std::unique_ptr<Display> process(const std::vector<std::string>& argv, std::map<std::string, std::string> env,
                                            const fs::path& log, const std::string& name, opensal::Size size,
                                            std::function<void()> wake, std::string& error);
    ~Display();
    void send(const opensal::Event& e);
    bool alive() const { return alive_; }
    // Why it ended, when it could not start.
    std::string error() const { std::lock_guard l(m_); return error_; }
    const std::string& name() const { return name_; }
    bool is_machine() const { return client_.has_value(); }

    struct Update { opensal::Rect rect; std::string rows; };
    // The surface's size (0 until the first frame) and the rectangles since the last call.
    opensal::Size take(std::vector<Update>& out);

private:
    Display() = default;
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
    opensal::Size size_;
    std::vector<Update> updates_;
    std::string error_;
    opensal::wire::Decoder decoder_;
};

// The desktop session `name` shows when it is entered: its setting
// (`luban config <n> --desktop`), else the first its root declares; nothing
// when it declares none or the setting is `none`.
std::optional<std::string> desktop_session(const fs::path& home, const std::string& name, const std::string& setting);

// Draws `c` filling the current imgui window; returns the cell grid it fits.
std::pair<int, int> draw_console(Console& c, void* mono_font, float font_size, bool focused);

}  // namespace luban::os
