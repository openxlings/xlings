#include <gtest/gtest.h>

import std;
import xlings.core.xim.indexfetch;
import xlings.core.config;
import xlings.platform;

using xlings::xim::parse_index_manifest;
using xlings::xim::index_asset_urls;
using xlings::xim::reconcile_index_temps;

// ── #377: per-repo artifact sources ──
static xlings::IndexRepo mkrepo(std::string name, std::string base) {
    xlings::IndexRepo r;
    r.name = std::move(name);
    r.url = "https://x/y.git";
    r.artifactBase = std::move(base);
    return r;
}

TEST(ArtifactSourceFor, GithubForgeBase) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("mcpplibs", "https://github.com/xlings-res/mcpp-index"));
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->forge());
    EXPECT_EQ(s->server, "https://github.com/xlings-res");
    EXPECT_EQ(s->repoName, "mcpp-index");
    EXPECT_EQ(s->key, "mcpplibs");
    EXPECT_FALSE(s->localDir.has_value());
}

TEST(ArtifactSourceFor, GitcodeForgeBaseTrimsTrailingSlash) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://gitcode.com/xlings-res/mcpp-index/"));
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->forge());
    EXPECT_EQ(s->server, "https://gitcode.com/xlings-res");
    EXPECT_EQ(s->repoName, "mcpp-index");
}

TEST(ArtifactSourceFor, FlatHttpBase) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://example.com/idx/myindex"));
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->forge());
    EXPECT_EQ(s->repoName, "myindex");
    EXPECT_FALSE(s->localDir.has_value());
}

TEST(ArtifactSourceFor, LocalDirAndFileUrl) {
    auto s1 = xlings::xim::artifact_source_for(mkrepo("m", "/tmp/serve/myindex"));
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s1->localDir.has_value());
    EXPECT_FALSE(s1->forge());
    EXPECT_EQ(s1->repoName, "myindex");
    auto s2 = xlings::xim::artifact_source_for(mkrepo("m", "file:///tmp/serve/myindex"));
    ASSERT_TRUE(s2.has_value());
    ASSERT_TRUE(s2->localDir.has_value());
    EXPECT_EQ(s2->localDir->generic_string(), "/tmp/serve/myindex");
}

TEST(ArtifactSourceFor, EmptyBaseIsNullopt) {
    EXPECT_FALSE(xlings::xim::artifact_source_for(mkrepo("m", "")).has_value());
}

TEST(SelectManifest, ExactMatchWins) {
    std::map<std::string, xlings::xim::IndexManifest> p;
    p["a"].index_name = "a";
    p["b"].index_name = "b";
    auto* m = xlings::xim::select_manifest(p, "b", true);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->index_name, "b");
}

TEST(SelectManifest, SoleEntryFallbackOnlyWhenEnabled) {
    std::map<std::string, xlings::xim::IndexManifest> p;
    p["mcpp"].index_name = "mcpp";
    EXPECT_NE(xlings::xim::select_manifest(p, "mcpplibs", true), nullptr);
    EXPECT_EQ(xlings::xim::select_manifest(p, "mcpplibs", false), nullptr);  // official: exact only
}

TEST(SelectManifest, MultiEntryMissIsNull) {
    std::map<std::string, xlings::xim::IndexManifest> p;
    p["a"].index_name = "a";
    p["b"].index_name = "b";
    EXPECT_EQ(xlings::xim::select_manifest(p, "c", true), nullptr);
}

TEST(IndexAssetUrls, CustomForgeVersionedTagFirstSingleServer) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://github.com/xlings-res/mcpp-index"));
    auto urls = index_asset_urls("mcpp-index-2e23e20.tar.gz", "GLOBAL", "2e23e20", &*s);
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0], "https://github.com/xlings-res/mcpp-index/releases/download/v2e23e20/mcpp-index-2e23e20.tar.gz");
    EXPECT_EQ(urls[1], "https://github.com/xlings-res/mcpp-index/releases/download/latest/mcpp-index-2e23e20.tar.gz");
}

TEST(IndexAssetUrls, CustomFlatBase) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://example.com/idx/myindex"));
    auto urls = index_asset_urls("a.tar.gz", "GLOBAL", "1", &*s);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://example.com/idx/myindex/a.tar.gz");
}

TEST(IndexPointerUrls, CustomForgeRawUrl) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://github.com/xlings-res/mcpp-index"));
    auto urls = xlings::xim::index_pointer_urls("mcpp-index-pointers.json", "GLOBAL", &*s);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://raw.githubusercontent.com/xlings-res/mcpp-index/main/mcpp-index-pointers.json");
}

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
