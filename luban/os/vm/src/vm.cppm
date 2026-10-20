// A machine run by Luban (Luban OS architecture design §8): an environment
// booted with its own kernel in a local virtual machine -- not a copy of it:
// its own tree and the home are shared with the guest (9p), so what the
// machine writes is the environment's.
//
//   host   `luban enter <n> --mode machine` starts a supervisor (one per
//          machine, outliving the terminal), which runs qemu and serves the
//          machine to every client -- terminals now, the Luban OS window later --
//          over one socket; the client is the terminal's relay.
//   guest  the initramfs's /init (luban) mounts the shares, switches to the
//          environment's root, starts the agent and hands PID 1 to the
//          environment's own init; the agent gives each session a pty and the
//          root's login shell.
//
// Files: <home>/run/machines/<name>/ (state, sockets, the initramfs, a
// session's scratch copy), <home>/logs/machines/<name>/ (console, supervisor).
export module luban.vm;

import std;
import xlings.libs.json;
import luban.service;
import xlings.platform.duplex;

export namespace luban::vm {

namespace fs = std::filesystem;

struct EnterOptions {
    bool temp { false };            // nothing it writes stays
    std::string display;            // terminal | window; empty: the environment's
    std::string memory;             // MB; empty: 2048
    std::string user;               // who logs in this once; empty: its etc/luban/login
};

// The environment `name` as a machine, a session of it on this terminal
// (started when it is not running). An image file (`.iso`, `.img`, `.qcow2`)
// boots as it is, keeps nothing, and its console is this terminal.
int enter(const service::Context& c, std::string_view name_or_image, const EnterOptions& o);

// Whether `name` runs as a machine here, and its supervisor's pid.
std::optional<int> running(const fs::path& home, std::string_view name);

// Starts `name` as a machine when it is not running (the events go to the
// context's sink); 0 once it answers.
int start(const service::Context& c, std::string_view name, const EnterOptions& o);

// One command in the machine `name` (luban run of a system that starts as a
// machine): on this terminal when there is one, else on pipes, as `user`
// (empty: its login user); its exit code. A machine that was not running is
// started for it and powered off after.
int run(const service::Context& c, std::string_view name, std::span<const std::string> argv, std::string_view user);

// One connection to a running machine's supervisor: sessions on channels
// (luban.vm.protocol), what the terminal relay and the Luban OS window use.
// Sending is safe from more than one thread (a frame is written whole);
// poll is one thread's.
class Client {
public:
    // The machine's supervisor. On a PC the machine is in the Luban host
    // machine: reached through `wsl.exe ... luban __pipe-unix <its socket>`.
    static std::expected<Client, std::string> connect(const fs::path& home, std::string_view name);
    // A relay that speaks the protocol on its input and output: on a PC the
    // host machine's agent (`wsl.exe ... luban __agent-stdio`), where a
    // sandbox's console and desktop run as commands (open_command).
    static std::expected<Client, std::string> through(const std::vector<std::string>& argv, const fs::path& log = {});
    Client(Client&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), child_(std::move(other.child_)), buffer_(std::move(other.buffer_)),
          writing_(std::move(other.writing_)) {}
    Client& operator=(Client&& other) noexcept;
    ~Client();
    // A session: the root's login shell on a pty of this size.
    bool open(std::uint32_t channel, int rows, int cols, const nlohmann::json& env = nlohmann::json::object());
    // A desktop: the session `session` (empty: the first the environment
    // declares), headless at this size, its bytes opensal.wire (opensal-wlr).
    bool open_display(std::uint32_t channel, std::string_view session, int width, int height);
    // A command instead of the login shell: on a terminal of rows x cols, or
    // on pipes (`pipes`: its output as D, its standard error as E).
    // `user`: who runs it (empty: the root's login user).
    bool open_command(std::uint32_t channel, const std::vector<std::string>& argv, int rows, int cols,
                      const nlohmann::json& env, bool pipes, std::string_view user = {});
    // The end of a piped command's input.
    bool end_input(std::uint32_t channel);
    bool send(std::uint32_t channel, std::string_view bytes);
    bool resize(std::uint32_t channel, int rows, int cols);
    bool close(std::uint32_t channel);
    bool power_off();
    // The frames that arrived within `timeout_ms`; nullopt when the machine is gone.
    std::optional<std::vector<std::pair<char, std::pair<std::uint32_t, std::string>>>> poll(int timeout_ms);
    int fd() const { return fd_; }
private:
    explicit Client(int fd) : fd_(fd) {}
    bool write_(std::string_view frame);      // whole frames, from any thread
    int fd_ { -1 };
    std::shared_ptr<xlings::platform::duplex::Child> child_;
    std::string buffer_;
    std::unique_ptr<std::mutex> writing_ = std::make_unique<std::mutex>();
};

// The agent of this home's Luban host machine (a PC's WSL 2), on a relay:
// a sandbox's console and desktop there are commands it runs
// (Client::open_command). Elsewhere: why there is none.
std::expected<Client, std::string> host_machine_agent(const fs::path& home);

// The environment a session needs inside (its network's proxy), from the
// machine's state.
nlohmann::json session_env(const fs::path& home, std::string_view name);

// Power it off (the guest shuts down; after a while it is stopped).
service::Outcome stop(const service::Context& c, std::string_view name);

// Plumbing: `luban __vm-supervise <home> <name>` (the supervisor), and the
// guest's first process (the initramfs's /init).
int supervise(const fs::path& home, std::string_view name);
// `luban __vm <verb>`: the Luban host machine on a Mac -- the vz carrier's
// helper (hostvm.cpp). Elsewhere `probe` says why not and the rest refuse.
int host_machine(const fs::path& home, std::span<const std::string> args);
int guest_init(int argc, char* argv[]);
// The guest agent on a stream it is given instead of its port (tests: the
// protocol, sessions and commands without a machine). Never returns.
[[noreturn]] void serve_agent(int fd);
// The agent on this process's standard input and output (`luban
// __agent-stdio`): what a PC's Luban OS window reaches the host machine's
// sandboxes through, by wsl.exe. Powers nothing off. Never returns.
[[noreturn]] void serve_stdio();

// The machine's directories.
fs::path run_dir(const fs::path& home, std::string_view name);

// A running machine's environment: what it was started with (its state), else nothing.
std::optional<std::map<std::string, std::string>> started_env(const fs::path& home, std::string_view name);
fs::path log_dir(const fs::path& home, std::string_view name);

// A machine's sound (design §9.4): with the audio grant and a host
// PipeWire / PulseAudio socket, the guestfwd that relays to it and the
// PULSE_SERVER its sessions use; nothing otherwise.
struct SoundForward { std::string guestfwd; std::string pulse_server; };
std::optional<SoundForward> sound_forward(bool granted, const fs::path& pulse_socket, const fs::path& luban);

// Whether `file` is an ELF that needs no loader (what an initramfs can run).
bool is_static_elf(const fs::path& file);

}  // namespace luban::vm
