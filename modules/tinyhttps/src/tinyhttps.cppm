export module xlings.libs.tinyhttps;

import std;
import mcpplibs.tinyhttps;

export namespace xlings::tinyhttps {

// ── Public types ─────────────────────────────────────────────────────

struct DownloadFileResult {
    bool success { false };
    std::string error;
    std::int64_t bytesWritten { 0 };
    std::optional<std::int64_t> expectedBytes;
    // Where the client ended up after redirects — for a release asset this is
    // usually a CDN host, not anything the caller listed.
    std::string finalUrl;
    // The CANDIDATE this transfer used, verbatim as passed in `urls`. The
    // caller listed those; `finalUrl` it did not, so only this one can answer
    // "which of my mirrors served this".
    std::string sourceUrl;
    std::string etag;
    std::string lastModified;
};

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
    // Per-URL acceptance hook: called after a successful transfer with the
    // source URL. Return empty to accept; return an error message to
    // REJECT the candidate — the file is removed, onUrlAttemptFailed
    // fires, and the next candidate URL is tried (no same-URL retry: the
    // payload is deterministic, so re-fetching the same source cannot
    // change the verdict). Used for sha256 integrity: a mirror may win
    // the latency race yet serve corrupted bytes.
    std::function<std::string(const std::string& url)> onVerify;
    // TEST SEAM: when set, replaces the network transfer for one URL
    // attempt (must write destFile on success). Lets unit tests exercise
    // the candidate loop / verify fallback without sockets.
    std::function<DownloadFileResult(const std::string& url,
                                     const std::filesystem::path& destFile)>
        transferOverride;
};

// Windowed-average stall detector (curl --speed-limit/--speed-time style).
// Pure logic, fed cumulative (elapsedSec, downloadedBytes) samples from the
// progress callback. Exported for unit tests.
class StallDetector {
public:
    StallDetector(int limitBytes, int windowSec);

    bool enabled() const;

    // Feed one sample. Returns true when the windowed average speed over a
    // full window fell below the limit (= stalled).
    bool update(double elapsedSec, double downloadedBytes);

private:
    int    limit_;
    int    window_;
    bool   started_ { false };
    double winT_ { 0 };
    double winB_ { 0 };
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
std::string url_host_(std::string_view url);

// libcurl/curl/Go-net compatible NO_PROXY matcher.
// `np` is comma-separated entries; an entry matches `host` if:
//   - exact equal
//   - leading dot suffix match (`.example.com` matches `foo.example.com` and `example.com`)
//   - bare suffix (`example.com` matches `foo.example.com` and `example.com`)
//   - `*` matches everything (rarely used; libcurl rejects it but Go honours it)
bool host_in_no_proxy_(std::string_view host, std::string_view np);

// Resolve which proxy to use for `url` from env, libcurl-style. Returns empty
// string when direct connection should be used. Matches libcurl/Go/git
// behaviour: HTTPS_PROXY for https:// URLs, HTTP_PROXY for http://, ALL_PROXY
// as fallback, NO_PROXY exemptions, lowercase variants accepted.
std::string env_proxy_for_(std::string_view url);

auto make_client(int connectTimeoutSec, int readTimeoutSec, std::string_view url)
    -> mcpplibs::tinyhttps::HttpClient;

// Resolve the effective stall-watchdog parameters: env override > options.
//   XLINGS_DOWNLOAD_LOW_SPEED=off|0      → disabled
//   XLINGS_DOWNLOAD_LOW_SPEED=<b>:<s>    → custom limit bytes / window secs
std::pair<int, int> effective_low_speed_(int limitBytes, int windowSec);

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
);

} // namespace detail_

std::string resolve_proxy(std::string_view url);

void global_init();

void global_cleanup();

DownloadFileResult download_file(const DownloadOptions& opts);

double probe_latency(const std::string& url, int timeoutMs);

bool fetch_to_file(const std::string& url, const std::filesystem::path& dest);

RemoteFileMeta query_remote_meta(const std::string& url, int connectTimeoutSec);

std::int64_t query_content_length(const std::string& url, int connectTimeoutSec);


// The proxy the most recent request used, or empty. See tinyhttps.cpp.
[[nodiscard]] std::string_view last_proxy();

} // namespace xlings::tinyhttps
