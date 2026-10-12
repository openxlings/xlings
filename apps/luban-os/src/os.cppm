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

// Draws `c` filling the current imgui window; returns the cell grid it fits.
std::pair<int, int> draw_console(Console& c, void* mono_font, float font_size, bool focused);

}  // namespace luban::os
