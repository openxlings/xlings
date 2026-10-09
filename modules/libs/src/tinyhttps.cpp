module xlings.libs.tinyhttps;

import std;
import mcpplibs.tinyhttps;

namespace xlings::tinyhttps {

namespace detail_ {

// Which proxy the most recent client was built with, or empty.
//
// Diagnostic only, and deliberately a record rather than a log line: this
// module is a workspace member now and members may not depend on the root
// package, where `log` lives. Whoever cares reports it in their own voice.
std::string gLastProxy_;


std::string url_host_(std::string_view url) {
    auto s = std::string{url};
    if (auto p = s.find("://"); p != std::string::npos) s = s.substr(p + 3);
    if (auto p = s.find('/'); p != std::string::npos) s = s.substr(0, p);
    if (auto p = s.rfind(':'); p != std::string::npos) {
        // Don't lop off a colon that's actually IPv6 part — but proxy-via-env
        // for IPv6 is enough of an edge case to ignore here.
        s = s.substr(0, p);
    }
    return s;
}

bool host_in_no_proxy_(std::string_view host, std::string_view np) {
    std::size_t i = 0;
    while (i < np.size()) {
        auto end = np.find(',', i);
        auto entry = np.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
        i = (end == std::string_view::npos) ? np.size() : end + 1;
        // trim spaces
        while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.remove_prefix(1);
        while (!entry.empty() && (entry.back()  == ' ' || entry.back()  == '\t')) entry.remove_suffix(1);
        if (entry.empty()) continue;
        if (entry == "*") return true;
        if (entry.front() == '.') entry.remove_prefix(1);
        if (host == entry) return true;
        if (host.size() > entry.size()
            && host[host.size() - entry.size() - 1] == '.'
            && host.substr(host.size() - entry.size()) == entry) {
            return true;
        }
    }
    return false;
}

std::string env_proxy_for_(std::string_view url) {
    // Two-name lookup: try uppercase first, then lowercase. Avoids the
    // GNU `?:` binary-conditional extension (MSVC rejects it).
    auto get_either = [](const char* a, const char* b) -> const char* {
        if (auto v = std::getenv(a); v && *v) return v;
        if (auto v = std::getenv(b); v && *v) return v;
        return nullptr;
    };

    // NO_PROXY exemption first.
    if (auto np = get_either("NO_PROXY", "no_proxy")) {
        if (host_in_no_proxy_(url_host_(url), np)) return {};
    }

    bool isHttps = url.starts_with("https://");
    if (isHttps) {
        if (auto p = get_either("HTTPS_PROXY", "https_proxy")) return p;
    } else {
        if (auto p = get_either("HTTP_PROXY", "http_proxy")) return p;
    }
    if (auto p = get_either("ALL_PROXY", "all_proxy")) return p;
    return {};
}

auto make_client(int connectTimeoutSec, int readTimeoutSec, std::string_view url)
    -> mcpplibs::tinyhttps::HttpClient {
    mcpplibs::tinyhttps::HttpClientConfig cfg;
    cfg.connectTimeoutMs = connectTimeoutSec * 1000;
    cfg.readTimeoutMs = readTimeoutSec * 1000;
    cfg.verifySsl = true;
    cfg.keepAlive = false;
    cfg.maxRedirects = 10;
    if (auto proxy = env_proxy_for_(url); !proxy.empty()) {
        // Recorded, not logged. This module became a workspace member and a
        // member may not depend on the root package, where `log` lives -- and
        // a fetch library writing to its caller's log was the wrong shape
        // anyway. `last_proxy()` lets whoever cares report it in their own
        // voice; nobody is forced to.
        gLastProxy_ = proxy;
        cfg.proxy = std::move(proxy);
    }
    return mcpplibs::tinyhttps::HttpClient(std::move(cfg));
}

std::pair<int, int> effective_low_speed_(int limitBytes, int windowSec) {
    const char* env = std::getenv("XLINGS_DOWNLOAD_LOW_SPEED");
    if (!env || !*env) return {limitBytes, windowSec};
    std::string v = env;
    if (v == "off" || v == "0") return {0, 0};
    auto colon = v.find(':');
    if (colon != std::string::npos) {
        try {
            int b = std::stoi(v.substr(0, colon));
            int s = std::stoi(v.substr(colon + 1));
            if (b >= 0 && s >= 0) return {b, s};
        } catch (...) {}
    }
    return {limitBytes, windowSec};
}

std::string human_bytes_(std::uintmax_t bytes) {
    constexpr std::array<const char*, 5> units { "B", "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) return std::format("{} B", bytes);
    return std::format("{:.1f} {}", value, units[unit]);
}

std::optional<std::uintmax_t> available_bytes_(const std::filesystem::path& dir) {
    std::error_code ec;
    auto info = std::filesystem::space(dir, ec);
    if (ec) return std::nullopt;
    return info.available;
}

std::uintmax_t space_shortfall_(std::optional<std::uintmax_t> available,
                                std::int64_t remaining) {
    if (!available || remaining <= 0) return 0;
    auto need = static_cast<std::uintmax_t>(remaining);
    return *available >= need ? 0 : need - *available;
}

std::optional<LandedVerdict> check_landed_(const std::filesystem::path& dest,
                                           const DownloadFileResult& reported) {
    const std::int64_t claimed = reported.expectedBytes.value_or(reported.bytesWritten);
    if (claimed <= 0) return std::nullopt;

    if (reported.expectedBytes && reported.bytesWritten < *reported.expectedBytes) {
        return LandedVerdict{ FailureKind::Transfer, std::format(
            "incomplete transfer: wrote {} of {} bytes",
            reported.bytesWritten, *reported.expectedBytes) };
    }

    // The client counts what it RECEIVED; only the file says what was kept.
    std::error_code ec;
    auto onDisk = std::filesystem::file_size(dest, ec);
    if (ec) {
        return LandedVerdict{ FailureKind::Local, std::format(
            "could not read back {}: {}", dest.filename().string(), ec.message()) };
    }
    if (static_cast<std::int64_t>(onDisk) < claimed) {
        return LandedVerdict{ FailureKind::Local, std::format(
            "could not write {}: {} of {} bytes reached the disk",
            dest.filename().string(), onDisk, claimed) };
    }
    return std::nullopt;
}

bool space_check_enabled_() {
    const char* env = std::getenv("XLINGS_DOWNLOAD_SPACE_CHECK");
    if (!env) return true;
    std::string_view v = env;
    return !(v == "off" || v == "0");
}

DownloadFileResult download_once(const std::string& url, const std::filesystem::path& dest, int connectSec, int maxSec, int lowSpeedLimitBytes, int lowSpeedTimeSec, std::function<void(double, double)> onProgress, std::function<bool()> isCancelled) {
    StallDetector detector(lowSpeedLimitBytes, lowSpeedTimeSec);

    // With the watchdog enabled, lower the per-read socket timeout so a
    // connection that sends NOTHING (progress never fires) also fails
    // quickly instead of sitting on the full maxTime budget.
    int readSec = maxSec;
    if (detector.enabled()) {
        readSec = std::min(maxSec, std::max(30, 2 * lowSpeedTimeSec));
    }
    auto client = make_client(connectSec, readSec, url);

    bool stalled = false;
    auto t0 = std::chrono::steady_clock::now();

    // Asked once, at the first figure the server gives: a disk that cannot
    // hold the body stops the transfer before it is spent, instead of after.
    bool spaceChecked = !space_check_enabled_();
    std::uintmax_t shortBy = 0;
    std::int64_t needed = 0;

    mcpplibs::tinyhttps::DownloadProgressFn progress;
    if (onProgress || detector.enabled() || !spaceChecked) {
        progress = [&](std::int64_t total, std::int64_t downloaded) {
            if (!spaceChecked && total > 0) {
                spaceChecked = true;
                needed = total - downloaded;
                shortBy = space_shortfall_(available_bytes_(dest.parent_path()), needed);
            }
            if (onProgress) {
                onProgress(static_cast<double>(total),
                           static_cast<double>(downloaded));
            }
            if (!stalled && detector.enabled()) {
                auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
                if (detector.update(elapsed, static_cast<double>(downloaded))) {
                    stalled = true;
                }
            }
        };
    }

    std::function<bool()> cancel;
    if (isCancelled || detector.enabled() || !spaceChecked) {
        cancel = [&]() -> bool {
            if (stalled || shortBy > 0) return true;
            return isCancelled && isCancelled();
        };
    }

    auto result = client.download_to_file(url, dest, progress, cancel);

    if (shortBy > 0) {
        auto dir = dest.parent_path();
        return {
            .success = false,
            .error = std::format(
                "not enough space for {}: needs {}, {} free under {} "
                "(set XLINGS_DOWNLOAD_SPACE_CHECK=off to skip this check)",
                dest.filename().string(),
                human_bytes_(static_cast<std::uintmax_t>(needed)),
                human_bytes_(static_cast<std::uintmax_t>(needed) - shortBy),
                dir.string()),
            .failure = FailureKind::Local,
        };
    }
    if (result.ok() && !stalled) {
        return {
            .success = true,
            .bytesWritten = result.bytesWritten,
            .expectedBytes = result.expectedBytes,
            .finalUrl = result.finalUrl,
            .etag = result.etag,
            .lastModified = result.lastModified,
        };
    }
    if (stalled) {
        return {
            .success = false,
            .error = std::format(
                "stalled: average speed below {} B/s over {} s "
                "(set XLINGS_DOWNLOAD_LOW_SPEED=off to disable the watchdog)",
                lowSpeedLimitBytes, lowSpeedTimeSec),
            .failure = FailureKind::Transfer,
        };
    }
    // A status the server chose is the source's answer; anything else (a
    // reset, a timeout, a short read) is the transfer's.
    //
    // A destination the client could not open is the disk's (full, or not
    // writable) -- the one local failure this client reports, and it reports
    // it only in words: "Cannot open file: <path>" (tinyhttps 0.2.9; 0.3.2
    // keeps the text and adds `writeFailed`).
    FailureKind kind = FailureKind::Transfer;
    if (isCancelled && isCancelled()) kind = FailureKind::Cancelled;
    else if (result.error.starts_with("Cannot open file: ")) kind = FailureKind::Local;
    else if (result.statusCode >= 400) kind = FailureKind::Source;
    return {
        .success = false,
        .error = result.error.empty()
            ? "HTTP " + std::to_string(result.statusCode) : result.error,
        .bytesWritten = result.bytesWritten,
        .expectedBytes = result.expectedBytes,
        .finalUrl = result.finalUrl,
        .etag = result.etag,
        .lastModified = result.lastModified,
        .failure = kind,
    };
}

}

std::string resolve_proxy(std::string_view url) {
    return detail_::env_proxy_for_(url);
}

void global_init() {
    mcpplibs::tinyhttps::Socket::platform_init();
}

void global_cleanup() {
    mcpplibs::tinyhttps::Socket::platform_cleanup();
}

DownloadFileResult download_file(const DownloadOptions& opts) {
    global_init();
    if (opts.urls.empty()) return {false, "no URLs provided"};

    std::error_code ec;
    std::filesystem::create_directories(opts.destFile.parent_path(), ec);

    auto [lowSpeedBytes, lowSpeedSecs] = detail_::effective_low_speed_(
        opts.lowSpeedLimitBytes, opts.lowSpeedTimeSec);

    // Breadth first: every candidate gets its first attempt before any
    // candidate gets its second.
    //
    // This used to be the other way round -- `for url { for attempt { } }` --
    // which meant one bad host could consume the entire download before the
    // next URL was ever contacted. With the shipped settings that is
    // `retryCount` 3 + 1 attempts x `maxTimeSec` 600 = **40 minutes on the
    // first candidate**, while a healthy mirror sat untried. The stall
    // watchdog does not help: it only fires below ~10 KB/s, and the measured
    // case was a source holding a steady ~105 KB/s -- slow enough to take
    // half an hour for a 36 MB package, fast enough to never look stalled.
    //
    // The per-candidate attempt budget is unchanged, and so is every
    // give-up-on-this-host rule; only the order changes. A host that failed
    // for a transient reason is still worth a second try, just not before the
    // alternatives have had a first one.
    // A local failure ends the whole download: every candidate writes to the
    // same disk. It is reported without the free space it ran into only when
    // that figure cannot be read.
    auto local_failure = [&](std::string error) {
        std::filesystem::remove(opts.destFile, ec);
        auto dir = opts.destFile.parent_path();
        if (auto free = detail_::available_bytes_(dir);
                free && !error.contains(" free under ")) {
            error += std::format(" ({} free under {})",
                                 detail_::human_bytes_(*free), dir.string());
        }
        return DownloadFileResult{
            .success = false,
            .error = std::move(error),
            .failure = FailureKind::Local,
        };
    };

    std::string lastErr;
    FailureKind lastKind = FailureKind::None;
    std::vector<bool> exhausted(opts.urls.size(), false);
    for (int round = 0; round <= opts.retryCount; ++round) {
        bool anyLive = false;
        for (std::size_t i = 0; i < opts.urls.size(); ++i) {
            if (exhausted[i]) continue;
            const auto& url = opts.urls[i];
            if (opts.isCancelled && opts.isCancelled()) {
                return {.success = false, .error = "cancelled",
                        .failure = FailureKind::Cancelled};
            }
            anyLive = true;
            auto r = opts.transferOverride
                ? opts.transferOverride(url, opts.destFile)
                : opts.loopbackHttp && url.starts_with("http://127.0.0.1:")
                    ? opts.loopbackHttp(url, opts.destFile)
                : detail_::download_once(url, opts.destFile,
                      opts.connectTimeoutSec, opts.maxTimeSec,
                      lowSpeedBytes, lowSpeedSecs,
                      opts.onProgress, opts.isCancelled);
            if (r.success) {
                // What the file holds is checked against what the transfer
                // reported BEFORE its content is judged: a file the disk cut
                // short hashes to the wrong digest too, and that verdict
                // would blame the source.
                if (auto landed = detail_::check_landed_(opts.destFile, r)) {
                    if (landed->kind == FailureKind::Local) {
                        return local_failure(std::move(landed->error));
                    }
                    lastErr = landed->error;
                    lastKind = landed->kind;
                    if (opts.onUrlAttemptFailed) opts.onUrlAttemptFailed(url, lastErr);
                    std::filesystem::remove(opts.destFile, ec);
                    continue;
                }
                // Candidate acceptance: integrity failures are a property
                // of the SOURCE, not the transfer — reject and move to
                // the next URL rather than failing the whole download.
                std::string verdict =
                    opts.onVerify ? opts.onVerify(url) : std::string{};
                if (verdict.empty()) {
                    r.sourceUrl = url;
                    return r;
                }
                lastErr = verdict;
                lastKind = FailureKind::Source;
                if (opts.onUrlAttemptFailed) opts.onUrlAttemptFailed(url, verdict);
                std::filesystem::remove(opts.destFile, ec);
                // The same bytes would fail again: this source is out for
                // good, not just for this round.
                exhausted[i] = true;
                continue;
            }
            if (r.failure == FailureKind::Local) return local_failure(std::move(r.error));
            if (r.failure == FailureKind::Cancelled) {
                std::filesystem::remove(opts.destFile, ec);
                return r;
            }
            lastErr = r.error;
            lastKind = r.failure == FailureKind::None ? FailureKind::Transfer : r.failure;
            if (opts.onUrlAttemptFailed) opts.onUrlAttemptFailed(url, r.error);
            std::filesystem::remove(opts.destFile, ec);
            // A stalled attempt means this host is throttled for us right
            // now — retrying it would burn another full window.
            if (r.error.rfind("stalled:", 0) == 0) exhausted[i] = true;
        }
        if (!anyLive) break;
        if (round < opts.retryCount) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500 * (round + 1)));
        }
    }
    return {.success = false, .error = lastErr, .failure = lastKind};
}

double probe_latency(const std::string& url, int timeoutMs) {
    global_init();
    mcpplibs::tinyhttps::Socket sock;
    // Extract host and port from URL
    std::string rest = url;
    auto sep = rest.find("://");
    if (sep != std::string::npos) rest = rest.substr(sep + 3);
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);

    int port = url.find("https") != std::string::npos ? 443 : 80;
    std::string host = rest;
    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        try { port = std::stoi(rest.substr(colon + 1)); } catch (...) {}
    }

    auto t0 = std::chrono::steady_clock::now();
    if (!sock.connect(host.c_str(), port, timeoutMs)) {
        return std::numeric_limits<double>::infinity();
    }
    sock.close();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

bool fetch_to_file(const std::string& url, const std::filesystem::path& dest) {
    DownloadOptions o;
    o.destFile = dest;
    o.urls = {url};
    o.retryCount = 3;
    o.connectTimeoutSec = 30;
    o.maxTimeSec = 120;
    return download_file(o).success;
}

RemoteFileMeta query_remote_meta(const std::string& url, int connectTimeoutSec) {
    RemoteFileMeta meta;
    global_init();
    auto client = detail_::make_client(connectTimeoutSec, /*readTimeoutSec=*/60, url);

    mcpplibs::tinyhttps::HttpRequest req;
    req.method = mcpplibs::tinyhttps::Method::HEAD;
    req.url = url;
    req.headers["User-Agent"] = "xlings/1.0";

    auto resp = client.send(req);
    meta.statusCode = resp.statusCode;

    if (!resp.ok()) {
        meta.error = resp.statusText.empty()
            ? std::format("HTTP {}", resp.statusCode)
            : std::format("HTTP {} {}", resp.statusCode, resp.statusText);
        return meta;
    }

    // Case-insensitive header lookup
    for (auto& [k, v] : resp.headers) {
        std::string lower = k;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "content-length") {
            try { meta.contentLength = std::stoll(v); } catch (...) { meta.contentLength = -1; }
        } else if (lower == "last-modified") {
            meta.lastModified = v;
        } else if (lower == "etag") {
            meta.etag = v;
        }
    }
    meta.ok = true;
    return meta;
}

std::int64_t query_content_length(const std::string& url, int connectTimeoutSec) {
    auto meta = query_remote_meta(url, connectTimeoutSec);
    return meta.ok ? meta.contentLength : -1;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::tinyhttps {

StallDetector::StallDetector(int limitBytes, int windowSec) : limit_(limitBytes), window_(windowSec) {}

bool StallDetector::enabled() const { return limit_ > 0 && window_ > 0; }

bool StallDetector::update(double elapsedSec, double downloadedBytes) {
    if (!enabled()) return false;
    if (!started_) {
        started_ = true;
        winT_ = elapsedSec;
        winB_ = downloadedBytes;
        return false;
    }
    if (downloadedBytes < winB_) {
        // Counter went backwards (redirect restart) — restart the window.
        winT_ = elapsedSec;
        winB_ = downloadedBytes;
        return false;
    }
    if (elapsedSec - winT_ < static_cast<double>(window_)) return false;
    double avg = (downloadedBytes - winB_) / (elapsedSec - winT_);
    if (avg < static_cast<double>(limit_)) return true;
    // Healthy window — slide forward.
    winT_ = elapsedSec;
    winB_ = downloadedBytes;
    return false;
}


[[nodiscard]] std::string_view last_proxy() { return detail_::gLastProxy_; }

} // namespace xlings::tinyhttps
