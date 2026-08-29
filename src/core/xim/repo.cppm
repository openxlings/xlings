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

// Strip a trailing `.git` and trailing slashes. Scheme and host are left
// alone on purpose — a mirror is a different source.
std::string normalize_repo_url_(std::string_view url);

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

// ── index_repos entries are PEERS ────────────────────────────────────
//
// There used to be two gates here — main_should_attempt_artifact and
// sub_should_attempt_artifact — because the sync split its work into "the main
// index" and "the others". The main one was index_repos[0], and it was fetched
// with a HARDCODED pointer key into a directory derived from that entry's name,
// so the destination and the content were chosen independently. A sub-index
// listed first therefore received the official index: data/scode held 185
// packages of xim and served every one of them under the `scode` namespace.
//
// Both gates are gone. Every entry — top-level or discovered sub-index — goes
// through sync_one_repo under its OWN name, and whether it may be served an
// artifact is a configuration question (artifact_is_declared_for), not a URL
// pattern compiled into xlings.

// Which entry of a published pointer describes this repo's tree. It is the
// repo's own name (or, for a repo that declares its own artifact base, that
// source's key — which is also derived from the repo). One origin, one answer.
std::string index_pointer_key(const IndexRepo& repo);

// Do these two URLs name the same repository? Compared after stripping a
// trailing `.git` and trailing slashes: a config that omits `.git` means the
// same repo, and reading it as a different one would silently drop the entry to
// git with no message.
bool url_matches_declared_source(std::string_view repoUrl,
                                 std::string_view declaredUrl);

// The URL the default index declares for a sub-index name, via its
// xim-indexrepos.lua. Empty when the name is not declared (a user-added
// sub-index, or the default index is not on disk yet).
std::string declared_sub_index_url(std::string_view name);

// May this repo be served the artifact published under its name?
//
// Configuration decides, and only configuration:
//   - it declares its own artifact base (#377) — that IS its declared source;
//   - or its url equals the source declared for its name: xim.index-repo for
//     the default index, the default index's xim-indexrepos.lua for a
//     sub-index.
// A local (file://) source is never artifact-fetched, and a name nothing
// declares — a private index, a fork — simply has no artifact and uses git.
//
// This replaces `url.find("openxlings/xim-pkgindex") != npos`, which matched
// every official SUB-index URL too, because they all carry it as a prefix.
bool artifact_is_declared_for(const IndexRepo& repo, bool projectScope);

// Sync ONE index repo into `repoDir`: artifact when one is declared for it and
// a pointer describes it, else git/local. This is the whole peer model — the
// directory, the namespace and the pointer key all come from `repo.name`, so a
// repo can only ever receive its OWN index.
//
// `linkLocalSource` picks how a local (file:// / path) source is materialised,
// and the two callers differ — an asymmetry that predates this function:
//   - top-level index_repos: SYMLINK to the source (ensure_local_repo_link_).
//   - sub-indexes:           git clone, i.e. a SNAPSHOT.
// The difference is user-visible, not cosmetic: a symlinked index tracks its
// source live, so the on-demand refresh (C2, #366) has nothing to refresh and
// never announces itself. Unifying it is a separate decision from this one, so
// each caller keeps what it had.
bool sync_one_repo(const IndexRepo& repo,
                   const std::filesystem::path& repoDir,
                   const std::string& globalIndexSource,
                   const std::string& mirror,
                   bool projectScope,
                   bool force,
                   bool linkLocalSource);

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

// Where the DEFAULT index lives (always global).
//
// Not a lookup — a constant. The default index is the entry named
// Config::DEFAULT_INDEX_REPO_NAME (Config guarantees one exists), and
// repo_dir_for maps that name onto DEFAULT_INDEX_REPO_DIR unconditionally, so
// no config a user can write moves it.
//
// It used to be repo_dir_for(index_repos[0]) — position, not identity. That is
// how `xlings index use xim <ver>`, which materialises the default entry at the
// END of the array, silently handed the default index's identity (its
// directory, its pin, and the xim-indexrepos.lua every sub-index is discovered
// from) to whatever happened to be first.
//
// Only sub-index discovery still asks: xim-indexrepos.lua ships in the default
// index and nowhere else.
std::filesystem::path main_repo_dir();

// Sync all configured repos.
// Global repos live under XLINGS_HOME/data, project repos under project .xlings/data.
bool sync_all_repos(bool force = false);

// Read the git HEAD hash for a repo directory.
// Returns empty string for non-git repos or on any error.
//
// Two shapes, one answer. A git working tree yields the sha; an
// artifact-managed index (no `.git` at all -- the shape a packaged index
// install produces) yields `artifact:<version>` off `.xlings-index-version`.
// Both are stable identities for "which snapshot answered", which is why
// there must not be a second reader that only knows one of them.
std::string get_repo_head_hash(const std::filesystem::path& repoDir);

// The same identity, shortened for a message: a 40-char sha becomes 7 chars,
// an `artifact:<version>` is already short and is returned whole.
std::string get_repo_revision_label(const std::filesystem::path& repoDir);

// How long ago this index was last synced, in seconds. -1 when unknown.
//
// The revision answers "which snapshot"; this answers "how stale", and only
// the second one can decide whether `xlings update` is useful advice. Told to
// someone who synced thirty seconds ago it is the noise this whole message
// was rewritten to remove.
long long get_repo_sync_age_seconds(const std::filesystem::path& repoDir);

} // namespace xlings::xim
