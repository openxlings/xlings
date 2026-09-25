export module xlings.core.destructive_log;

import std;

// A record of every destructive operation, one NDJSON line each, in
// `<XLINGS_HOME>/logs/destructive.ndjson`.
//
// It exists because of an investigation that could not be finished: a
// maintainer lost user data from several SubOS home directories and suspected
// auto-repair, an upgrade or `self doctor --fix`. Every command that can
// delete a SubOS was found and measured, but nothing had written down which
// one ran, when, from where, or how it was confirmed -- so the loss could not
// be attributed to any of them. The next one can.
//
// Recording never fails the operation it describes: a log that cannot be
// written is a lost log line, not a refused deletion.
export namespace xlings::destructive_log {

struct Entry {
    std::string op;               // "subos-remove", "self-install-overwrite", "gc", ...
    std::filesystem::path path;
    std::uintmax_t bytes { 0 };
    std::uintmax_t files { 0 };
    // How the person running it said yes: "terminal", "-y", "yes:true" -- or
    // "automatic" for derived data a repair may remove without asking.
    std::string confirmedBy;
    std::string detail;
};

// The command line of this process, recorded with every entry. Set once by
// main(); an entry written before it is set carries an empty command.
void set_command(std::string command);

void record(const Entry& entry) noexcept;

[[nodiscard]] std::filesystem::path log_path();

// Total bytes and regular files under `root`, symlinks not followed. Shared
// so the number in a confirmation prompt and the number in the log are
// computed the same way.
struct Size { std::uintmax_t bytes { 0 }; std::uintmax_t files { 0 }; };
[[nodiscard]] Size measure(const std::filesystem::path& root);

// "2.5 GB", "812 KB", "0 B" -- for prompts and messages.
[[nodiscard]] std::string human_bytes(std::uintmax_t bytes);

}  // namespace xlings::destructive_log
