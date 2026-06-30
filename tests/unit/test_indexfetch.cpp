#include <gtest/gtest.h>

import std;
import xlings.core.xim.indexfetch;
import xlings.platform;

using xlings::xim::parse_index_manifest;
using xlings::xim::index_asset_urls;
using xlings::xim::reconcile_index_temps;

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

// ── reconcile_index_temps: recover interrupted swaps, sweep dead-pid leaks ──

TEST(ReconcileIndexTemps, RecoversOrphanSweepsDeadKeepsLive) {
    namespace fs = std::filesystem;
    auto data = fs::temp_directory_path() / "xlings_reconcile_temps_test";
    fs::remove_all(data);
    fs::create_directories(data);

    // A pid far above any live process — is_process_alive() must report dead.
    const std::string deadPid = "2147480000";

    // Case 1: interrupted swap — the live base is MISSING, but an `.old.<dead>`
    // still holds the only good index copy. Must be restored into place.
    auto base   = data / "xim-pkgindex";
    auto orphan = data / ("xim-pkgindex.old." + deadPid);
    fs::create_directories(orphan / "pkgs");
    { std::ofstream(orphan / ".xlings-index-version") << "0.4.50"; }

    // Case 2: leaked staging from a dead pid — must be swept.
    auto leaked = data / ("xim-pkgindex.artifact." + deadPid);
    fs::create_directories(leaked);
    { std::ofstream(leaked / "scratch") << "x"; }

    // Case 3: staging owned by THIS (live) process — must be preserved.
    auto live = data / ("xim-pkgindex.tmp." + std::to_string(xlings::platform::get_pid()));
    fs::create_directories(live);

    reconcile_index_temps(data);

    EXPECT_TRUE(fs::exists(base / "pkgs"))                 << "orphan not restored";
    EXPECT_TRUE(fs::exists(base / ".xlings-index-version"));
    EXPECT_FALSE(fs::exists(orphan))                       << "orphan dir should be consumed";
    EXPECT_FALSE(fs::exists(leaked))                       << "dead-pid staging not swept";
    EXPECT_TRUE(fs::exists(live))                          << "live-pid staging must survive";

    fs::remove_all(data);
}

// A healthy base must not be clobbered by a stale `.old.<dead>` sibling.
TEST(ReconcileIndexTemps, KeepsHealthyBaseDropsStaleBackup) {
    namespace fs = std::filesystem;
    auto data = fs::temp_directory_path() / "xlings_reconcile_keepbase_test";
    fs::remove_all(data);
    fs::create_directories(data);

    auto base = data / "xim-pkgindex";
    fs::create_directories(base / "pkgs");
    { std::ofstream(base / ".xlings-index-version") << "0.4.61"; }

    auto stale = data / "xim-pkgindex.old.2147480001";
    fs::create_directories(stale / "pkgs");
    { std::ofstream(stale / ".xlings-index-version") << "0.4.50"; }  // older copy

    reconcile_index_temps(data);

    EXPECT_TRUE(fs::exists(base / ".xlings-index-version"));
    // The healthy current index must remain untouched (still 0.4.61). Scope the
    // stream so the handle is closed before remove_all (Windows can't delete a
    // file that's still open).
    std::string v;
    { std::ifstream in(base / ".xlings-index-version"); std::getline(in, v); }
    EXPECT_EQ(v, "0.4.61");
    EXPECT_FALSE(fs::exists(stale)) << "spent backup should be reclaimed";

    fs::remove_all(data);
}
