module;

#include <ctime>
#include <cstdio>

export module xlings.core.profile;

import std;

import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.core.xvm.db;
import xlings.core.xvm.types;

namespace xlings::profile {

namespace fs = std::filesystem;

export struct Generation {
    int                                    number;
    std::string                            created;
    std::string                            reason;
    std::map<std::string, std::string>     packages;
};

std::string utc_now_iso_();

int next_gen_number_(const fs::path& gensDir);

// NOTE: `.workspace.xvm.yaml` used to be written here and by rollback().
// Nothing in the tree ever read it -- which is what made `profile rollback`
// a no-op with respect to version selection: it recorded an intent in a file
// no reader consulted, and the live workspace kept whatever it had. Rollback
// now returns the selection so the caller can apply it through the real
// switch path, and the file is gone rather than left as a decoy.

std::uintmax_t dir_size_(const fs::path& dir);

export Generation load_current(const fs::path& envDir);

export int commit(const fs::path& envDir,
                  std::map<std::string, std::string> packages,
                  const std::string& reason);

export std::vector<Generation> list_generations(const fs::path& envDir);

// Roll the recorded selection back to a generation, and hand it to the
// caller to apply.
//
// Applying is deliberately not done here. Putting a version back means
// running the same switch `xlings use` runs -- resolving the release,
// moving shims, libraries, headers and declared file assets -- and that
// lives in xvm::commands, which imports xself, which imports this module.
// Returning the selection keeps the cycle out of the module graph and keeps
// one implementation of "make this version active" rather than two.
//
// This used to write the selection into `<envDir>/xvm/.workspace.xvm.yaml`
// and stop. Nothing read that file, so rollback changed no version at all.
export std::expected<std::map<std::string, std::string>, std::string>
rollback(const fs::path& envDir, int targetGen);

// One subos's workspace file, as read off disk.
//
// The unit every cross-subos question is answered in: "who else references
// this version", "which subos owns this finding", "is any other subos still
// pointing at something the DB no longer has". Those used to be three
// separate walks of ~/.xlings/subos/*/.xlings.json with three slightly
// different notions of what counts as a readable file; this is the one walk.
export struct SubosSnapshot {
    std::string         name;
    // The subos root, so a caller can tell "the subos named x under this home"
    // from "the project subos that happens to be named x" without guessing
    // from the name.
    fs::path            dir;
    xvm::SubosWorkspace workspace;
};

// Every subos under a home, in name order.
//
// Unreadable and malformed files are SKIPPED, not reported: this feeds
// read-only inspection and reference counting, and a subos whose config a
// user hand-edited into invalid JSON must not take down an unrelated
// `remove`. `current` is a symlink to the active one and would double-count.
export std::vector<SubosSnapshot> load_subos_snapshots(const fs::path& xlingsHome);

// Write one other subos's workspace back, preserving everything else in its
// state file.
//
// The counterpart to load_subos_snapshots, and it exists for one caller:
// `self doctor --fix` repairing a subos it is not currently in. Those repairs
// are pure deletions from `active` / `installed[]` -- a pointer at a version
// the shared database no longer has -- so nothing here needs to understand
// what the subos is for.
//
// Read-modify-write of the whole document rather than a targeted edit: the
// file carries fields this process does not model, and rewriting it from a
// SubosWorkspace alone would silently drop them. Callers hold the home's state
// lock; this does not take it, because it has no way to know whether the
// caller is mid-transaction over several files.
export bool save_subos_workspace(const fs::path& subosRoot,
                                 const xvm::SubosWorkspace& workspace);

// Build a set of referenced xpkg "dirname/version" keys from all subos workspaces
// by mapping workspace target names to xpkg directory paths via the versions DB.
std::set<std::string> collect_subos_references_(const fs::path& xlingsHome);

// Find which subos environments reference a given package target name.
export std::vector<std::string> find_subos_referencing(
        const fs::path& xlingsHome, const std::string& target);

// Which subos still pin an EXACT version of a target.
//
// The version-exact counterpart to find_subos_referencing(). `remove` keeps a
// payload alive when any OTHER subos still references the exact version being
// removed (installer.cppm's stillReferenced branch); naming those subos is
// what turns "✓ removed" from a claim the state contradicts into a next step.
//
// Version-exact and not name-only, because on a multi-version home the
// name-only answer names subos that pin a different version and therefore
// cannot explain why this payload was kept.
//
// Namespace handling mirrors is_version_referenced_anywhere_: stored values
// are namespaced for non-primary index repos (`local:0.0.1`), and the two must
// agree or this would omit a subos that really is holding the payload down.
export std::vector<std::string> find_subos_pinning_version(
        const fs::path& xlingsHome,
        const std::string& target,
        const std::string& version);

export int gc(const fs::path& xlingsHome, bool dryRun = false);

} // namespace xlings::profile
