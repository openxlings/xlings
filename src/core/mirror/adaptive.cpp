// xlings.core.mirror.adaptive — latency-ordered candidate selection for
// multi-URL downloads (0.4.49).
//
// Design: .agents/docs/2026-06-04-github-asset-adaptive-mirror.md
//
// Problem: with no explicit mirror config, plain GitHub asset URLs are
// always tried original-first; on a degraded network ("connects, then
// trickles") the user waits out the full timeout budget before any mirror
// is attempted. This partition probes each candidate host's TCP connect
// latency once per process (memoized) and orders candidates accordingly,
// gated by a sha256 integrity rule: third-party mirrors may only be tried
// BEFORE the author-declared URL when the payload's bytes are pinned by a
// declared sha256.
//
// The same cache doubles as the session network profile: a host whose
// transfer stalled (watchdog abort in tinyhttps) is penalized to ∞ so the
// next download in this process skips straight past it.

module;

#include <cstdlib>

module xlings.core.mirror.adaptive;

import std;
import xlings.core.log;
import xlings.libs.tinyhttps;

namespace xlings::mirror::adaptive {

namespace {

constexpr int kProbeTimeoutMs = 1500;

// "https://github.com/x/y/releases/..." → "https://github.com"
std::string host_root_(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return {};
    auto host_start = scheme_end + 3;
    auto path_start = url.find('/', host_start);
    return std::string(path_start == std::string_view::npos
        ? url : url.substr(0, path_start));
}

struct Cache {
    std::mutex mtx;
    std::unordered_map<std::string, double> latency;  // host root → seconds (∞ = bad)
};

Cache& cache_() {
    static Cache c;
    return c;
}

bool adaptive_disabled_() {
    const char* env = std::getenv("XLINGS_ADAPTIVE_MIRROR");
    if (!env || !*env) return false;
    std::string v = env;
    return v == "off" || v == "0";
}

}

double host_latency(const std::string& url, const ProbeFn& probe) {
    auto host = host_root_(url);
    if (host.empty()) return 0.0;

    auto& c = cache_();
    {
        std::scoped_lock lock(c.mtx);
        if (auto it = c.latency.find(host); it != c.latency.end())
            return it->second;
    }
    double lat = probe
        ? probe(host)
        : tinyhttps::probe_latency(host, kProbeTimeoutMs);
    {
        std::scoped_lock lock(c.mtx);
        c.latency.emplace(host, lat);
    }
    log::debug("[mirror:adaptive] {} latency {:.0f} ms", host,
               std::isinf(lat) ? -1.0 : lat * 1000.0);
    return lat;
}

void penalize_host(const std::string& url) {
    auto host = host_root_(url);
    if (host.empty()) return;
    auto& c = cache_();
    std::scoped_lock lock(c.mtx);
    c.latency[host] = std::numeric_limits<double>::infinity();
    log::debug("[mirror:adaptive] penalized {}", host);
}

void reset_for_tests() {
    auto& c = cache_();
    std::scoped_lock lock(c.mtx);
    c.latency.clear();
}

std::vector<std::string> reorder(std::vector<std::string> urls, bool has_sha256, const ProbeFn& probe) {
    if (urls.size() < 2 || adaptive_disabled_()) return urls;

    struct Entry {
        std::string url;
        double      lat;
        std::size_t idx;   // declared position, for stable ties
    };
    std::vector<Entry> entries;
    entries.reserve(urls.size());
    for (std::size_t i = 0; i < urls.size(); ++i) {
        double lat = host_latency(urls[i], probe);
        entries.push_back({std::move(urls[i]), lat, i});
    }

    auto by_latency = [](const Entry& a, const Entry& b) {
        if (a.lat != b.lat) return a.lat < b.lat;
        return a.idx < b.idx;   // stable: keep declared order on ties
    };

    bool primary_pinned = !has_sha256
        && !std::isinf(entries.front().lat);

    if (primary_pinned) {
        // Unpinned content: the author-declared URL stays authoritative;
        // only the fallbacks compete on latency.
        std::sort(entries.begin() + 1, entries.end(), by_latency);
    } else {
        std::sort(entries.begin(), entries.end(), by_latency);
    }

    std::vector<std::string> out;
    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(std::move(e.url));
    return out;
}

}
