export module xlings.core.home_config;

import std;

import xlings.libs.json;
import xlings.core.xvm.lock;

// Serialized read-modify-write of `<home>/.xlings.json`.
//
// One file, several owners. `versions` is written by install/remove/use,
// `workspace` by the same three, `subos` and `activeSubos` by the subos
// commands, `lang` by `xlings config`, `index_repos` by the MCP repo
// capabilities, `mirror` and `version` by `xlings self install`. Each owner
// reads the whole document, edits its own key and writes the whole document
// back.
//
// That is safe only while the read and the write are one indivisible step.
// They were not. Two effects, both silent:
//
//   Lost update. `xlings subos new big --storage image` reads the config,
//   spends seconds in mkfs.ext4, then writes back what it read. An install
//   that finished in between had its `versions` entry in the document the
//   subos command never re-read, so the write puts the file back to its
//   pre-install content. The package stays on disk with no record of it.
//
//   Torn state. Since 0.4.70 the version database and the workspace are two
//   halves of one release (see xvm/lock.cppm). Reverting `versions` while
//   `workspace` keeps the newer content leaves a group manifest naming a
//   member the database no longer has, and the whole toolchain refuses to
//   switch.
//
// install/remove/use already took the home-wide state lock. The other writers
// did not, so the lock only protected them from each other.
//
// The rule this module enforces is narrower than "hold the lock for the whole
// command", deliberately. `subos new` runs mkfs and `subos rm` runs
// remove_all; holding the lock across either would make a routine install in
// another terminal wait past its 30s timeout and fail. So the lock covers the
// commit alone, and the document handed to `mutate` is re-read *inside* it.
//
// The consequence for callers is the part worth stating plainly: **anything
// decided from a lock-free read may be stale by the time `mutate` runs.**
// A caller that checked "this subos does not exist yet" before doing the slow
// work has to check again inside `mutate`, and return false if the answer
// changed.
//
// Not covered here: the *project* state file and the per-subos `.xlings.json`.
// Those are scoped to one project tree or one subos rather than to the home,
// and the state lock is per-home; giving them the home lock would serialize
// unrelated projects against each other. They keep the pre-existing
// last-writer-wins behavior.
export namespace xlings {

std::filesystem::path home_config_path(const std::filesystem::path& home);

// Parse `<home>/.xlings.json`, or an empty object if it is absent, empty,
// unreadable or malformed. Matches what every hand-rolled reader in the tree
// did before this module existed: a corrupt config is treated as no config
// rather than failing the command.
nlohmann::json read_home_config(const std::filesystem::path& home);

// ── Partial reads of the home config ────────────────────────────────
//
// `versions` sits near the end of the home config's alphabetical key order
// and holds ~90% of the bytes on a real home (measured: 2.4 MB of a
// 3.65 MB file, 3970 entries). It is also the one key a shim dispatch
// never needs. A shim that paid a full DOM parse for it paid ~450 ms per
// tool invocation on an -O0 build and ~30 ms even optimized.

// How much of the home config one capture materializes.
enum class HomeCaptureMode {
    // Stop the parse as soon as the `versions` key appears. Captures every
    // key that sorts before it -- `activeSubos`, `subos`, `mirror`, `lang`,
    // `dbIndex`, the UI prefs -- and lexes only that far. What a shim
    // dispatch needs, and nothing more.
    AbortAtVersions,
    // Lex the whole file but never materialize `versions`. Captures
    // everything else, including `xim` which sorts after `versions`. What
    // the CLI needs on a home whose versions DB lives in its own file.
    SkipVersions,
};

struct HomeConfigCapture {
    // The captured top-level keys. Never contains `versions`.
    // Assignment-init, not brace-init: `json {object()}` constructs an
    // ARRAY holding an object, which is not what any reader wants here.
    nlohmann::json json = nlohmann::json::object();
    // The root was an object and no parse error occurred. When false, `json`
    // holds nothing and every consumer must treat the config as unreadable
    // -- never as empty.
    bool ok { false };
    // True for AbortAtVersions: the parse stopped at `versions`, so keys
    // that sort after it (`xim`) are absent BY DESIGN, not because the file
    // lacks them.
    bool truncated { false };
    // Size and mtime of the file the capture came from. A caller that
    // derives state from the capture (e.g. knownProjects' change stamp)
    // needs these to match what a writer will stat afterwards.
    std::uintmax_t size {};
    std::filesystem::file_time_type mtime {};
};

// Read (part of) the home config, memoized per process.
//
// Within one process the file is parsed once per mode: a second caller with
// the same mode gets the memoized capture after one stat. The memo validates
// against the file's current size+mtime -- a mismatch (the config was
// rewritten since, e.g. by reload_state_ after a lock acquisition) re-parses.
// There is no cross-process sharing, so no stale-capture window exists
// beyond the one any single read already has.
//
// Returns nullptr when the file does not exist. A file that exists but
// cannot be parsed yields a capture with ok == false, memoized like any
// other: re-parsing a corrupt file every call buys nothing.
[[nodiscard]] std::shared_ptr<const HomeConfigCapture>
home_config_capture(const std::filesystem::path& configPath,
                    HomeCaptureMode mode);

// Where this home's versions DB lives once it has one of its own.
std::filesystem::path versions_db_path(const std::filesystem::path& home);

// The home's versions database as JSON, from ONE place.
//
// Preference: `<home>/data/versions.json` (the DB written by save_versions
// since the versions-out-of-home-config migration) → the home config's
// `versions` field (every home written before it, and any home where the
// DB file could not be written).
//
// The DB file is a DERIVED copy, and the home config's field is what every
// client ≤2026.9.30.1 writes (they do not know the DB file exists). So the
// DB file carries a STAMP of the home config's size+mtime taken when both
// were written together, and the reader TRUSTS the file only while the
// config's current stat still matches: an older client's write changes the
// config and leaves the stamp behind, and the reader falls back to the
// config field — the config, which every writer updates, is the fresher of
// the two by construction. (Size alone catches the common case — installs
// and removes change it; the mtime covers equal-length rewrites up to the
// filesystem's timestamp granularity, the same residual the view cache's
// fingerprint carries.)
//
// Returns nullopt only when the DB file is unreadable AND the home config
// exists but cannot be parsed -- UNOBSERVED state, which the consumers that
// refuse on nullopt (profile.cpp's payload collector) must keep refusing
// rather than read as "references nothing". An absent home config, or a
// readable one with no versions yet, is an observed EMPTY database -- a
// legitimate state, not a failure.
//
// The returned object is the versions MAP itself (program name → entry),
// the same shape as the home config's `versions` field, so
// xvm::versions_from_json consumes it unchanged. Memoized per process
// against both files' stats: home_knows_program's existence check and
// Config's version load share one parse.
[[nodiscard]] std::optional<nlohmann::json>
load_versions_json(const std::filesystem::path& home);

// Apply `mutate` to a freshly-read `<home>/.xlings.json` while holding the
// home-wide state lock, then write it back.
//
// `mutate` returns whether the document changed. Returning false skips the
// write entirely, which keeps a no-op `xlings config` from rewriting the file
// and gives a caller that lost a race a way to abort without inventing an
// error path.
//
// Returns the commit decision, or the reason the update could not happen:
// another xlings holding the lock past the timeout, or the write failing.
// Both are real failures the caller must surface -- reporting success after
// either one tells the user a change landed that did not.
std::expected<bool, std::string> update_home_config(
        const std::filesystem::path& home,
        const std::function<bool(nlohmann::json&)>& mutate,
        std::chrono::milliseconds timeout = xvm::default_lock_timeout());

}  // namespace xlings
