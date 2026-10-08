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
bool take_controlling_terminal();       // stdin's terminal, TIOCSCTTY
// fds[0..2] become this process's stdin, stdout, stderr.
void redirect_stdio(std::span<const int> fds);
// argv with a deadline, its own process group, killed whole when late (124).
int run_argv_with_timeout(const std::vector<std::string>& argv, std::chrono::milliseconds limit);

struct UserIds { unsigned uid { 0 }; unsigned gid { 0 }; unsigned euid { 0 }; };
UserIds user_ids();

std::map<std::string, std::string> environment();
bool stdout_is_terminal();
void unset_env_variable(const std::string& name);

// ── descriptors ──────────────────────────────────────────────────────

void close_fds(std::vector<int>& fds);
bool set_inheritable(int fd, bool inheritable);
// Close-on-exec unless asked otherwise.
std::optional<std::array<int, 2>> make_pipe(bool cloexec = true);
bool read_exact(int fd, void* buf, std::size_t size);
int open_null();
// Owner-only (0600), created when missing, appended to.
int open_for_append(const std::filesystem::path& path);

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

struct Message {
    std::string data;
    std::vector<int> fds;       // received descriptors, close-on-exec
};
bool send_message(int sock, std::string_view data, std::span<const int> fds = {});
// nullopt on end of stream or error.
std::optional<Message> receive_message(int sock, std::size_t max = std::size_t{1} << 20);

}  // namespace xlings::platform
