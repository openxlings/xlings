export module xlings.testkit;

import std;
import xlings.libs.json;

// End-to-end test support (design §24.3, checkpoint T1).
//
// Four things, each with one answer:
//
//   which binary      `xlings_binary()`: $XLINGS_BIN, else the newest
//                     target/**/bin/xlings. A test never builds or guesses.
//   which home        `Home::isolated()`: a fresh directory under the system
//                     temp dir, never under $HOME, removed afterwards unless
//                     the test failed or XTEST_KEEP=1.
//   which environment `run()` starts from NOTHING and adds what the test
//                     names. Inheriting is what let XLINGS_ACTIVE_SUBOS from a
//                     developer's shell decide which subos a unit test saw.
//   why it did not run `check_requirements()`: a missing capability is a skip
//                     on a developer machine and a FAILURE on a CI lane that
//                     declared it (XDEV_LANE_CAPS). A lane that silently skips
//                     its own subject is F13 in the design.
export namespace xlings::testkit {

namespace fs = std::filesystem;

// The platform, as constants for `if constexpr`. Spelled like
// xlings::platform's, and not imported from it: the tool that measures the
// product does not share its modules.
#if defined(_WIN32)
inline constexpr bool is_windows = true;
#else
inline constexpr bool is_windows = false;
#endif
#if defined(__APPLE__)
inline constexpr bool is_macos = true;
#else
inline constexpr bool is_macos = false;
#endif
#if defined(__linux__)
inline constexpr bool is_linux = true;
#else
inline constexpr bool is_linux = false;
#endif
inline constexpr bool is_posix = !is_windows;

// ── Metadata ─────────────────────────────────────────────────────────

enum class Cost { Fast, Medium, Slow };

struct Meta {
    std::string area;                    // "subos", "xim", "testkit", ...
    Cost cost { Cost::Fast };
    std::vector<std::string> covers;     // requirement IDs (tests/requirements.toml)
    std::vector<std::string> requires_;  // capabilities, see `capability_names()`
    std::vector<std::string> resources;  // exclusive locks: "sandbox", "port:8080", ...
    // "flow" for a test that runs the fake provider: it proves the path is
    // wired, not that anything is isolated. Isolation requirement IDs are only
    // covered by "isolation" tests (design §24.4).
    std::string proves { "flow" };
};

std::string_view to_string(Cost c);

// Static registration; returns true so it can initialise a namespace-scope
// bool. Every registered test is written to $XTEST_META_OUT (NDJSON) at exit,
// whether or not it ran -- the coverage map needs the tests a filter skipped.
bool register_meta(std::string_view test, Meta meta);

// The registry, for xdev and for testkit's own tests.
const std::map<std::string, Meta, std::less<>>& registry();

// ── Capabilities ─────────────────────────────────────────────────────

// Known capability names. An unknown name in `requires_` is a test bug and
// fails the test: a typo must not turn into a permanent skip.
std::span<const std::string_view> capability_names();

// Probe one capability on this host. Cached per process.
// nullopt = present; otherwise the reason it is missing.
std::optional<std::string> probe(std::string_view capability);

// For the tools around the tests (xdev): this process's environment, and
// what std::system's return value means, without their own system headers.
void set_env(const std::string& name, const std::string& value);
// The command's exit code, or 128 + the signal that ended it; -1 when the
// shell itself could not run.
int system_exit_code(int raw);

// The capabilities this lane promised, from XDEV_LANE_CAPS (comma separated).
bool lane_declares(std::string_view capability);

struct Verdict {
    bool fail { false };     // true: report as a failure, false: a skip
    std::string reason;
};

// nullopt = run the test. The lane's declarations come from XDEV_LANE_CAPS;
// the second form takes them explicitly (the rule itself is tested that way).
std::optional<Verdict> check_requirements(const Meta& meta);
std::optional<Verdict> check_requirements(const Meta& meta,
                                          std::span<const std::string> lane_caps);

// ── Processes ────────────────────────────────────────────────────────

struct RunOptions {
    std::vector<std::string> argv;               // argv[0] is the program
    std::map<std::string, std::string> env;      // the WHOLE environment
    fs::path cwd;                                // empty = temp dir
    std::chrono::milliseconds timeout { std::chrono::seconds(120) };
    std::string stdin_data;                      // written, then stdin closed
    // Run on a pseudo-terminal with nothing typed. A program that waits for
    // input there times out instead of finishing -- which is the observation
    // the agent contract scan makes. POSIX only.
    bool pty { false };
};

struct RunResult {
    int exit_code { -1 };       // -1 when it did not exit (timeout / signal)
    int signal { 0 };
    bool timed_out { false };
    std::string out;            // with pty: everything the terminal received
    std::string err;
    std::chrono::milliseconds elapsed { 0 };

    // Each line of `out` that parses as a JSON object, in order.
    std::vector<nlohmann::json> json_lines() const;
    // stdout + stderr, for failure messages.
    std::string transcript() const;
};

RunResult run(const RunOptions& options);

// The binary under test. Empty when there is none (the caller decides).
fs::path xlings_binary();

// This process's own environment, for tools (xdev) that run commands the way
// a developer would. Tests use Home::env() instead.
std::map<std::string, std::string> inherited_env();

// ── Homes ────────────────────────────────────────────────────────────

class Home {
public:
    // A new, empty xlings home in the system temp dir, with a user home
    // next to it. `name` only makes the directory recognisable.
    static Home isolated(std::string_view name);

    Home(Home&&) noexcept;
    Home& operator=(Home&&) noexcept;
    Home(const Home&) = delete;
    Home& operator=(const Home&) = delete;
    ~Home();

    const fs::path& root() const { return root_; }       // $HOME of the run
    const fs::path& dir() const { return dir_; }         // $XLINGS_HOME

    // The hermetic environment every run in this home starts from.
    std::map<std::string, std::string> env() const;

    // `xlings <args...>` in this home. `extra_env` is added on top of env().
    RunResult xlings(std::vector<std::string> args,
                     std::map<std::string, std::string> extra_env = {},
                     std::chrono::milliseconds timeout = std::chrono::seconds(120)) const;
    RunResult xlings(RunOptions options) const;   // argv without the binary

    // Place a sandbox backend this host already has into the home, so a
    // sandbox test does not download one. Returns false when there is none.
    bool seed_sandbox_backend() const;

    // Keep the directory after the test (set automatically on failure).
    void keep() { keep_ = true; }

private:
    Home() = default;
    fs::path root_;
    fs::path dir_;
    bool keep_ { false };
};

// ── Failure capture ──────────────────────────────────────────────────

// Installed by xtest.hpp: whether the running test has failed, and its name.
using FailureProbe = bool (*)();
using NameProbe = std::string (*)();
void set_probes(FailureProbe failed, NameProbe name);

// Where failure artefacts go: $XTEST_ARTIFACTS, else target/xtest-artifacts.
fs::path artifacts_dir();

// Write the result line for one finished test to $XTEST_RESULTS_OUT.
void record_result(std::string_view test, std::string_view status,
                   long long elapsed_ms, std::string_view message);

// Small helpers shared by tests.
std::string read_file(const fs::path& path);
void write_file(const fs::path& path, std::string_view content);
std::string current_user();

}  // namespace xlings::testkit
