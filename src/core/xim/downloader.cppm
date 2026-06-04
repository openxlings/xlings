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

// Clone a git repository, trying the primary URL then mirror fallbacks.
// Each attempt that fails has its partial clone directory removed before
// the next URL is tried.
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
    // don't switch remotes here. Pull failure removes and re-clones,
    // which then goes through the URL-list fallback path below.
    if (fs::exists(destDir / ".git")) {
        log::debug("already cloned {}, pulling latest...", task.name);
        auto pull = compact::git::pull_ff_only(destDir);
        if (pull.rc == 0) {
            result.success = true;
            return result;
        }
        log::warn("pull failed for {}, re-cloning...", task.name);
        fs::remove_all(destDir, ec);
    }

    auto urls = git_candidate_urls_(task);
    for (std::size_t i = 0; i < urls.size(); ++i) {
        const auto& url = urls[i];
        log::debug("cloning {} attempt {}/{}: {}",
                   task.name, i + 1, urls.size(), url);
        auto clone = compact::git::clone_shallow(url, destDir, true);
        if (clone.rc == 0) {
            if (i > 0)
                log::info("[mirror] git clone fallback succeeded via {}", url);
            result.success = true;
            return result;
        }
        // Clean partial clone before next attempt.
        ec.clear();
        fs::remove_all(destDir, ec);
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

            auto urls = git_candidate_urls_(task);
            std::string lastError;
            for (std::size_t i = 0; i < urls.size(); ++i) {
                const auto& url = urls[i];
                log::debug("cloning {} (cancellable) attempt {}/{}: {}",
                           task.name, i + 1, urls.size(), url);
                auto h = compact::git::spawn({
                    "clone", "--depth", "1", "--recursive", "--quiet",
                    url, destDir.string()
                });
                if (h.pid <= 0) { lastError = "failed to spawn git"; continue; }
                auto [code, output] = platform::wait_or_kill(
                    h, cancel, std::chrono::minutes{10});
                if (cancel->is_paused() || cancel->is_cancelled()) {
                    result.error = "cancelled";
                    return result;
                }
                if (code == 0) {
                    if (i > 0)
                        log::info("[mirror] git clone fallback succeeded via {}", url);
                    result.success = true;
                    return result;
                }
                lastError = output;
                std::error_code ec2;
                fs::remove_all(destDir, ec2);
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
        auto cmd = std::format("sha256sum \"{}\"", destFile.string());
        auto [rc, output] = platform::run_command_capture(cmd);
        if (rc == 0 && output.find(task.sha256) != std::string::npos) {
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
        // A stalled host is throttled for us right now — demote it for the
        // rest of the session so later downloads skip straight past it.
        opts.onUrlAttemptFailed = [](const std::string& u, const std::string& err) {
            if (err.rfind("stalled:", 0) == 0) mirror::adaptive::penalize_host(u);
        };

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

    // Verify SHA256 if provided
    if (!task.sha256.empty()) {
        auto shaCmd = std::format("sha256sum \"{}\"", destFile.string());
        auto [shaRc, shaOut] = platform::run_command_capture(shaCmd);
        if (shaRc != 0 || shaOut.find(task.sha256) == std::string::npos) {
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
