export module xlings.subos.broker;

import std;
import xlings.libs.json;
import xlings.subos.home_view;
import xlings.subos.policy;

// The broker (design §8, §9): how the xlings inside a sandbox changes what it
// cannot write.
//
// Inside, the home is read-only. A command that would change it is sent, as
// argv with its stdin/stdout/stderr, to the supervisor outside, which decides
// with the SAME policy::decide the client used for its early answer -- this
// one counts -- and then runs it on the host for this instance, writing to
// the caller's terminal directly. `ask` queues the request for the owner
// (`subos requests / approve / deny`); the caller gets exit 75 and the id.
export namespace xlings::subos::broker {

namespace fs = std::filesystem;

inline constexpr std::string_view kSocketInside = "/run/xlings/broker.sock";
// Where a sandbox without a view of its own (Landlock) finds it: the host
// path, named by this variable.
inline constexpr std::string_view kSocketEnv = "XLINGS_BROKER_SOCKET";
inline constexpr int kExitPending = 75;      // EX_TEMPFAIL: waiting for approval
inline constexpr int kExitPermission = 13;   // E_PERMISSION

enum class Route {
    Local,     // read-only, or the instance's own state: runs where it is
    Broker,    // changes the home: the supervisor decides and runs it
    Owner,     // the home, other instances, the policy: only from outside
};

struct Classified {
    Route route { Route::Local };
    std::vector<policy::Op> ops;   // what decide() is asked, one per target
};

// `argv` is the command after `xlings` and without global flags.
Classified classify(std::span<const std::string> argv, std::string_view instance);

// The supervisor's answer for a whole command: the strictest of its ops.
policy::Decision decide(const policy::Policy& p, const Classified& c);

// ── inside: the client ───────────────────────────────────────────────

// Whether this process is a sandbox's xlings with a broker to talk to.
bool available();

// Send the command and this process's stdio (or `stdio`: three descriptors);
// returns the exit code. `quiet_refusal`: a refusal (exit 13) prints nothing
// -- the caller has a way of its own (clipboard copy: the terminal).
int forward(std::span<const std::string> argv, std::span<const int> stdio = {}, bool quiet_refusal = false);

// ── outside: the queue of requests waiting for the owner ─────────────

struct Request {
    std::string id;
    std::vector<std::string> argv;
    std::string created;
    std::string reason;
};

std::string enqueue(const HomeView& home, std::string_view instance,
                    std::span<const std::string> argv, std::string_view reason);
std::vector<Request> pending(const HomeView& home, std::string_view instance);
// Removes it from the queue.
std::optional<Request> take(const HomeView& home, std::string_view instance, std::string_view id);

}  // namespace xlings::subos::broker
