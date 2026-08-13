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
