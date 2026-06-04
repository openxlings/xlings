// Unit tests for the adaptive GitHub-asset mirroring (0.4.49):
//   - tinyhttps::StallDetector (windowed-average stall watchdog)
//   - mirror::adaptive::reorder / host_latency / penalize_host
// Design: .agents/docs/2026-06-04-github-asset-adaptive-mirror.md
#include <gtest/gtest.h>

import std;
import xlings.libs.tinyhttps;
import xlings.core.mirror.adaptive;
import xlings.platform;

using xlings::tinyhttps::StallDetector;
namespace adaptive = xlings::mirror::adaptive;

// ── StallDetector ────────────────────────────────────────────────────

TEST(StallDetector, DisabledNeverStalls) {
    StallDetector d(0, 0);
    EXPECT_FALSE(d.enabled());
    EXPECT_FALSE(d.update(0, 0));
    EXPECT_FALSE(d.update(100, 0));
}

TEST(StallDetector, HealthySpeedSlidesWindow) {
    StallDetector d(10 * 1024, 15);
    EXPECT_FALSE(d.update(0, 0));
    // 1 MB in 16 s ≈ 64 KB/s — healthy, window slides.
    EXPECT_FALSE(d.update(16, 1024.0 * 1024));
    // Another healthy window.
    EXPECT_FALSE(d.update(32, 2 * 1024.0 * 1024));
}

TEST(StallDetector, TrickleStallsAfterOneWindow) {
    StallDetector d(10 * 1024, 15);
    EXPECT_FALSE(d.update(0, 0));
    // Inside the window: no verdict yet.
    EXPECT_FALSE(d.update(10, 5 * 1024));
    // Window elapsed: 16 KB over 16 s = 1 KB/s < 10 KB/s → stalled.
    EXPECT_TRUE(d.update(16, 16 * 1024));
}

TEST(StallDetector, ZeroBytesStalls) {
    StallDetector d(10 * 1024, 15);
    EXPECT_FALSE(d.update(0, 0));
    EXPECT_TRUE(d.update(15.5, 0));
}

TEST(StallDetector, FastThenThrottledStalls) {
    StallDetector d(10 * 1024, 15);
    EXPECT_FALSE(d.update(0, 0));
    EXPECT_FALSE(d.update(15, 10 * 1024.0 * 1024));  // 10 MB fast start
    // Then the link gets throttled: +30 KB over the next 16 s.
    EXPECT_TRUE(d.update(31, 10 * 1024.0 * 1024 + 30 * 1024));
}

TEST(StallDetector, CounterResetRestartsWindow) {
    StallDetector d(10 * 1024, 15);
    EXPECT_FALSE(d.update(0, 0));
    EXPECT_FALSE(d.update(10, 5 * 1024.0 * 1024));
    // Redirect restarted the transfer — counter went backwards.
    EXPECT_FALSE(d.update(12, 0));
    // New window measured from the reset point: healthy again.
    EXPECT_FALSE(d.update(28, 2 * 1024.0 * 1024));
}

// ── mirror::adaptive ─────────────────────────────────────────────────

namespace {

struct ProbeRecorder {
    std::map<std::string, double> table;     // host root → latency
    mutable std::map<std::string, int> hits; // host root → probe count
    adaptive::ProbeFn fn() const {
        return [this](const std::string& host) {
            ++hits[host];
            auto it = table.find(host);
            return it == table.end()
                ? std::numeric_limits<double>::infinity() : it->second;
        };
    }
};

struct EnvGuard {
    std::string key, old;
    EnvGuard(const std::string& k, const std::string& v) : key(k) {
        if (const char* o = std::getenv(k.c_str())) old = o;
        xlings::platform::set_env_variable(k, v);
    }
    ~EnvGuard() { xlings::platform::set_env_variable(key, old); }
};

const std::string kGithub = "https://github.com/o/r/releases/download/v1/a.tar.gz";
const std::string kFast   = "https://ghfast.top/https://github.com/o/r/releases/download/v1/a.tar.gz";
const std::string kProxy  = "https://ghproxy.net/https://github.com/o/r/releases/download/v1/a.tar.gz";

} // namespace

TEST(AdaptiveReorder, Sha256FullSortByLatency) {
    adaptive::reset_for_tests();
    ProbeRecorder p;
    p.table = {{"https://github.com", 1.2},
               {"https://ghfast.top", 0.05},
               {"https://ghproxy.net", 0.3}};
    auto out = adaptive::reorder({kGithub, kFast, kProxy},
                                 /*has_sha256=*/true, p.fn());
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], kFast);
    EXPECT_EQ(out[1], kProxy);
    EXPECT_EQ(out[2], kGithub);
}

TEST(AdaptiveReorder, NoSha256KeepsReachablePrimaryFirst) {
    adaptive::reset_for_tests();
    ProbeRecorder p;
    p.table = {{"https://github.com", 1.2},
               {"https://ghfast.top", 0.05},
               {"https://ghproxy.net", 0.3}};
    auto out = adaptive::reorder({kGithub, kProxy, kFast},
                                 /*has_sha256=*/false, p.fn());
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], kGithub);   // unpinned content: author URL stays first
    EXPECT_EQ(out[1], kFast);     // fallbacks latency-sorted
    EXPECT_EQ(out[2], kProxy);
}

TEST(AdaptiveReorder, NoSha256DeadPrimaryIsDemoted) {
    adaptive::reset_for_tests();
    ProbeRecorder p;
    p.table = {{"https://ghfast.top", 0.05},
               {"https://ghproxy.net", 0.3}};   // github absent → ∞
    auto out = adaptive::reorder({kGithub, kProxy, kFast},
                                 /*has_sha256=*/false, p.fn());
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], kFast);
    EXPECT_EQ(out[1], kProxy);
    EXPECT_EQ(out[2], kGithub);
}

TEST(AdaptiveReorder, ProbeMemoizedPerHost) {
    adaptive::reset_for_tests();
    ProbeRecorder p;
    p.table = {{"https://github.com", 0.1},
               {"https://ghfast.top", 0.2},
               {"https://ghproxy.net", 0.3}};
    (void)adaptive::reorder({kGithub, kFast, kProxy}, true, p.fn());
    (void)adaptive::reorder({kGithub, kFast, kProxy}, true, p.fn());
    EXPECT_EQ(p.hits["https://github.com"], 1);
    EXPECT_EQ(p.hits["https://ghfast.top"], 1);
    EXPECT_EQ(p.hits["https://ghproxy.net"], 1);
}

TEST(AdaptiveReorder, PenalizedHostGoesLast) {
    adaptive::reset_for_tests();
    ProbeRecorder p;
    p.table = {{"https://github.com", 0.1},
               {"https://ghfast.top", 0.2}};
    adaptive::penalize_host(kGithub);   // stalled earlier this session
    auto out = adaptive::reorder({kGithub, kFast},
                                 /*has_sha256=*/true, p.fn());
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], kFast);
    EXPECT_EQ(out[1], kGithub);
    // The penalty must not have been overwritten by a fresh probe.
    EXPECT_EQ(p.hits["https://github.com"], 0);
}

TEST(AdaptiveReorder, SingleCandidateUnchangedAndUnprobed) {
    adaptive::reset_for_tests();
    ProbeRecorder p;
    auto out = adaptive::reorder({kGithub}, true, p.fn());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], kGithub);
    EXPECT_TRUE(p.hits.empty());
}

TEST(AdaptiveReorder, EnvOffDisablesReordering) {
    adaptive::reset_for_tests();
    ProbeRecorder p;
    p.table = {{"https://github.com", 9.0},
               {"https://ghfast.top", 0.01}};
    EnvGuard g("XLINGS_ADAPTIVE_MIRROR", "off");
    auto out = adaptive::reorder({kGithub, kFast}, true, p.fn());
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], kGithub);
    EXPECT_EQ(out[1], kFast);
    EXPECT_TRUE(p.hits.empty());
}
