// The freshness probe of a cached download with no sha256 (#417, design
// 2026-11-27 X9): HEAD, and where a server refuses HEAD for what GET serves --
// GitCode's release assets answer 401 -- the headers of a GET stopped after
// its first block.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.tinyhttps;

namespace tk = xlings::testkit;

XTEST(FreshnessGet, AHeadRefusedByMethodIsAskedWithGet, .area = "xim", .covers = {"FRESHNESS-GET"}) {
    for (const int refused : {401, 403, 405, 501})
        EXPECT_TRUE(xlings::tinyhttps::head_refused(refused)) << refused;
    // Not there, or failed: GET would answer the same.
    for (const int answered : {200, 206, 301, 404, 410, 429, 500, 502, 503})
        EXPECT_FALSE(xlings::tinyhttps::head_refused(answered)) << answered;
}

// The real server (CN mirror lanes only: it is a GitCode asset).
XTEST(FreshnessGet, AGitCodeReleaseAssetHasItsSizeThroughGet, .area = "xim", .covers = {"FRESHNESS-GET"},
      .requires_ = {"network"}) {
    const char* mirror = std::getenv("XLINGS_TEST_MIRROR");
    if (!mirror || std::string_view(mirror) != "CN") GTEST_SKIP() << "a GitCode asset: XLINGS_TEST_MIRROR=CN";
    const auto meta = xlings::tinyhttps::query_remote_meta(
        "https://gitcode.com/xlings-res/glibc/releases/download/2.39/glibc-2.39-linux-x86_64.tar.gz");
    EXPECT_TRUE(meta.ok) << meta.statusCode << " " << meta.error;
    EXPECT_GT(meta.contentLength, 0) << meta.statusCode;
}
