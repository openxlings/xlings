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

    mcpplibs::tinyhttps::DownloadProgressFn progress;
    if (onProgress || detector.enabled()) {
        progress = [&](std::int64_t total, std::int64_t downloaded) {
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
    if (isCancelled || detector.enabled()) {
        cancel = [&]() -> bool {
            if (stalled) return true;
            return isCancelled && isCancelled();
        };
    }

    auto result = client.download_to_file(url, dest, progress, cancel);

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
        return {false, std::format(
            "stalled: average speed below {} B/s over {} s "
            "(set XLINGS_DOWNLOAD_LOW_SPEED=off to disable the watchdog)",
            lowSpeedLimitBytes, lowSpeedTimeSec)};
    }
    return {
        .success = false,
        .error = result.error.empty()
            ? "HTTP " + std::to_string(result.statusCode) : result.error,
        .bytesWritten = result.bytesWritten,
        .expectedBytes = result.expectedBytes,
        .finalUrl = result.finalUrl,
        .etag = result.etag,
        .lastModified = result.lastModified,
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
    std::string lastErr;
    std::vector<bool> exhausted(opts.urls.size(), false);
    for (int round = 0; round <= opts.retryCount; ++round) {
        bool anyLive = false;
        for (std::size_t i = 0; i < opts.urls.size(); ++i) {
            if (exhausted[i]) continue;
            const auto& url = opts.urls[i];
            if (opts.isCancelled && opts.isCancelled()) return {false, "cancelled"};
            anyLive = true;
            auto r = opts.transferOverride
                ? opts.transferOverride(url, opts.destFile)
                : detail_::download_once(url, opts.destFile,
                      opts.connectTimeoutSec, opts.maxTimeSec,
                      lowSpeedBytes, lowSpeedSecs,
                      opts.onProgress, opts.isCancelled);
            if (r.success) {
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
                if (opts.onUrlAttemptFailed) opts.onUrlAttemptFailed(url, verdict);
                std::filesystem::remove(opts.destFile, ec);
                // The same bytes would fail again: this source is out for
                // good, not just for this round.
                exhausted[i] = true;
                continue;
            }
            lastErr = r.error;
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
    return {false, lastErr};
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
