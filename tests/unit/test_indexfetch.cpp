#include <gtest/gtest.h>

import xlings.core.xim.indexfetch;

using xlings::xim::parse_index_manifest;
using xlings::xim::index_asset_urls;

TEST(IndexManifest, ParsesValid) {
    auto m = parse_index_manifest(R"({
        "format_version": 1,
        "index_version": "0.4.52",
        "index_name": "xim",
        "generated_at": "2026-06-22T00:00:00Z",
        "source_commit": "abc123",
        "artifact": { "name": "xim-index-0.4.52.tar.gz", "sha256": "DEADbeef", "size": 95000 },
        "signature": null
    })");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->format_version, 1);
    EXPECT_EQ(m->index_version, "0.4.52");
    EXPECT_EQ(m->artifact_name, "xim-index-0.4.52.tar.gz");
    EXPECT_EQ(m->artifact_sha256, "deadbeef");   // lowercased
    EXPECT_EQ(m->artifact_size, 95000u);
}

TEST(IndexManifest, RejectsMissingArtifact) {
    EXPECT_FALSE(parse_index_manifest(R"({"format_version":1})").has_value());
}

TEST(IndexManifest, RejectsMissingSha256) {
    EXPECT_FALSE(parse_index_manifest(
        R"({"format_version":1,"artifact":{"name":"x.tar.gz"}})").has_value());
}

TEST(IndexManifest, RejectsGarbage) {
    EXPECT_FALSE(parse_index_manifest("not json").has_value());
}

TEST(IndexAssetUrls, BuildsReleaseDownloadUrlForGlobal) {
    auto urls = index_asset_urls("xim-index-latest.json", "GLOBAL");
    ASSERT_FALSE(urls.empty());
    // Must hit the xim-index repo's release-download path on a resource server.
    bool found = false;
    for (auto& u : urls) {
        if (u.find("/xim-index/releases/download/") != std::string::npos
            && u.find("xim-index-latest.json") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "no release-download URL for the pointer asset";
}

TEST(IndexAssetUrls, PrefersVersionedTagWhenVersionGiven) {
    // GitCode only serves assets via the versioned tag (`v<version>`), not the
    // rolling `latest` (404 on releases/download/latest/...). When the version
    // is known, the versioned-tag URL must be present and ordered before the
    // `latest` URL on the same server.
    auto urls = index_asset_urls("xim-index-scode-0.4.58.tar.gz", "GLOBAL", "0.4.58");
    ASSERT_FALSE(urls.empty());
    auto firstVersioned = std::string::npos, firstLatest = std::string::npos;
    for (std::size_t i = 0; i < urls.size(); ++i) {
        if (urls[i].find("/releases/download/v0.4.58/") != std::string::npos
            && firstVersioned == std::string::npos) firstVersioned = i;
        if (urls[i].find("/releases/download/latest/") != std::string::npos
            && firstLatest == std::string::npos) firstLatest = i;
    }
    EXPECT_NE(firstVersioned, std::string::npos) << "no v0.4.58 versioned-tag URL";
    if (firstLatest != std::string::npos)
        EXPECT_LT(firstVersioned, firstLatest) << "versioned tag must precede latest";
}
