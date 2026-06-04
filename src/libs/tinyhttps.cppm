export module xlings.libs.tinyhttps;

import std;
import mcpplibs.tinyhttps;
import xlings.core.log;

export namespace xlings::tinyhttps {

// ── Public types ─────────────────────────────────────────────────────

struct DownloadOptions {
    std::filesystem::path destFile;
    std::vector<std::string> urls;          // primary + fallbacks, tried in order
    int retryCount        { 3 };
    int connectTimeoutSec { 30 };
    int maxTimeSec        { 600 };
    // Stall watchdog (0.4.49; see
    // .agents/docs/2026-06-04-github-asset-adaptive-mirror.md):
    // abort an attempt whose windowed average speed stays below
    // lowSpeedLimitBytes for lowSpeedTimeSec, and move to the next
    // candidate URL. 0 in either field disables. Env override:
    // XLINGS_DOWNLOAD_LOW_SPEED=off | <bytes>:<secs>.
    int lowSpeedLimitBytes { 10 * 1024 };
    int lowSpeedTimeSec    { 15 };
    std::function<void(double total, double now)> onProgress;
    std::function<bool()> isCancelled;      // returns true to abort download
    // Per-URL attempt failure hook: called once per failed attempt with the
    // attempted URL and its error string ("stalled: ..." for watchdog
    // aborts). Used by the downloader to penalize degraded hosts.
    std::function<void(const std::string& url, const std::string& error)>
        onUrlAttemptFailed;
};

// Windowed-average stall detector (curl --speed-limit/--speed-time style).
// Pure logic, fed cumulative (elapsedSec, downloadedBytes) samples from the
// progress callback. Exported for unit tests.
class StallDetector {
public:
    StallDetector(int limitBytes, int windowSec)
        : limit_(limitBytes), window_(windowSec) {}

    bool enabled() const { return limit_ > 0 && window_ > 0; }

    // Feed one sample. Returns true when the windowed average speed over a
    // full window fell below the limit (= stalled).
    bool update(double elapsedSec, double downloadedBytes) {
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

private:
    int    limit_;
    int    window_;
    bool   started_ { false };
    double winT_ { 0 };
    double winB_ { 0 };
};

struct DownloadFileResult {
    bool success { false };
    std::string error;
};

// Result of a HEAD probe used by the cache to decide whether a previously
// downloaded file is still current. `ok` is true only when the server
// returned a 2xx response — header fields may still be empty if the
// server omitted them, in which case callers should fall back to other
// signals (e.g. file existence) rather than re-downloading blindly.
struct RemoteFileMeta {
    bool ok { false };
    int statusCode { 0 };
    std::int64_t contentLength { -1 };  // -1 if header missing/unparseable
    std::string lastModified;           // RFC 1123 string, empty if missing
    std::string etag;                   // empty if missing
    std::string error;
};

void global_init();
void global_cleanup();
DownloadFileResult download_file(const DownloadOptions& opts);
double probe_latency(const std::string& url, int timeoutMs = 2000);
bool fetch_to_file(const std::string& url, const std::filesystem::path& dest);
std::int64_t query_content_length(const std::string& url, int connectTimeoutSec = 10);
RemoteFileMeta query_remote_meta(const std::string& url, int connectTimeoutSec = 10);

// Returns the proxy URL that would be used for `url` (libcurl-style env
// resolution: HTTPS_PROXY / HTTP_PROXY / ALL_PROXY with NO_PROXY exemption,
// case-insensitive variants accepted). Empty result means direct connection.
// Exposed for testability and for callers that want to display "what proxy
// am I about to use" — the actual download path calls this internally.
std::string resolve_proxy(std::string_view url);

// ── Implementations ──────────────────────────────────────────────────

namespace detail_ {

// Pull host out of an http(s) URL: "https://example.com:443/foo" → "example.com".
// Used by NO_PROXY suffix matching.
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

// libcurl/curl/Go-net compatible NO_PROXY matcher.
// `np` is comma-separated entries; an entry matches `host` if:
//   - exact equal
//   - leading dot suffix match (`.example.com` matches `foo.example.com` and `example.com`)
//   - bare suffix (`example.com` matches `foo.example.com` and `example.com`)
//   - `*` matches everything (rarely used; libcurl rejects it but Go honours it)
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

// Resolve which proxy to use for `url` from env, libcurl-style. Returns empty
// string when direct connection should be used. Matches libcurl/Go/git
// behaviour: HTTPS_PROXY for https:// URLs, HTTP_PROXY for http://, ALL_PROXY
// as fallback, NO_PROXY exemptions, lowercase variants accepted.
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
        log::debug("tinyhttps: using proxy {} for {}", proxy, url);
        cfg.proxy = std::move(proxy);
    }
    return mcpplibs::tinyhttps::HttpClient(std::move(cfg));
}

// Resolve the effective stall-watchdog parameters: env override > options.
//   XLINGS_DOWNLOAD_LOW_SPEED=off|0      → disabled
//   XLINGS_DOWNLOAD_LOW_SPEED=<b>:<s>    → custom limit bytes / window secs
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

// Single download attempt: stream GET url → dest file with progress + cancel
// + optional stall watchdog.
DownloadFileResult download_once(
    const std::string& url,
    const std::filesystem::path& dest,
    int connectSec,
    int maxSec,
    int lowSpeedLimitBytes,
    int lowSpeedTimeSec,
    std::function<void(double, double)> onProgress,
    std::function<bool()> isCancelled = nullptr
) {
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
        return {true, {}};
    }
    if (stalled) {
        return {false, std::format(
            "stalled: average speed below {} B/s over {} s "
            "(set XLINGS_DOWNLOAD_LOW_SPEED=off to disable the watchdog)",
            lowSpeedLimitBytes, lowSpeedTimeSec)};
    }
    return {false, result.error.empty()
        ? "HTTP " + std::to_string(result.statusCode) : result.error};
}

} // namespace detail_

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

    std::string lastErr;
    for (auto& url : opts.urls) {
        if (opts.isCancelled && opts.isCancelled()) return {false, "cancelled"};
        for (int att = 0; att <= opts.retryCount; ++att) {
            if (opts.isCancelled && opts.isCancelled()) return {false, "cancelled"};
            auto r = detail_::download_once(url, opts.destFile,
                opts.connectTimeoutSec, opts.maxTimeSec,
                lowSpeedBytes, lowSpeedSecs,
                opts.onProgress, opts.isCancelled);
            if (r.success) return r;
            lastErr = r.error;
            if (opts.onUrlAttemptFailed) opts.onUrlAttemptFailed(url, r.error);
            std::filesystem::remove(opts.destFile, ec);
            // A stalled attempt means this host is throttled for us right
            // now — retrying the same URL would burn another full window.
            // Move straight to the next candidate.
            if (r.error.rfind("stalled:", 0) == 0) break;
            if (att < opts.retryCount) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(500 * (att + 1)));
            }
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

} // namespace xlings::tinyhttps
