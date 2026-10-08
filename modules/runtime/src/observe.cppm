export module xlings.observe;

import std;
import xlings.libs.json;

// The event model and the journal (design §22).
//
//   kind        ops | lifecycle | perm | exec | net | fs | destructive | trace
//   ts          UTC, second resolution, ISO 8601
//   fields      kind-specific; only ever ADDED to (the protocol rule)
//
// A journal is an NDJSON file. `append()` creates its directory, rotates it
// when it passes `max_bytes` (file -> file.1 -> ... -> file.<keep>), and
// reports failures through append_checked(); the compatibility append() ignores them.
export namespace xlings::observe {

enum class Kind { Ops, Lifecycle, Perm, Exec, Net, Fs, Destructive, Trace };

std::string_view to_string(Kind k);
std::optional<Kind> kind_from_string(std::string_view s);

// "2026-10-05T12:34:56Z"
std::string utc_now();

struct Event {
    Kind kind { Kind::Ops };
    std::string ts;                       // filled by append() when empty
    nlohmann::json fields = nlohmann::json::object();

    nlohmann::json to_json() const;
};

struct JournalLimits {
    std::uintmax_t max_bytes { 8u * 1024u * 1024u };
    int keep { 3 };
};

// Checked writes include directory creation, rotation, writing, flushing and closing.
// A false result means the caller cannot rely on this record being stored.
bool append_checked(const std::filesystem::path& file, const Event& event,
                    JournalLimits limits = {}) noexcept;
bool append_json_checked(const std::filesystem::path& file, const nlohmann::json& line,
                         JournalLimits limits = {}) noexcept;

// Never throws, never fails the caller.
void append(const std::filesystem::path& file, const Event& event,
            JournalLimits limits = {}) noexcept;
void append_json(const std::filesystem::path& file, const nlohmann::json& line,
                 JournalLimits limits = {}) noexcept;

// Every parsable line of a journal and its rotated predecessors, oldest
// first. Lines that do not parse are skipped (a torn write is not an error).
std::vector<nlohmann::json> read(const std::filesystem::path& file, bool include_rotated = true);

// ── Redaction ────────────────────────────────────────────────────────

// Names of the variables, sorted; never their values.
std::vector<std::string> redact_env(const std::map<std::string, std::string>& env);

// Whether a variable NAME looks like it holds a credential. Used to decide
// what an allow-list may never pass by pattern (it can still be named
// explicitly).
bool looks_secret(std::string_view name);

// ── Trace ────────────────────────────────────────────────────────────

// XLINGS_TRACE=home,caps,spec,provider,broker,session (or "all"). Read once.
bool trace_enabled(std::string_view category);

// One line on stderr, prefixed `[trace:<category>]`, when enabled.
void trace(std::string_view category, std::string_view message);

}  // namespace xlings::observe

export namespace xlings::observe::destructive {

// The destructive-operation record (AGENTS.md "SubOS user data"), moved here
// from src/core/destructive_log with its fields unchanged.
struct Entry {
    std::string op;               // "subos-remove", "self-install-overwrite", "gc", ...
    std::filesystem::path path;
    std::uintmax_t bytes { 0 };
    std::uintmax_t files { 0 };
    std::string confirmedBy;      // "terminal", "-y", "yes:true", "automatic"
    std::string detail;
};

// Identity stamped on every entry: the client version and its command line.
void set_identity(std::string version, std::string command);

// <home>/logs/destructive.ndjson
std::filesystem::path log_path(const std::filesystem::path& home);

void record(const std::filesystem::path& home, const Entry& entry) noexcept;

struct Size { std::uintmax_t bytes { 0 }; std::uintmax_t files { 0 }; };
// Total bytes and regular files under `root`, symlinks not followed.
Size measure(const std::filesystem::path& root);

// "2.5 GB", "812 KB", "0 B"
std::string human_bytes(std::uintmax_t bytes);

}  // namespace xlings::observe::destructive
