// Processes, signals and the descriptors between them -- the POSIX surface a
// supervisor needs (the SubOS session, its broker), so that nothing outside
// this module includes a system header for it.
//
// Declared on every platform; implemented on POSIX. Elsewhere each call
// reports failure (-1, false, nullopt) and the caller -- which already knows
// sessions are Linux/POSIX-only -- never gets that far.
//
// Signal numbers and errno values cross process boundaries as plain ints
// (in a JSON message, a wait status); the names below are this platform's.
export module xlings.platform:process;

import std;

export namespace xlings::platform {

// ── signals ──────────────────────────────────────────────────────────

namespace sig {
extern const int interrupt;   // SIGINT
extern const int quit;        // SIGQUIT
extern const int terminate;   // SIGTERM
extern const int hangup;      // SIGHUP
extern const int pipe;        // SIGPIPE
extern const int kill;        // SIGKILL
extern const int child;       // SIGCHLD
extern const int user2;       // SIGUSR2 (busybox init: power off)
}  // namespace sig

bool send_signal(int pid, int signal);
// To every process of the group `pgid` leads.
bool send_signal_group(int pgid, int signal);

// Signals delivered as bytes on a pipe, so a poll loop sees them: `forward`
// are caught, `ignore` ignored. One pipe per process; the descriptor to poll
// is returned (-1 when the pipe could not be made).
int route_signals(std::initializer_list<int> forward, std::initializer_list<int> ignore);
std::vector<int> pending_signals();
void ignore_signals(std::initializer_list<int> signals);
// In a child before exec: default dispositions, nothing blocked.
void reset_signals();

// ── errors ───────────────────────────────────────────────────────────

int last_error();                       // errno
std::string error_text(int error);
bool is_not_found(int error);           // ENOENT
bool is_not_executable(int error);      // EACCES, ENOEXEC, EISDIR

// ── processes ────────────────────────────────────────────────────────

struct ExitStatus {
    bool exited { false };
    int code { 0 };          // when exited
    int signal { 0 };        // when killed by one
};
// The shell's convention: the code, or 128 + the signal.
int exit_code(const ExitStatus& s);

// stdio is flushed first, so nothing buffered is written twice.
int fork_process();
[[noreturn]] void exit_now(int code);
// execvp's search against `env`'s PATH (a name with a `/` is run as is),
// with execvp's errno rule: a permission problem anywhere beats "not found".
// Returns only on failure, with the error.
int exec_program(const std::vector<std::string>& argv,
                 const std::map<std::string, std::string>& env);
// Exactly `argv[0]`, no search.
int exec_path(const std::vector<std::string>& argv, const std::map<std::string, std::string>& env);

// `block`: wait for it; otherwise nullopt while it runs.
std::optional<ExitStatus> wait_process(int pid, bool block);
// Any child that has ended, without blocking.
std::optional<std::pair<int, ExitStatus>> reap_child();

bool new_session();                     // setsid
// Gives up root for `uid`/`gid` (no supplementary groups). False when it could not.
bool become_user(unsigned uid, unsigned gid);
// `path` owned by `uid`/`gid` (not following a link). False when it could not be.
bool give_to(const std::filesystem::path& path, unsigned uid, unsigned gid);
bool take_controlling_terminal();       // stdin's terminal, TIOCSCTTY
// fds[0..2] become this process's stdin, stdout, stderr.
void redirect_stdio(std::span<const int> fds);
// argv with a deadline, its own process group, killed whole when late (124).
int run_argv_with_timeout(const std::vector<std::string>& argv, std::chrono::milliseconds limit);
// A shell command run for nobody: its own session (no controlling terminal)
// and the null device as input, so a question it asks -- a password, a
// confirmation read from /dev/tty -- fails at once instead of waiting where
// nobody can answer. Ctrl-C still reaches this process: it ends the child's
// group, then is delivered here as usual; when this process dies, the child
// does too (Linux). Past `limit` it is ended whole and 124 returned.
// Windows: the shell, as std::system.
int run_shell_unattended(const std::string& command,
                         std::optional<std::chrono::milliseconds> limit = std::nullopt);

struct UserIds { unsigned uid { 0 }; unsigned gid { 0 }; unsigned euid { 0 }; };
UserIds user_ids();

std::map<std::string, std::string> environment();
bool stdout_is_terminal();
// False when a standard stream is this process's controlling terminal and
// its foreground is another process group: a background job, or a command
// run for somebody else on their terminal. Such a process may write lines, and must not change the
// terminal's modes or query it -- the kernel stops the whole group for that
// (SIGTTOU). True with no terminal, and always on Windows (no process groups).
bool in_terminal_foreground();
void unset_env_variable(const std::string& name);

// ── descriptors ──────────────────────────────────────────────────────

void close_fds(std::vector<int>& fds);
bool set_inheritable(int fd, bool inheritable);
// Close-on-exec unless asked otherwise.
std::optional<std::array<int, 2>> make_pipe(bool cloexec = true);
bool read_exact(int fd, void* buf, std::size_t size);

// ── terminals ────────────────────────────────────────────────────────

// A new pseudo-terminal, {master, slave}, both close-on-exec, with the window
// size of terminal `size_from` when that is one.
std::optional<std::array<int, 2>> open_pty(int size_from = -1);
// Gives terminal `to` the window size of terminal `from`; true when it changed.
bool copy_window_size(int from, int to);
// Puts `fd`'s terminal in raw mode; the returned state restores it. nullopt
// when `fd` is not a terminal.
std::optional<std::string> make_terminal_raw(int fd);
void restore_terminal(int fd, const std::string& saved);
// Copies `in` to the master side of a pseudo-terminal and its output to `out`
// until the other side is closed, keeping the window size of `in` (a
// terminal) on it. Allocates nothing.
void relay_terminal(int master, int in, int out);
int open_null();
// Owner-only (0600), created when missing, appended to.
int open_for_append(const std::filesystem::path& path);
int open_for_read(const std::filesystem::path& path);           // -1 when it cannot

struct PollFd {
    int fd { -1 };
    bool readable { false };    // set by poll_fds
    bool closed { false };      // hang-up or error
};
// -1 waits forever. Returns how many are ready (0 on timeout).
int poll_fds(std::span<PollFd> fds, int timeout_ms);

// ── local sockets with descriptors ───────────────────────────────────
//
// Sequenced packets: one message is one datagram, carried whole, with up to
// eight descriptors. A path longer than sockaddr_un holds is reached through
// its directory's descriptor.

// Owner-only (umask 077): whoever connects can act on what listens.
int unix_listen(const std::filesystem::path& path);
int unix_connect(const std::filesystem::path& path);
int unix_accept(int listen_fd);
std::optional<std::array<int, 2>> unix_pair();
// Two connected byte streams (SOCK_STREAM): a channel one end of which is
// handed to something that reads and writes bytes (a virtual machine's port).
std::optional<std::array<int, 2>> unix_stream_pair();

// A byte stream to a listening socket (SOCK_STREAM): what qemu's chardev
// sockets are. -1 when nothing listens there.
int unix_connect_stream(const std::filesystem::path& path);
// Listens for byte-stream connections at `path` (owner-only).
int unix_listen_stream(const std::filesystem::path& path);

// ── plain descriptors ────────────────────────────────────────────────

// What `fd` has now, up to `buf.size()` bytes: 0 at end of stream, nullopt on
// an error (EINTR and EAGAIN are retried by the caller through poll_fds).
std::optional<std::size_t> read_some(int fd, std::span<char> buf);
// All of `data`, through short writes and EINTR.
bool write_all(int fd, std::string_view data);
// Read-write, no controlling terminal taken (a device, a fifo).
int open_device(const std::filesystem::path& path);
// The window size of terminal `fd` (rows, columns); nullopt for a non-terminal.
std::optional<std::pair<unsigned short, unsigned short>> terminal_size(int fd);
bool set_terminal_size(int fd, unsigned short rows, unsigned short cols);
// Whether a process of this id exists (kill(pid, 0)).
bool process_alive(int pid);
// This process's id.
int current_pid();
// `argv` in a session of its own that outlives this process (double fork),
// standard input the null device, output and errors appended to `log`. Its
// pid; nullopt when it could not be started.
std::optional<int> spawn_detached(const std::vector<std::string>& argv, const std::filesystem::path& log);

struct Message {
    std::string data;
    std::vector<int> fds;       // received descriptors, close-on-exec
};
bool send_message(int sock, std::string_view data, std::span<const int> fds = {});
// nullopt on end of stream or error.
std::optional<Message> receive_message(int sock, std::size_t max = std::size_t{1} << 20);

// Memory another process can map: an anonymous file of `size` bytes, mapped
// here read-write; its descriptor is what is handed over. nullopt where there
// is none (only Linux has it now).
struct SharedMemory {
    int fd { -1 };
    std::byte* data { nullptr };
    std::size_t size { 0 };
};
std::optional<SharedMemory> create_shared_memory(std::size_t size);
void release_shared_memory(SharedMemory& m);

}  // namespace xlings::platform
