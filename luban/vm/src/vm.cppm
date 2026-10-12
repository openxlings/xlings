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

export namespace luban::vm {

namespace fs = std::filesystem;

struct EnterOptions {
    bool temp { false };            // nothing it writes stays
    std::string display;            // terminal | window; empty: the environment's
    std::string memory;             // MB; empty: 2048
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

// One connection to a running machine's supervisor: sessions on channels
// (luban.vm.protocol), what the terminal relay and the Luban OS window use.
class Client {
public:
    static std::expected<Client, std::string> connect(const fs::path& home, std::string_view name);
    Client(Client&& other) noexcept : fd_(std::exchange(other.fd_, -1)), buffer_(std::move(other.buffer_)) {}
    Client& operator=(Client&& other) noexcept;
    ~Client();
    // A session: the root's login shell on a pty of this size.
    bool open(std::uint32_t channel, int rows, int cols, const nlohmann::json& env = nlohmann::json::object());
    bool send(std::uint32_t channel, std::string_view bytes);
    bool resize(std::uint32_t channel, int rows, int cols);
    bool close(std::uint32_t channel);
    bool power_off();
    // The frames that arrived within `timeout_ms`; nullopt when the machine is gone.
    std::optional<std::vector<std::pair<char, std::pair<std::uint32_t, std::string>>>> poll(int timeout_ms);
    int fd() const { return fd_; }
private:
    explicit Client(int fd) : fd_(fd) {}
    int fd_ { -1 };
    std::string buffer_;
};

// The environment a session needs inside (its network's proxy), from the
// machine's state.
nlohmann::json session_env(const fs::path& home, std::string_view name);

// Power it off (the guest shuts down; after a while it is stopped).
service::Outcome stop(const service::Context& c, std::string_view name);

// Plumbing: `luban __vm-supervise <home> <name>` (the supervisor), and the
// guest's first process (the initramfs's /init).
int supervise(const fs::path& home, std::string_view name);
int guest_init(int argc, char* argv[]);

// The machine's directories.
fs::path run_dir(const fs::path& home, std::string_view name);
fs::path log_dir(const fs::path& home, std::string_view name);

// Whether `file` is an ELF that needs no loader (what an initramfs can run).
bool is_static_elf(const fs::path& file);

}  // namespace luban::vm
