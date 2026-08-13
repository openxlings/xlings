export module xlings.core.xim.indexfetch;

// Index-as-Resource fetch (Y-asset; see
// .agents/docs/2026-06-22-index-as-resource-impl-plan.md).
//
// Instead of `git clone`-ing the package index from GitHub at runtime, fetch a
// versioned index *artifact* (tar.gz + manifest.json) published to xlings-res
// over the SAME resource-server path that package binaries use. This routes
// index acquisition through the in-process HTTP downloader, which already has
// the 0.4.49 adaptive mirror reorder + stall watchdog — closing gap G2 (the
// git-clone path never got that resilience).
//
// Trust/discovery anchor: a fixed rolling release tag ("latest") under the
// xim-index repo on each resource server. The pointer asset
// (xim-index[-<name>]-latest.json) is the entry point; it names the artifact +
// its sha256. The fetch verifies sha256 against the manifest, extracts to a
// temp dir, then atomically renames into place. git clone remains the caller's
// fallback.
//
// Left room for the future X-full upgrade (manifest carries format_version +
// signature; fetch is structured so a signed pointer / ETag-304 can slot in
// without a rewrite).

import std;
import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.core.config;
import xlings.core.mirror;
import xlings.core.xim.extract;

export namespace xlings::xim {

// A client-version constraint an index snapshot declares: [min, max).
// Both optional; empty means unbounded on that side.
//
// Deliberately two bare version strings and NOT a range expression. Two bounds
// already ARE a range, and a second grammar next to semver's would drift from
// it -- while inheriting the defect measured below. See #476 design doc §1.
struct IndexRequirement {
    std::string min;   // inclusive
    std::string max;   // exclusive
    bool empty() const;
};

// One addressable index snapshot: an artifact plus what it asks of its client.
struct IndexSnapshot {
    std::string   index_version;
    std::string   generated_at;
    std::string   artifact_name;
    std::string   artifact_sha256;   // lowercase hex
    std::uint64_t artifact_size = 0;
    // consumer name -> {min,max}. xlings evaluates ONLY the "xlings" key; every
    // other key is carried verbatim for whoever it belongs to.
    nlohmann::json requirements = nlohmann::json::object();
};

struct IndexManifest {
    int           format_version = 0;
    std::string   index_version;
    std::string   index_name;        // "xim" for the main index, else sub name
    std::string   generated_at;
    std::string   source_commit;
    std::string   artifact_name;     // e.g. xim-index-0.4.52.tar.gz
    std::string   artifact_sha256;   // lowercase hex
    std::uint64_t artifact_size = 0;

    // ── #476: version contract + addressable history ──────────────────
    // All optional. A pointer that predates them parses exactly as before,
    // which is why they are ADDED rather than the manifest being restructured:
    // the parser below hard-requires `artifact` at this level, so moving it
    // under a `latest` node would make every released client reject the
    // pointer outright.
    nlohmann::json              requirements = nlohmann::json::object();
    std::vector<IndexSnapshot>  history;            // newest first; [0] == this
    bool                        history_truncated = false;
};

// The requirement `consumer` must meet, or nullopt when unconstrained.
std::optional<IndexRequirement> requirement_for(const nlohmann::json& requirements,
                                                std::string_view consumer);

// Does `selfVersion` satisfy `req`?
//
// Compared with version_order::compare, NOT semver::satisfies_expr. Measured
// on 2026-08-04: semver::Version is three components, so it cannot parse
// xlings's own four-component YYYY.M.D.N at all --
// `satisfies_expr("2026.8.3.2", ">=2026.8.3.1")` returns FALSE. Building the
// contract on it would make every client fail every check and report "no
// compatible snapshot", which reads as a publishing problem rather than a
// parser that does not know the version scheme.
bool satisfies_requirement(std::string_view selfVersion, const IndexRequirement& req);

// Every addressable snapshot of a manifest, newest first. A manifest with no
// history yields exactly one -- itself -- so callers have a single shape.
std::vector<IndexSnapshot> snapshots_of(const IndexManifest& manifest);

struct SnapshotChoice {
    IndexSnapshot snapshot;
    bool          isNewest = true;
    std::string   reason;   // why we stepped back; empty when isNewest
};

// Pick the snapshot this client should use.
//
//   pin empty / "latest" -> newest snapshot whose contract this client meets
//   pin set              -> that exact snapshot, contract check bypassed
//                           (you asked for it) but sha256 still enforced
//
// Returns an error rather than falling back: silently handing back the newest
// snapshot would give the client exactly the thing it was routing away from.
std::expected<SnapshotChoice, std::string> choose_snapshot(
    const IndexManifest& manifest, std::string_view selfVersion, std::string_view pin);

// Parse a manifest JSON. Returns nullopt if required fields are missing/invalid.
std::optional<IndexManifest> parse_index_manifest(std::string_view jsonText);

// #377: a per-repo artifact source derived from IndexRepo.artifactBase.
// Forge bases (github/gitcode host) use raw-file pointer + releases/download
// assets; any other base (plain http(s), file://, local path) is a flat
// directory serving <base>/<filename>. The pointer file is
// `<repoName>-pointers.json` (repoName = last path segment of the base).
struct ArtifactSource {
    std::string base;      // no trailing slash
    std::string server;    // forge only: scheme://host/org ("" => flat/local)
    std::string repoName;  // last path segment of base — pointer file prefix
    std::string key;       // manifest lookup key (config repo name)
    std::optional<std::filesystem::path> localDir;  // set when base is local
    bool forge() const;
};

// Build an ArtifactSource for a repo, or nullopt when it declares none.
std::optional<ArtifactSource> artifact_source_for(const IndexRepo& repo);

// Select the manifest for `key`: exact match; else, for CUSTOM sources only
// (soleEntryFallback), the sole entry — a single-index pointer need not repeat
// the consumer's configured repo name (mcpp publishes key "mcpp", consumers
// configure "mcpplibs"). NEVER sole-fallback for the official combined pointer:
// a missing sub entry there must fail, not silently serve the main index.
const IndexManifest* select_manifest(const std::map<std::string, IndexManifest>& pointers,
                                     std::string_view key, bool soleEntryFallback);

// Build candidate download URLs for an asset filename under the xim-index repo
// on every configured resource server (selected/latency-probed server first),
// then GitHub proxy variants. `mirror` selects region (empty => Config default).
// `version` (e.g. "0.4.58" / "72b00a4") selects the release tag: when given,
// the versioned tag `v<version>` is tried first (GitCode only serves assets via
// the versioned tag, not the rolling `latest`), with `latest` kept as fallback.
// #377: `custom` overrides the official server+repo with a per-repo source.
std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror = {},
                                          std::string_view version = {},
                                          const ArtifactSource* custom = nullptr);

// Raw-file URLs for the rolling pointer (see definition). #377: `custom`
// targets a per-repo source instead of the official xim-index repo.
std::vector<std::string> index_pointer_urls(std::string_view filename,
                                            std::string_view mirror,
                                            const ArtifactSource* custom = nullptr);

// Cached fetch of a combined pointer file: one raw fetch per process PER BASE
// ("" = the official xim-index-pointers.json; a custom source's base fetches
// `<repoName>-pointers.json` from that base).
const std::map<std::string, IndexManifest>& load_index_pointers(std::string_view mirror,
                                            const ArtifactSource* custom = nullptr);

// Fetch the latest index artifact for `subName` ("" = main index) and atomically
// install it into destIndexDir. Returns true on success; on failure `err` is set
// and destIndexDir is left untouched (caller may fall back to git).
// #377: `custom` fetches from a per-repo artifact source (pointer key =
// custom->key with sole-entry fallback) instead of the official pointer.
bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName = {},
                          const ArtifactSource* custom = nullptr,
                          std::string_view pin = {});

// Reconcile leftover index temp dirs from crashed / SIGKILL'd runs: restore an
// index dir orphaned by an interrupted swap (`<base>.old.<deadpid>` holding
// pkgs/) and sweep leaked `.artifact.*` / `.tmp.*` / spent `.old.*` staging from
// dead processes. Dirs owned by a live process are left untouched. Idempotent;
// call before any git-vs-artifact gating so a transient gap can't trigger a
// destructive git clone. See 2026-06-30 regression analysis.
void reconcile_index_temps(const std::filesystem::path& dataDir);

} // namespace xlings::xim


namespace xlings::xim {

namespace detail_ {

// Index source base override (env now; config key folded in by the caller via
// Config). When set to a local dir / file:// it's copied; a remote base means
// "<base>/<filename>". Empty => use the passed remoteUrls (xlings-res default).
struct BaseOverride {
    std::string base;                      // remote base, or empty
    std::optional<std::filesystem::path> local;  // local dir, if base is local
};

} // namespace detail_

std::optional<IndexManifest> parse_index_manifest(std::string_view jsonText);

std::optional<IndexRequirement> requirement_for(const nlohmann::json& requirements,
                                                std::string_view consumer);

bool satisfies_requirement(std::string_view selfVersion, const IndexRequirement& req);

std::vector<IndexSnapshot> snapshots_of(const IndexManifest& manifest);

namespace detail_ {

}  // namespace detail_

std::expected<SnapshotChoice, std::string> choose_snapshot(
    const IndexManifest& manifest, std::string_view selfVersion, std::string_view pin);

std::optional<ArtifactSource> artifact_source_for(const IndexRepo& repo);

const IndexManifest* select_manifest(const std::map<std::string, IndexManifest>& pointers,
                                     std::string_view key, bool soleEntryFallback);

std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror,
                                          std::string_view version,
                                          const ArtifactSource* custom);

// Raw-file URLs for the rolling pointer (a file committed to the index repo,
// updated by git push, served raw). github.com/<org> ->
// raw.githubusercontent.com/<org>/<repo>/main/<f>; gitcode.com/<org> ->
// raw.gitcode.com/<org>/<repo>/raw/main/<f>. Region-ordered + GLOBAL fallback.
std::vector<std::string> index_pointer_urls(std::string_view filename,
                                            std::string_view mirror,
                                            const ArtifactSource* custom);

namespace detail_ {
// base -> consumer -> newest version, filled by load_index_pointers().
inline std::map<std::string, std::map<std::string, std::string>>& client_latest_cache_() {
    static std::map<std::string, std::map<std::string, std::string>> value;
    return value;
}
}

// Cached fetch of the COMBINED pointer file (xim-index-pointers.json): ONE raw
// fetch per process covering ALL indexes (main + subs). One fetch (vs one per
// index) avoids gitcode raw rate-limiting, and is the single file a self-hosted
// index server must serve. Format:
//   {"format_version":1,"indexes":{"xim":{<manifest>},"awesome":{<manifest>},...}}
const std::map<std::string, IndexManifest>& load_index_pointers(std::string_view mirror,
                                                                const ArtifactSource* custom);

bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName,
                          const ArtifactSource* custom,
                          std::string_view pin);

// Reconcile leftover index temp dirs from crashed / SIGKILL'd runs. Two jobs:
//   1) RECOVER: if a live index dir is missing or empty but a sibling
//      `<base>.old.<pid>` (from an interrupted artifact swap) still holds pkgs/,
//      move it back into place — BEFORE any caller mistakes the gap for "no
//      index" and triggers a destructive git clone (the regression root cause).
//   2) SWEEP: delete `<base>.artifact.<pid>` / `.tmp.<pid>` / spent `.old.<pid>`
//      staging dirs whose owning process is dead. The RAII cleanup in
//      fetch_index_artifact / sync_repo never runs on SIGKILL, so these
//      otherwise accumulate without bound (10+ observed in the wild).
// Dirs still owned by a live process are left untouched (concurrency-safe).
// See .agents/docs/2026-06-30-index-artifact-git-regression-analysis.md.
void reconcile_index_temps(const std::filesystem::path& dataDir);

} // namespace xlings::xim
