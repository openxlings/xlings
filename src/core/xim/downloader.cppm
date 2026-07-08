module;

#include <cstdio>

export module xlings.core.xim.downloader;

import std;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.log;
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
bool is_git_url(const std::string& url) {
    return url.ends_with(".git");
}

// Filename-based archive sniff for the post-download sanity check
// below. Kept here (not shared with installer.cppm::is_archive_) so this
// module stays self-contained — the two predicates can diverge if a
// downloader-specific format ever needs filtering.
bool looks_like_archive_filename_(const std::filesystem::path& path) {
    auto name = path.filename().string();
    return name.ends_with(".tar.gz")
        || name.ends_with(".tar.xz")
        || name.ends_with(".tar.bz2")
        || name.ends_with(".tar.zst")
        || name.ends_with(".tgz")
        || name.ends_with(".zip");
}

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
std::string lower_hex_(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

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
    std::string lastModified;
    std::string etag;
};

std::optional<MetaSidecar_> read_meta_sidecar_(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) return std::nullopt;
    MetaSidecar_ m;
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // trim
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
            while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '\r')) s.pop_back();
        };
        trim(key); trim(val);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "last-modified") m.lastModified = std::move(val);
        else if (key == "etag")     m.etag = std::move(val);
    }
    return m;
}

void write_meta_sidecar_(const std::filesystem::path& p,
                         const tinyhttps::RemoteFileMeta& meta) {
    std::ofstream out(p, std::ios::trunc);
    if (!out) return;
    if (!meta.lastModified.empty()) out << "last-modified: " << meta.lastModified << "\n";
    if (!meta.etag.empty())         out << "etag: "          << meta.etag         << "\n";
}

// Derive the destination directory name from a git URL, e.g.
// "https://github.com/user/repo.git" -> "repo" (or task.name fallback).
std::string git_dest_repo_name_(const std::string& url, const std::string& fallback) {
    std::string repoName;
    auto lastSlash = url.rfind('/');
    if (lastSlash != std::string::npos) {
        repoName = url.substr(lastSlash + 1);
        if (repoName.ends_with(".git"))
            repoName = repoName.substr(0, repoName.size() - 4);
    }
    return repoName.empty() ? fallback : repoName;
}

// Build the ordered list of git clone URLs to try: primary + author-
// declared fallbacks + mirror expansions. Mirror::expand handles the
// Mode::Off / non-GitHub passthrough cases internally.
std::vector<std::string> git_candidate_urls_(const DownloadTask& task) {
    std::vector<std::string> urls;
    urls.push_back(task.url);
    for (auto& fb : task.fallbackUrls) urls.push_back(fb);

    auto mirrored = mirror::expand(task.url, {.type = mirror::ResourceType::Git});
    for (auto& u : mirrored) {
        if (std::ranges::find(urls, u) == urls.end())
            urls.push_back(std::move(u));
    }
    return urls;
}

// Atomically replace `live` with `staging`: live -> live.old,
// staging -> live, drop live.old. Any failure restores/keeps the previous
// live tree — a consumer never observes an absent or half-written dir.
// (Root fix for "pkgs/ directory not found in <index>": the old flow
// removed the live clone BEFORE acquiring its replacement, so a failed or
// interrupted re-clone left nothing behind and the error surfaced much
// later at catalog build.)
bool atomic_swap_dir_(const std::filesystem::path& staging,
                      const std::filesystem::path& live) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto old = live.parent_path() / (live.filename().string() + ".old");
    fs::remove_all(old, ec);          // crashed-run leftovers
    ec.clear();
    if (fs::exists(live)) {
        fs::rename(live, old, ec);
        if (ec) return false;
    }
    fs::rename(staging, live, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(old, live, ec2);   // best-effort restore
        return false;
    }
    fs::remove_all(old, ec);
    return true;
}

// Clone a git repository, trying the primary URL then mirror fallbacks.
// Acquisition is STAGED: clones land in a sibling .staging dir and are
// atomically swapped into place only once complete — the live tree is
// never removed first. When a previous copy exists and every URL fails,
// the previous copy is kept (stale-but-consistent beats absent) with a
// loud warning at fetch time.
DownloadResult git_clone_one(const DownloadTask& task) {
    namespace fs = std::filesystem;

    DownloadResult result;
    result.name = task.name;

    std::error_code ec;
    fs::create_directories(task.destDir, ec);
    if (ec) {
        result.error = std::format("failed to create directory {}: {}",
                                   task.destDir.string(), ec.message());
        return result;
    }

    auto repoName = git_dest_repo_name_(task.url, task.name);
    auto destDir = task.destDir / repoName;
    result.localFile = destDir;

    // If already cloned, pull latest. Pull is single-URL by design — we
    // don't switch remotes here. Pull failure falls through to a STAGED
    // re-clone; the live tree stays untouched until a replacement is ready.
    const bool hadLive = fs::exists(destDir / ".git");
    if (hadLive) {
        log::debug("already cloned {}, pulling latest...", task.name);
        auto pull = compact::git::pull_ff_only(destDir);
        if (pull.rc == 0) {
            result.success = true;
            return result;
        }
        log::warn("pull failed for {}, re-cloning into staging...", task.name);
    }

    auto staging = task.destDir / (".staging-" + repoName);
    ec.clear();
    fs::remove_all(staging, ec);      // crashed-run leftovers

    auto urls = git_candidate_urls_(task);
    for (std::size_t i = 0; i < urls.size(); ++i) {
        const auto& url = urls[i];
        log::debug("cloning {} attempt {}/{}: {}",
                   task.name, i + 1, urls.size(), url);
        auto clone = compact::git::clone_shallow(url, staging, true);
        if (clone.rc == 0 && fs::exists(staging / ".git")) {
            if (i > 0)
                log::info("[mirror] git clone fallback succeeded via {}", url);
            if (atomic_swap_dir_(staging, destDir)) {
                result.success = true;
                return result;
            }
            log::warn("atomic swap failed for {}", task.name);
        }
        // Clean partial staging clone before next attempt.
        ec.clear();
        fs::remove_all(staging, ec);
    }

    if (hadLive) {
        // Every URL failed but a previous copy exists: keep serving it.
        log::warn("{}: refresh failed on all URLs; keeping the previous copy",
                  task.name);
        result.success = true;
        return result;
    }
    result.error = std::format("all git clone URLs failed for {}", task.name);
    return result;
}

// Download a single file using libcurl with real-time progress callback.
DownloadResult download_one(const DownloadTask& task,
                            std::function<void(double total, double now)> onProgress = nullptr,
                            CancellationToken* cancel = nullptr) {
    namespace fs = std::filesystem;

    DownloadResult result;
    result.name = task.name;

    // Ensure dest directory exists
    std::error_code ec;
    fs::create_directories(task.destDir, ec);
    if (ec) {
        result.error = std::format("failed to create directory {}: {}",
                                   task.destDir.string(), ec.message());
        return result;
    }

    // Git clone for .git URLs. The non-cancellable path delegates to
    // git_clone_one which already handles mirror fallback; the cancellable
    // path needs the same fallback wiring inline because it uses
    // spawn_command/wait_or_kill instead of blocking exec.
    if (is_git_url(task.url)) {
        if (cancel) {
            namespace fs = std::filesystem;
            auto repoName = git_dest_repo_name_(task.url, task.name);
            auto destDir = task.destDir / repoName;
            result.localFile = destDir;

            // Staged acquisition, mirroring git_clone_one: clone into a
            // sibling .staging dir, atomic-swap on success, and keep any
            // previous live copy when every URL fails (or on cancel).
            const bool hadLive = fs::exists(destDir / ".git");
            auto staging = task.destDir / (".staging-" + repoName);
            std::error_code ecs;
            fs::remove_all(staging, ecs);

            auto urls = git_candidate_urls_(task);
            std::string lastError;
            for (std::size_t i = 0; i < urls.size(); ++i) {
                const auto& url = urls[i];
                log::debug("cloning {} (cancellable) attempt {}/{}: {}",
                           task.name, i + 1, urls.size(), url);
                auto h = compact::git::spawn({
                    "clone", "--depth", "1", "--recursive", "--quiet",
                    url, staging.string()
                });
                if (h.pid <= 0) { lastError = "failed to spawn git"; continue; }
                auto [code, output] = platform::wait_or_kill(
                    h, cancel, std::chrono::minutes{10});
                if (cancel->is_paused() || cancel->is_cancelled()) {
                    fs::remove_all(staging, ecs);
                    result.error = "cancelled";
                    return result;
                }
                if (code == 0 && fs::exists(staging / ".git")
                    && atomic_swap_dir_(staging, destDir)) {
                    if (i > 0)
                        log::info("[mirror] git clone fallback succeeded via {}", url);
                    result.success = true;
                    return result;
                }
                lastError = output;
                fs::remove_all(staging, ecs);
            }
            if (hadLive) {
                log::warn("{}: refresh failed on all URLs; keeping the previous copy",
                          task.name);
                result.success = true;
                return result;
            }
            result.error = lastError.empty()
                ? std::format("all git clone URLs failed for {}", task.name)
                : lastError;
            return result;
        }
        return git_clone_one(task);
    }

    log::debug("downloading {} from {}", task.name, task.url);

    // Extract filename from URL
    std::string url = task.url;
    std::string filename;
    auto lastSlash = url.rfind('/');
    if (lastSlash != std::string::npos) {
        filename = url.substr(lastSlash + 1);
        auto q = filename.find('?');
        if (q != std::string::npos) filename = filename.substr(0, q);
    }
    if (filename.empty()) filename = task.name + ".download";

    auto destFile = task.destDir / filename;
    auto sidecarPath = destFile;
    sidecarPath += ".meta";
    result.localFile = destFile;

    // ── Cache hit path 1: sha256 verified (cheapest, most reliable) ──
    // If the recipe declares a sha256 and the on-disk file matches, we're
    // byte-identical to upstream — skip download outright.
    if (fs::exists(destFile) && !task.sha256.empty()) {
        // In-process hash — `sha256sum` is a coreutils tool absent on
        // stock macOS, where shelling out made every pinned download
        // "mismatch" (see xlings.libs.sha256 header).
        auto digest = sha256::hex_file(destFile);
        if (digest && *digest == lower_hex_(task.sha256)) {
            log::debug("already downloaded (sha256): {}", destFile.string());
            result.success = true;
            return result;
        }
        // sha mismatch: stale/corrupt cache. Remove before falling through
        // so the next download_to_file starts from a clean slate (defends
        // against a future tinyhttps that might enable Range/resume).
        fs::remove(destFile, ec);
    }

    // ── Cache hit path 2: HEAD-based freshness (when sha256 is unset) ──
    // Trade a tiny HEAD round-trip for not re-downloading toolchain-sized
    // payloads on every `xlings install`. We only consult this when the
    // recipe omits sha256 — otherwise path 1 already decided.
    tinyhttps::RemoteFileMeta probedMeta;
    bool probedMetaValid = false;
    if (fs::exists(destFile) && task.sha256.empty()) {
        probedMeta = tinyhttps::query_remote_meta(task.url);
        probedMetaValid = true;

        if (probedMeta.ok) {
            std::error_code sec;
            auto localSize = static_cast<std::int64_t>(fs::file_size(destFile, sec));
            std::string storedLM, storedETag;
            if (auto stored = read_meta_sidecar_(sidecarPath)) {
                storedLM   = std::move(stored->lastModified);
                storedETag = std::move(stored->etag);
            }
            bool sizeMatch = (probedMeta.contentLength > 0)
                          && (localSize == probedMeta.contentLength);
            // Strong freshness signal: server's Last-Modified or ETag
            // matches what we recorded the last time we downloaded.
            bool freshMatch =
                (!probedMeta.lastModified.empty() && probedMeta.lastModified == storedLM)
             || (!probedMeta.etag.empty()         && probedMeta.etag         == storedETag);
            // Weak signal: no sidecar (legacy file from before this code),
            // but the size matches what the server reports right now.
            bool weakMatch = sizeMatch && storedLM.empty() && storedETag.empty();

            if (sizeMatch && (freshMatch || weakMatch)) {
                log::debug("already downloaded (HEAD cache hit, size={}): {}",
                           localSize, destFile.string());
                result.success = true;
                return result;
            }
            // Stale: drop both file and sidecar before re-downloading.
            fs::remove(destFile, ec);
            fs::remove(sidecarPath, ec);
        } else {
            // HEAD failed (offline, server blocks HEAD, 4xx, etc.). If we
            // already have a non-empty cached file, prefer it over failing
            // — being airline-friendly is worth the small risk of serving
            // a stale payload when sha256 is unset.
            std::error_code sec;
            auto localSize = fs::file_size(destFile, sec);
            if (!sec && localSize > 0) {
                log::warn("HEAD probe failed for {} ({}); using cached file: {}",
                          task.url,
                          probedMeta.error.empty() ? "unknown" : probedMeta.error,
                          destFile.string());
                result.success = true;
                return result;
            }
        }
    }

    // Build ordered list of URLs to try:
    //   1. primary URL (as-is)
    //   2. package-author-declared fallback URLs (already mirror-selected
    //      by upstream resolver)
    //   3. github mirror fallbacks (only appended if URL is github.com/
    //      raw.githubusercontent.com/etc and mirror mode != Off)
    std::vector<std::string> urls;
    urls.push_back(url);
    for (auto& fb : task.fallbackUrls) urls.push_back(fb);

    auto mirrored = mirror::expand(url);
    for (auto& u : mirrored) {
        if (std::ranges::find(urls, u) == urls.end())
            urls.push_back(std::move(u));
    }

    // Adaptive ordering (0.4.49): probe candidate hosts' connect latency
    // (memoized per process) and order accordingly. With a declared sha256
    // the bytes are pinned, so faster mirrors may be tried before the
    // original URL; without one the author-declared URL stays first unless
    // its host is unreachable/penalized. See
    // .agents/docs/2026-06-04-github-asset-adaptive-mirror.md.
    urls = mirror::adaptive::reorder(std::move(urls), !task.sha256.empty());

    // Use in-process tinyhttps for all downloads (streaming progress).
    // When a CancellationToken is available, wire isCancelled so ESC aborts.
    {
        tinyhttps::DownloadOptions opts;
        opts.destFile = destFile;
        opts.urls = std::move(urls);
        opts.retryCount = 3;
        opts.connectTimeoutSec = 30;
        opts.maxTimeSec = 600;
        opts.onProgress = onProgress;
        if (cancel) {
            opts.isCancelled = [cancel] { return cancel->is_paused() || cancel->is_cancelled(); };
        }
        // A stalled host is throttled for us right now, and a host that
        // served bytes failing the integrity check is worse — demote both
        // for the rest of the session so later downloads skip them.
        opts.onUrlAttemptFailed = [](const std::string& u, const std::string& err) {
            if (err.rfind("stalled:", 0) == 0
                || err.rfind("sha256 mismatch", 0) == 0)
                mirror::adaptive::penalize_host(u);
        };
        // Per-candidate integrity acceptance: verify INSIDE the URL loop
        // so a mirror that wins the latency race but serves corrupted
        // bytes is rejected and the next candidate (ultimately the
        // author URL) is tried — previously a single mismatch failed the
        // whole download with the remaining candidates untried.
        if (!task.sha256.empty()) {
            auto want = lower_hex_(task.sha256);
            opts.onVerify = [destFile, want, &task](const std::string& u)
                -> std::string {
                auto digest = sha256::hex_file(destFile);
                if (digest && *digest == want) return {};
                return std::format(
                    "sha256 mismatch for {} (source {}): got {}, want {}",
                    task.name, u, digest ? *digest : "<unreadable>", want);
            };
        }

        auto dlResult = tinyhttps::download_file(opts);
        if (!dlResult.success) {
            result.error = dlResult.error;
            return result;
        }
    }

    // Size sanity check for archives without sha256. When a recipe
    // omits sha256 we have no way to cross-check content authenticity,
    // so a CDN serving 200 OK + a tiny error stub (e.g., gitcode's
    // 9-byte "Not Found" body for missing release artifacts) would
    // otherwise propagate to extract and fail there. Reject early so
    // the error message points at the actual problem instead of a
    // misleading libarchive "unrecognized format". Skip when sha256
    // is declared — that's a stronger check than size alone.
    if (task.sha256.empty() && looks_like_archive_filename_(destFile)) {
        auto sz = fs::file_size(destFile, ec);
        if (!ec && sz < kMinPlausibleArchiveBytes_) {
            result.error = std::format(
                "{}: downloaded payload is only {} bytes — likely an "
                "error stub returned as 200 OK (no sha256 declared to "
                "cross-check). URL: {}",
                task.name, sz, task.url);
            fs::remove(destFile, ec);
            return result;
        }
    }

    // Final SHA256 re-check (defense in depth — the per-candidate
    // onVerify above already gated acceptance; in-process hash, no
    // dependency on a host `sha256sum` binary, which stock macOS lacks).
    if (!task.sha256.empty()) {
        auto digest = sha256::hex_file(destFile);
        if (!digest || *digest != lower_hex_(task.sha256)) {
            result.error = std::format("SHA256 mismatch for {}", task.name);
            fs::remove(destFile, ec);
            return result;
        }
    } else {
        // No sha256 declared: persist server-reported Last-Modified / ETag
        // alongside the payload so the next install can use a HEAD probe
        // to decide cache freshness instead of re-downloading.
        if (!probedMetaValid) {
            probedMeta = tinyhttps::query_remote_meta(task.url);
            probedMetaValid = true;
        }
        if (probedMeta.ok && (!probedMeta.lastModified.empty() || !probedMeta.etag.empty())) {
            write_meta_sidecar_(sidecarPath, probedMeta);
        }
    }

    result.success = true;
    return result;
}

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
             CancellationToken* cancel = nullptr) {

    if (tasks.empty()) return {};

    tinyhttps::global_init();

    std::vector<DownloadResult> results(tasks.size());
    std::mutex mutex;
    std::condition_variable cv;
    int activeCount = 0;
    int maxConcur = std::max(1, config.maxConcurrency);

    // Shared progress state for TUI
    std::vector<TaskProgress> progState(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i)
        progState[i].name = tasks[i].name;

    std::atomic<bool> allDone { false };
    std::atomic<bool> sizesReady { false };

    // Compute max name width for alignment
    std::size_t nameWidth = 20;
    for (auto& t : tasks) {
        if (t.name.size() > nameWidth) nameWidth = t.name.size();
    }
    nameWidth += 2; // padding

    // Background HEAD requests to pre-fetch Content-Length for byte-weighted progress.
    // Runs in parallel with downloads — does not block startup.
    std::jthread headThread([&]() {
        std::vector<std::jthread> headWorkers;
        headWorkers.reserve(tasks.size());
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            if (is_git_url(tasks[i].url)) continue;
            headWorkers.emplace_back([&, i]() {
                auto len = tinyhttps::query_content_length(tasks[i].url);
                if (len > 0) {
                    std::lock_guard lock(mutex);
                    // Only set if download hasn't already reported a size
                    if (progState[i].totalBytes <= 0.0)
                        progState[i].totalBytes = static_cast<double>(len);
                }
            });
        }
        // jthread destructors join all HEAD workers
        headWorkers.clear();
        sizesReady.store(true);
    });

    // TUI refresh thread: redraws progress every 200ms using FTXUI.
    // Uses relative cursor movement (\033[<N>A) to overwrite previous frame in-place.
    auto startTime = std::chrono::steady_clock::now();

    bool canRewrite = platform::supports_rewrite_output() && !platform::is_tui_mode();
    int lastLines = 0;  // lines rendered in previous frame (for cursor-up)

    std::jthread tuiThread([&](std::stop_token stoken) {
        if (!onRender) return;  // No renderer — skip TUI

        if (canRewrite) {
            // Hide cursor during download (CLI mode only)
            std::print("\033[?25l");
            std::fflush(stdout);
        }

        while (!stoken.stop_requested() && !allDone.load() &&
               !(cancel && (cancel->is_paused() || cancel->is_cancelled()))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            auto elapsed = std::chrono::steady_clock::now() - startTime;
            auto elapsedSec = std::chrono::duration<double>(elapsed).count();
            {
                std::lock_guard lock(mutex);
                lastLines = onRender(progState, nameWidth, elapsedSec,
                                     sizesReady.load(), canRewrite ? lastLines : 0);
            }
        }

        // Final render
        {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            auto elapsedSec = std::chrono::duration<double>(elapsed).count();
            std::lock_guard lock(mutex);
            onRender(progState, nameWidth, elapsedSec,
                     sizesReady.load(), canRewrite ? lastLines : 0);
        }
        if (canRewrite) {
            // Show cursor again
            std::print("\033[?25h");
            std::fflush(stdout);
        }
    });

    std::vector<std::jthread> threads;
    threads.reserve(tasks.size());

    for (std::size_t i = 0; i < tasks.size(); ++i) {
        threads.emplace_back([&, i]() {
            // Wait for concurrency slot
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] {
                    return activeCount < maxConcur || (cancel && (cancel->is_paused() || cancel->is_cancelled()));
                });
                if (cancel && (cancel->is_paused() || cancel->is_cancelled())) {
                    std::lock_guard lk(mutex);
                    results[i].name = tasks[i].name;
                    results[i].error = "cancelled";
                    progState[i].finished = true;
                    cv.notify_one();
                    return;
                }
                ++activeCount;
                progState[i].started = true;
            }

            if (onProgress) {
                onProgress(tasks[i].name, 0.0f);
            }

            // Per-task progress callback updates shared state
            auto taskProgress = [&](double total, double now) {
                std::lock_guard lock(mutex);
                progState[i].totalBytes = total;
                progState[i].downloadedBytes = now;
            };

            auto result = download_one(tasks[i], taskProgress, cancel);

            {
                std::lock_guard lock(mutex);
                progState[i].finished = true;
                progState[i].success = result.success;
                results[i] = std::move(result);
                --activeCount;
            }

            if (onProgress) {
                onProgress(tasks[i].name, results[i].success ? 1.0f : -1.0f);
            }

            cv.notify_one();
        });
    }

    // jthread destructor joins automatically
    threads.clear();
    allDone.store(true);

    // Stop TUI thread
    tuiThread.request_stop();
    tuiThread.join();

    tinyhttps::global_cleanup();
    return results;
}

// extract_archive lives in xlings.core.xim.extract (libarchive-backed).
// Re-exported above so existing importers keep working without changes.

} // namespace xlings::xim
