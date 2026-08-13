module;
#include <ctime>

export module xlings.core.xim.repo;

import std;
import xlings.core.config;
import xlings.core.mirror;
import xlings.core.xim.indexfetch;
import xlings.libs.tinyhttps;

export namespace xlings::xim {

namespace detail_ {

bool ensure_local_repo_link_(const std::filesystem::path& localDir,
                             const std::filesystem::path& sourceDir);

// Parse xim-indexrepos.lua to discover sub-index repositories.
// Format: xim_indexrepos["name"] = { ["GLOBAL"] = "url", ["CN"] = "url" }
std::vector<IndexRepo> discover_sub_repos_(const std::filesystem::path& repoDir,
                                            const std::string& mirror);

std::string sync_repo_url_(const std::string& url, const std::string& mirror [[maybe_unused]]);

}  // namespace detail_

// Exported wrapper for discover_sub_repos (used by catalog)
std::vector<IndexRepo> discover_sub_repos(const std::filesystem::path& repoDir,
                                           const std::string& mirror);

std::string sync_repo_url(const std::string& url, const std::string& mirror);

// Merge the main index's lua-declared default sub-indexes with the
// persisted / locally-added json list.
//
// Lua defaults are AUTHORITATIVE for their own names: if the official index
// moves a default to a new org/URL, the lua URL wins over a stale json entry
// (which save_sub_repos_json wrote from a previous sync), and the json is
// healed on the next save. The json contributes ONLY names that are NOT lua
// defaults — i.e. user-added sub-indexes (a private fork, or a once-default
// repo that has since been dropped from the lua, like `fromsource`).
//
// This is what lets an org-drifted default (json pinned to the old org) keep
// classifying as default-official so it can migrate to the artifact path; a
// json-overrides-lua merge would leave it stuck on git forever.
std::vector<IndexRepo> merge_sub_repos(const std::vector<IndexRepo>& luaDefaults,
                                       const std::vector<IndexRepo>& jsonRepos);

// Decide whether the MAIN index should be fetched as a versioned artifact (vs
// git clone/pull). The OFFICIAL remote index always converges to artifact in
// auto mode — git is only ever a fallback, never a terminal state:
//   - indexSource "artifact": force artifact (only for the official remote).
//   - indexSource "git":      always git.
//   - indexSource "auto":     artifact whenever the configured main index is the
//     official index from a remote source. This is the symmetric counterpart of
//     the sub-index C1 migration gate: a stranded git main (pkgs/ present, marker
//     lost — e.g. after an interrupted swap or a destructive git fallback) heals
//     itself on the next update instead of being frozen on git forever. Local
//     (file://)/fork/custom-URL mains are excluded by isOfficialRemote, so git
//     fixtures and private indexes are unaffected.
// `hasMarker`/`hasPkgs` are kept in the signature for symmetry/diagnostics and
// future ETag-style gating; auto no longer needs them because an official remote
// always attempts artifact (a redundant attempt on an already-current index is a
// cheap pointer fetch + sha match).
bool main_should_attempt_artifact(bool isOfficialRemote,
                                  const std::string& indexSource,
                                  bool /*hasMarker*/, bool /*hasPkgs*/);

// Decide whether a sub-index should be fetched as a versioned artifact (vs git
// clone/pull). Mirrors the main index's gating:
//   - indexSource "artifact": force artifact for default-official subs and
//     (#377) repos with a declared artifact source.
//   - indexSource "git":      always git.
//   - indexSource "auto":     artifact for a default-official sub that is fresh
//     (no pkgs), already artifact-managed, OR — once the MAIN index is
//     artifact-managed — to MIGRATE an existing git checkout (the whole index
//     is one unit; this is the C1 migration gate so existing installs don't
//     stay split: main on artifact, subs on git). #377: a repo that DECLARES
//     an artifact source always attempts (converges like the main index; the
//     atomic swap migrates an existing git checkout safely, so no C1 gate).
// Repos with neither a default-official identity nor a declared artifact
// source are never artifact-fetched.
bool sub_should_attempt_artifact(bool isDefaultOfficial,
                                 const std::string& indexSource,
                                 bool subManaged, bool subHasPkgs,
                                 bool mainArtifactManaged,
                                 bool hasArtifactSource = false);

// A local repo source (file:// URL or a filesystem path) has no network host,
// so the TCP reachability probe below always reports it "unreachable" and would
// wrongly skip it. Local sources are always reachable — let git clone/pull
// validate the path instead. (Fixes e2e sub-index file:// fixtures being
// "host unreachable within 3000ms" and never synced.)
bool is_local_repo_url(std::string_view u);

// Sync a single git repository (clone or pull)
// Returns true on success
bool sync_repo(const std::filesystem::path& localDir,
               const std::string& url,
               bool force = false);

// #377: sync one CUSTOM repo that declares its own artifact source: artifact
// first (per the repo's effective source mode), then the repo's pre-existing
// git/local path via syncFallback. source:"artifact" hard-fails (no fallback),
// mirroring the main index's XLINGS_INDEX_SOURCE=artifact semantics.
bool sync_repo_with_artifact(const IndexRepo& repo,
                             const std::filesystem::path& repoDir,
                             const std::string& globalIndexSource,
                             const std::function<bool()>& syncFallback);

// Extract repo directory name from URL (matches Lua _to_repodir behavior)
// e.g. "https://github.com/d2learn/xim-pkgindex-d2x.git" -> "xim-pkgindex-d2x"
std::string url_to_dirname(const std::string& url);

// Load sub-repos from the JSON file (written by sync or Lua RepoManager)
std::vector<IndexRepo> load_sub_repos_json(const std::filesystem::path& jsonFile);

// Save sub-repos to JSON file
void save_sub_repos_json(const std::filesystem::path& jsonFile,
                         const std::vector<IndexRepo>& repos);

// Get the sub-repos index directory (global or project-local)
std::filesystem::path sub_repos_dir(bool projectScope = false);

// Get the sub-repos JSON file path
std::filesystem::path sub_repos_json_path(bool projectScope = false);

// Get directory for a sub-repo
std::filesystem::path sub_repo_dir_for(const IndexRepo& repo, bool projectScope = false);

// Return all discovered global sub-repos (for use by PackageCatalog)
std::vector<IndexRepo> discovered_global_sub_repos();

// Return all discovered project-local sub-repos
std::vector<IndexRepo> discovered_project_sub_repos();

// #366: true once the default/global sub-indexes have been synced at least
// once (the persisted xim-indexrepos.json marker exists). On a fresh machine
// this is false even though the main index rebuilds fine, which is exactly why
// get_catalog() must force a first-run sync — otherwise sub-index packages
// (scode/awesome/d2x) never resolve until the user runs `xlings update`.
bool sub_indexes_initialized();

// Get the main index repo directory path (always global)
std::filesystem::path main_repo_dir();

// Sync all configured repos.
// Global repos live under XLINGS_HOME/data, project repos under project .xlings/data.
bool sync_all_repos(bool force = false);

// Read the git HEAD hash for a repo directory.
// Returns empty string for non-git repos or on any error.
std::string get_repo_head_hash(const std::filesystem::path& repoDir);

} // namespace xlings::xim
