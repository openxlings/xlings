module;

#include <cstdio>

export module xlings.core.xim.downloader;

import std;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.log;
import xlings.core.palette;
import xlings.core.compact;
import xlings.platform;
import xlings.core.config;
import xlings.libs.tinyhttps;
import xlings.runtime.cancellation;
import xlings.core.mirror;
import xlings.libs.sha256;
// Re-export extract_archive so existing importers (installer) keep working.
export import xlings.core.xim.extract;

export namespace xlings::xim {

// Check if a URL is a git repository URL
bool is_git_url(const std::string& url);

// Filename-based archive sniff for the post-download sanity check
// below. Kept here (not shared with installer.cppm::is_archive_) so this
// module stays self-contained — the two predicates can diverge if a
// downloader-specific format ever needs filtering.
bool looks_like_archive_filename_(const std::filesystem::path& path);

// Lower bound for what we accept as a real archive payload when there's
// no sha256 to cross-check. Any compressed archive's own format header
// (gzip 10B, xz 12B, zip 22B EOCD, tar 512B block) plus realistic
// content already pushes past 256B; 1 KiB gives us headroom without
// risking false rejection of intentionally-tiny xpkg stub archives.
//
// The motivating failure: gitcode's file-cdn returns 200 OK + a 9-byte
// "Not Found" body (with valid Content-Type / ETag / Content-Disposition
// headers!) for missing release artifacts, and caches that response
// with TTL=1y. Without this check, libarchive eventually fails the
// extract phase but cmd_install historically swallowed the failure as
// exitCode=0. See .agents/docs/2026-05-22-cmd-install-silent-failure-analysis.md
constexpr std::uintmax_t kMinPlausibleArchiveBytes_ = 1024;

// Lowercase a declared sha256 so it compares against our hex digests
// regardless of the recipe author's casing.
std::string lower_hex_(std::string_view s);

// ── Sidecar (.meta) helpers for HEAD-based cache freshness ────────────
//
// When a package recipe omits sha256 (~8% of pkgindex entries declare a
// URL but no checksum), we can't verify a cached file by hash. Instead
// we save the server-reported Last-Modified / ETag next to the file in
// a tiny <name>.meta sidecar and use it on the next install to decide
// whether to reuse the cached payload.
//
// Format: one "key: value" per line, only `last-modified` and `etag`
// recognized. Anything else is ignored. Missing sidecar = no metadata.
struct MetaSidecar_ {
    int format { 1 };
    bool complete { false };
    std::int64_t size { -1 };
    std::string sourceUrl;
    std::string cacheIdentity;
    std::string lastModified;
    std::string etag;
};

std::filesystem::path unique_sibling_path_(
        const std::filesystem::path& base,
        std::string_view marker);

bool commit_staging_file_(const std::filesystem::path& staging,
                          const std::filesystem::path& destination,
                          std::string& error,
                          bool failAfterBackupForTest = false);

bool recover_download_transaction_(
        const std::filesystem::path& destination,
        std::string& error);

std::optional<MetaSidecar_> read_meta_sidecar_(const std::filesystem::path& p);

bool write_meta_sidecar_(const std::filesystem::path& p,
                         const tinyhttps::RemoteFileMeta& meta,
                         std::int64_t size,
                         std::string_view cacheIdentity,
                         std::string_view sourceUrl);

enum class CacheAdmission_ {
    Hit,
    OfflineUnverifiedHit,
    Redownload,
};

struct CacheAdmissionInput_ {
    std::int64_t localSize { -1 };
    bool headSucceeded { false };
    std::int64_t remoteSize { -1 };
    std::string remoteLastModified;
    std::string remoteEtag;
    std::optional<MetaSidecar_> sidecar;
    std::string expectedCacheIdentity;
};

CacheAdmission_ decide_cache_admission_(const CacheAdmissionInput_& input);

struct DownloadTestHooks_ {
    std::function<tinyhttps::DownloadFileResult(
        const std::string&, const std::filesystem::path&)> transferOverride;
    std::function<tinyhttps::RemoteFileMeta(const std::string&)> queryRemoteMeta;
};

tinyhttps::RemoteFileMeta query_remote_meta_(
        const std::string& url,
        const DownloadTestHooks_* hooks);

// Derive the destination directory name from a git URL, e.g.
// "https://github.com/user/repo.git" -> "repo" (or task.name fallback).
std::string git_dest_repo_name_(const std::string& url, const std::string& fallback);

// Build the ordered list of git clone URLs to try: primary + author-
// declared fallbacks + mirror expansions. Mirror::expand handles the
// Mode::Off / non-GitHub passthrough cases internally.
std::vector<std::string> git_candidate_urls_(const DownloadTask& task);

// Atomically replace `live` with `staging`: live -> live.old,
// staging -> live, drop live.old. Any failure restores/keeps the previous
// live tree — a consumer never observes an absent or half-written dir.
// (Root fix for "pkgs/ directory not found in <index>": the old flow
// removed the live clone BEFORE acquiring its replacement, so a failed or
// interrupted re-clone left nothing behind and the error surfaced much
// later at catalog build.)
bool atomic_swap_dir_(const std::filesystem::path& staging,
                      const std::filesystem::path& live);

// Clone a git repository, trying the primary URL then mirror fallbacks.
// Acquisition is STAGED: clones land in a sibling .staging dir and are
// atomically swapped into place only once complete — the live tree is
// never removed first. When a previous copy exists and every URL fails,
// the previous copy is kept (stale-but-consistent beats absent) with a
// loud warning at fetch time.
DownloadResult git_clone_one(const DownloadTask& task);

// Download a single file using libcurl with real-time progress callback.
DownloadResult download_one(const DownloadTask& task,
                            std::function<void(double total, double now)> onProgress = nullptr,
                            CancellationToken* cancel = nullptr,
                            const DownloadTestHooks_* testHooks = nullptr);

// Per-task progress state, shared between download threads and the TUI refresh thread
struct TaskProgress {
    std::string name;
    double totalBytes { 0.0 };
    double downloadedBytes { 0.0 };
    bool started  { false };
    bool finished { false };
    bool success  { false };
};

// Callback for rendering download progress.
// Called from the TUI refresh thread (under mutex) every ~200ms.
// prevLines: number of lines from the previous frame (0 on first call or when
//            rewriting is unsupported). The renderer should move cursor up by
//            prevLines and overwrite in a single write to avoid flicker.
// Returns the number of terminal lines rendered (for the next cursor-up).
using DownloadProgressRenderer = std::function<int(
    std::span<const TaskProgress> state,
    std::size_t nameWidth,
    double elapsedSec,
    bool sizesReady,
    int prevLines)>;

// Download all tasks with limited concurrency, real-time per-task progress
std::vector<DownloadResult>
download_all(std::span<const DownloadTask> tasks,
             const DownloaderConfig& config,
             DownloadProgressRenderer onRender,
             std::function<void(std::string_view name, float progress)> onProgress,
             CancellationToken* cancel = nullptr);

// extract_archive lives in xlings.core.xim.extract (libarchive-backed).
// Re-exported above so existing importers keep working without changes.

} // namespace xlings::xim
