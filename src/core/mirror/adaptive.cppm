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

export module xlings.core.mirror.adaptive;

import std;

import xlings.libs.tinyhttps;

namespace xlings::mirror::adaptive { // anonymous namespace

// Injectable prober for tests. Empty = real TCP connect probe.
export using ProbeFn = std::function<double(const std::string& hostRootUrl)>;

// Connect latency for the host of `url`, probed once per process and
// memoized. Unknown/unparseable URLs return 0 (neutral — keeps declared
// order under stable sort).
export double host_latency(const std::string& url, const ProbeFn& probe = {});

// Session-scoped demotion: a host whose transfer stalled is as bad as an
// unreachable one for the rest of this process.
export void penalize_host(const std::string& url);

// Test hook: drop all cached latencies.
export void reset_for_tests();

// Order download candidates by measured host latency.
//
//   has_sha256 == true   full stable sort ascending by latency — mirrors
//                        may be tried before the original URL; the
//                        declared sha256 pins the exact bytes.
//   has_sha256 == false  the author-declared first URL keeps its position
//                        unless its host is unreachable/penalized (∞);
//                        only the remaining candidates are latency-sorted.
//
// Single-candidate lists and XLINGS_ADAPTIVE_MIRROR=off return unchanged.
export std::vector<std::string> reorder(std::vector<std::string> urls,
                                        bool has_sha256,
                                        const ProbeFn& probe = {});

} // namespace xlings::mirror::adaptive
