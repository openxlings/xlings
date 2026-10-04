export module xlings.subos.session;

import std;
import xlings.libs.json;
import xlings.subos.home_view;

// Sessions (design §12, §16): one running sandbox per instance, hosted by a
// supervisor outside it.
//
//   host:     supervisor (an xlings process) -- exec.sock, audit, signals
//                 |  fork + wait (no more execvp: something outside the
//                 |  sandbox stays to watch it, F15)
//   sandbox:  session-init (xlings in an internal mode, the first process)
//                 |-- the main command (the shell of `use`, or `--cmd`)
//                 `-- commands that joined (`exec`, a second `use`)
//
// A client joins by connecting to <home>/run/subos/<n>/exec.sock and passing
// its stdin/stdout/stderr with SCM_RIGHTS. The supervisor records the request
// and hands request and descriptors to session-init over a private socket;
// the process inside reads and writes the client's terminal or pipes
// directly -- nothing relays the bytes. The exit status comes back the same
// way. Clients never talk to the sandbox: only the supervisor does, so the
// sandbox cannot answer for it.
//
// Messages are one JSON object per SOCK_SEQPACKET datagram, descriptors
// attached to the datagram they belong to.
export namespace xlings::subos::session {

namespace fs = std::filesystem;

// Exit codes (design §12.2), shared by every surface that runs a command.
inline constexpr int kExitSetup = 125;      // failed before the command started
inline constexpr int kExitCannotRun = 126;  // found, could not be executed
inline constexpr int kExitNotFound = 127;   // not found
inline constexpr int kExitTimeout = 124;    // --timeout
// signal n -> 128 + n

// What a live session publishes in run/subos/<n>/session.json.
struct Info {
    std::string instance;
    std::string id;
    int supervisor_pid { 0 };
    int sandbox_pid { 0 };
    std::string started;            // UTC
    std::string backend;
    std::string digest;             // of the compiled spec: a join with a different spec is refused
    int ttl { 0 };                  // idle seconds after the last command; 0 = until stop
    bool detached { false };        // started by `subos start`
};

nlohmann::json to_json(const Info& i);
Info info_from_json(const nlohmann::json& j);

// The live session of an instance. A session.json whose supervisor is gone
// is stale: it is removed, and the instance has no session.
std::optional<Info> find(const HomeView& home, std::string_view instance);
std::vector<Info> list(const HomeView& home);

// ── Hosting ──────────────────────────────────────────────────────────

struct Launch {
    std::string instance;
    std::vector<std::string> argv;               // the backend's argv, session-init inside
    std::map<std::string, std::string> env;      // the backend's environment
    std::vector<int> keep_fds;                   // descriptors the backend must inherit
    std::string backend;
    std::string digest;
    nlohmann::json spec;                         // SandboxSpec::describe(), for the audit
    std::map<std::string, std::string> exec_env; // base environment of joined commands
    std::vector<std::string> env_pass;           // what a joined command may add
    std::string default_cwd;                     // inside
    int ttl { 0 };
    bool detached { false };
};

// The environment variables session-init reads its control socket and its
// idle TTL from. `host` sets both; the caller does not.
inline constexpr std::string_view kControlFdEnv = "XLINGS_SESSION_FD";
inline constexpr std::string_view kTtlEnv = "XLINGS_SESSION_TTL";

// Run the session to its end. Attached: returns the main command's exit code
// (128+n for a signal). Detached: forks a supervisor that outlives this
// process and returns 0 once the session is ready, or kExitSetup.
int host(const HomeView& home, Launch launch);

// ── Joining ──────────────────────────────────────────────────────────

struct ExecRequest {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;      // candidates; filtered by the session
    std::string cwd;                             // empty = the session's default
    std::optional<std::chrono::milliseconds> timeout;
    bool tty { false };
};

struct ExecResult {
    int exit_code { kExitSetup };
    std::string phase;          // "setup" when it never started
    std::string error;
};

// Run a command in the live session of `instance`, with this process's
// stdin/stdout/stderr.
ExecResult join(const HomeView& home, std::string_view instance, const ExecRequest& request);

// Ask a live session to end (SIGTERM to its sandbox through the supervisor).
bool stop(const HomeView& home, std::string_view instance);

// ── Inside ───────────────────────────────────────────────────────────

// `xlings __session-init [-- <argv...>]`: the sandbox's first process.
int session_init(std::span<const std::string> argv);

}  // namespace xlings::subos::session
