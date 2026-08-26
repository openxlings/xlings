export module xlings.core.xvm.commands;

import std;

import xlings.core.config;
import xlings.core.subos.manifest;
import xlings.runtime;
import xlings.core.semver;
import xlings.core.entry_binary;
import xlings.core.xself;
import xlings.core.xself.repair;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.xvm.bindings;
import xlings.core.xvm.inspect;
import xlings.core.xvm.errors;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;

export namespace xlings::xvm {

namespace fs = std::filesystem;

// Cross-platform link: symlink on Unix, directory junction or copy on Windows
void create_link_(const fs::path& src, const fs::path& dst);

// Where a header asset lands: `<sysroot>/include/<destinationPrefix>`, or
// `<sysroot>/include` when the asset declares no prefix.
//
// The prefix exists because a source directory's name is not always the name
// the compiler looks under -- a toolchain's `include/c++/15.1.0` has to appear
// as `c++/15.1.0`, not as a flattened pile of its contents. Every asset the
// current recipe API can produce has an empty prefix (libxpkg's `xvm.setup`
// takes a single `includedir` and no destination), so today this is always
// the sysroot include root; it is honored anyway because the field is part of
// the persisted model and round-trips through the version database. A
// serialized field that materialization ignores is exactly the kind of state
// this release is removing, not adding.
fs::path header_destination_(const HeaderAsset& asset,
                             const fs::path& sysroot_include);

// Whether a payload directory may be linked into a sysroot.
//
// Everything xlings materializes lives in the home it materializes into --
// under `data/`, or elsewhere inside the home. A source outside both means the
// run's payload store and its sysroot belong to DIFFERENT homes, and the link
// it would leave behind outlives the run that made it: the measured case is an
// isolated run whose store was a temp dir and whose sysroot was the user's
// real `~/.xlings/subos/dev-hello`, leaving three header links pointing into a
// `/tmp` path that no longer exists. Every later `remove` and `install` of the
// package repaired the OTHER subos and reported success, because nothing was
// ever wrong there.
bool sysroot_source_is_local_(const fs::path& src);

// Install header symlinks from source includedir into sysroot include/
void install_headers(const std::string& includedir, const fs::path& sysroot_include);

// Remove header symlinks that point into the given source includedir
void remove_headers(const std::string& includedir, const fs::path& sysroot_include);

// Same two operations, addressed by header asset rather than by bare source
// directory, so the destination prefix is honored.
void install_headers(const HeaderAsset& asset, const fs::path& sysroot_include);

void remove_headers(const HeaderAsset& asset, const fs::path& sysroot_include);

// Place one file at an exact destination, replacing whatever is there.
//
// Shared by libraries and by declared file assets: both are "this payload
// file becomes that path in the subos", and both need the same replacement
// discipline.
//
// Replaces by rename rather than remove-then-link. Two versions of a library
// share a soname, so a switch overwrites the same name -- and `use`
// re-materializes the active release on every invocation to repair a drifted
// sysroot, so remove-then-link would open a window on every one of those
// calls where the file is simply absent. Long enough for a concurrent build
// step to fail on it. rename(2) replaces atomically; Windows has no
// equivalent for every entry kind, so the staging file is cleaned up and the
// direct path is taken there.
void place_asset(const std::string& source, const fs::path& destination);

// Take one placed file back out.
void remove_asset(const fs::path& destination);

// Remove the directories that only existed to hold a declared asset.
//
// Stops at three path components below `subosRoot`:
// `usr/include/xkbcommon` was created to hold one release's links and is
// litter once they are gone; `usr/include`, `usr/lib` and `etc/ssl` are the
// sysroot's own shape and are not ours to remove even when empty -- a
// compiler configured with `--sysroot` cares that `usr/include` exists.
// `remove` on a non-empty directory fails, so this can never take another
// package's assets with it.
//
// Exported because `self doctor --fix` deletes the same links by a different
// route and has to leave the same shape behind. A repairer that leaves what
// the remover cleans is how the two drift.
void prune_empty_asset_dirs(const fs::path& absolute,
                            const fs::path& subosRoot);

// Give up a set of declared destinations in one subos.
//
// Every path that stops wanting a declared asset ends here: a full uninstall,
// a detach that only opts this subos out, a re-registration whose new version
// declares fewer assets, and a `use` that moves to a release with a smaller
// asset set. They used to disagree -- `use` reported the leftovers and
// removed nothing, the detach path asked a question that could not match and
// so did nothing at all, and the uninstall path never asked (#423). The only
// thing that legitimately differs between them is which active view to
// reconcile against, so that is the only thing they pass differently.
//
// `activeAfter` is the workspace as it will be once the operation lands, not
// as it was when it started. A destination some *other* active release still
// declares is re-pointed at that release rather than deleted -- both halves
// matter: leaving it alone would strand a link into a payload being deleted,
// and deleting it would take a header away from a package that is still
// installed and still declares it. On a real installation exactly one path,
// `usr/include/scsi`, is claimed by two packages, and both mistakes are
// reachable there.
//
// Directories that only existed to hold these assets go too, down to three
// path components: `usr/include/xkbcommon` is ours, `usr/include` is the
// sysroot's own shape.
void reclaim_declared_assets(const fs::path& subosDir,
                             const fs::path& payloadRoot,
                             const std::set<std::string>& destinations,
                             const VersionDB& db,
                             const Workspace& activeAfter);

// Library-shaped wrappers, kept because the sysroot lib directory is implied
// rather than declared for libraries.
void place_library(const std::string& source,
                   const std::string& name,
                   const fs::path& sysroot_lib);

void remove_library(const std::string& name, const fs::path& sysroot_lib);

// Helper: filter a list of version keys (`ns:ver` or bare) down to those
// that appear in the current subos's installed[] for `target`. Tolerant
// of bare-vs-namespaced mismatch the same way detach_current_subos_ is —
// the stored form is namespaced for non-primary repos but callers may
// pass bare versions.
inline std::vector<std::string>
filter_to_subos_installed_(const std::string& target,
                           const std::vector<std::string>& candidates) {
    const auto& wsi = Config::workspace_installed();
    auto it = wsi.find(target);
    if (it == wsi.end()) return {};
    const auto& installed = it->second;

    auto in_installed = [&](const std::string& v) {
        for (auto& sv : installed) {
            if (sv == v || strip_namespace(sv) == v) return true;
        }
        return false;
    };

    std::vector<std::string> out;
    for (auto& v : candidates) {
        if (in_installed(v)) out.push_back(v);
    }
    return out;
}

// Refuse a prospective core-runtime activation that contradicts the current
// SubOS manifest. This runs immediately after the locked reload, before even
// legacy-edge self-healing: a refusal promised to change nothing cannot first
// rewrite the versions DB as an incidental repair.
bool runtime_activation_refused_(const VersionDB& db,
                                 const std::string& target,
                                 const std::string& requestedVersion);

// xlings use <target> <version>
// Updates the active subos workspace and creates/updates bin/ hardlinks
int cmd_use(const std::string& target, const std::string& version,
            EventStream& stream, bool strict = false);

// The versions `xlings use <target>` / the listing panel may choose from.
//
// 0.4.19+: defaults to **current subos scope** (only versions in
// `installed[]`), so a fresh subos shows an empty / minimal list rather than
// every version every other subos has ever installed. Pass `all=true` to opt
// back into the pre-0.4.19 global view (CLI exposes this via `--all`).
struct VersionCandidates {
    std::vector<std::string> versions;
    std::string active;   // empty when nothing is active in this subos
    std::string title;
};

// Errors are reported here (they already carry a next command) and returned
// as the exit code the caller should use, so both callers below fail the same
// way.
std::expected<VersionCandidates, int>
collect_version_candidates_(const std::string& target, bool all);

void emit_version_panel_(const std::string& target,
                         const VersionCandidates& candidates,
                         EventStream& stream);

// List versions for a target. Never switches anything.
//
// This is what the `list_installed_versions` capability runs, and the name is
// the whole contract: a caller that asked to *see* the versions must not come
// back to a different active toolchain. `use <target>` is below.
int cmd_list_versions(const std::string& target, EventStream& stream, bool all = false);

// `xlings use <target>` with no version — deterministic, or it lists.
//
// It used to decide by asking whether a terminal was attached: with a TTY it
// opened an arrow-key picker and blocked until somebody pressed a key,
// without one it printed the list and `return 0`. Both are unusable to
// anything driving xlings.
//
// The TTY gate does not even separate the two populations it was meant to:
// agents and terminal-automation tools routinely allocate a pty, so
// `stdin_is_terminal()` is true for them and they hang with no timeout. And
// the non-TTY branch is the worse half — it changed nothing, said nothing
// about that, and exited 0, so a script had every reason to believe the
// switch happened.
//
// **Whether a human is at the keyboard is not detectable; whether this
// command has a single correct outcome is.** So that is what decides:
//
//   1 candidate   → switch to it                                  (exit 0)
//   >1 candidates → change nothing, list them, name the exact
//                   command                                       (exit 0)
//   0 candidates  → error, as before                              (exit 1)
//
// The >1 case reported `[error]` and exit 2 in 2026.7.31.2, from applying
// "did nothing ⇒ non-zero" to a command that had not failed. That rule is
// about an *action* failing or silently no-op'ing; `use <target>` without a
// version is a *query*, and it answers it completely. What the rule really
// forbade was the old behaviour of printing a list when a single candidate
// made the switch unambiguous, and that is fixed above. So this branch is now
// literally the listing command -- same panel, same tip, exit 0.
//
// The cost, stated plainly: a script can no longer tell "switched" from
// "listed" by exit code alone. That is fine, because naming a version is how
// a switch is requested -- `xlings use gcc 16.1.0` is 0 on success and
// non-zero on failure, with no third meaning.
//
// `--pick` went with it. It existed to give the removed picker an explicit
// door; once the default path is deterministic, it is one more path to
// maintain, test and document for a problem that no longer exists.
int cmd_use_by_name(const std::string& target, EventStream& stream,
                    bool all = false, bool strict = false);

// Register a version in the global database (called after xim install)
void register_version(const std::string& target,
                      const std::string& version,
                      const std::string& path,
                      const std::string& type,
                      const std::string& filename);

} // namespace xlings::xvm
