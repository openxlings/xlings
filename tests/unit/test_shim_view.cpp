// tests/unit/test_shim_view.cpp — the shim-dispatch performance layer:
// the partial home-config capture (SAX, memoized per process), the versions
// DB's own file and its fallback, the program-name index, find_vinfo's
// one-target merge, and the fingerprint-validated shim view cache.
//
// Split from test_xvm_shim.cpp's territory because these tests need no
// shim on disk — they test the READ layer dispatch sits on.

#include <gtest/gtest.h>
#include <cstdio>
#ifndef _WIN32
#include <unistd.h>  // getpid
#endif

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.home_config;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.core.xvm.shim_view;

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kHomeConfig = R"({
  "activeSubos": "default",
  "dbIndex": {"mcpp": "mcpp", "gcc": "gcc", "7zz": "7z"},
  "knownProjects": {"/w/proj": {"lastSeen": "2026-10-04"}},
  "lang": "zh",
  "mirror": "CN",
  "versions": {
    "mcpp": {"type": "program", "filename": "mcpp", "versions": {
      "1.0": {"path": "/store/mcpp/1.0/bin", "kind": "program",
              "destinationName": "mcpp", "sourceName": "mcpp"}}},
    "7zz": {"type": "program", "filename": "7z", "versions": {
      "26.02": {"path": "/store/7zip/26.02/bin", "kind": "program"}}}
  },
  "xim": {"index-repo": "https://example.com/index"}
})";
// Key order above is the writer's (std::map dump) order, on purpose: the
// AbortAtVersions capture is only allowed to abort a SORTED stream, so a
// fixture in any other order would test the degraded path, not the fast one.

class ShimViewTest : public ::testing::Test {
protected:
    std::filesystem::path home_;

    void SetUp() override {
        home_ = fs::temp_directory_path()
              / "xlings_shim_view_test" / std::to_string(::getpid())
              / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::error_code ec;
        fs::remove_all(home_, ec);
        fs::create_directories(home_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(home_, ec);
    }

    void writeHomeConfig(std::string_view content) {
        auto dir = home_ / "data";
        fs::create_directories(dir);
        xlings::platform::write_string_to_file(
            (home_ / ".xlings.json").string(), std::string(content));
    }
    void writeVersionsDb(std::string_view content) {
        auto dir = home_ / "data";
        fs::create_directories(dir);
        xlings::platform::write_string_to_file(
            xlings::versions_db_path(home_).string(), std::string(content));
    }
    // The DB file's real shape since the freshness stamp: a wrapper carrying
    // format, the home config's stat, and the versions map. `stampOverride`
    // empty means "stamp with the config's CURRENT stat" (what a writer
    // that just wrote both files produces); anything else simulates a
    // config that changed since.
    void writeVersionsDbWrapper(std::string_view versions,
                                std::string_view stampOverride = "") {
        std::error_code sec, tec;
        auto cfgPath = home_ / ".xlings.json";
        const auto size = fs::file_size(cfgPath, sec);
        const auto mtime = fs::last_write_time(cfgPath, tec);
        auto stamp = stampOverride.empty()
            ? std::format("{}:{}",
                          static_cast<std::int64_t>(size),
                          static_cast<std::int64_t>(
                              mtime.time_since_epoch().count()))
            : std::string(stampOverride);
        nlohmann::json wrapper = nlohmann::json::object();
        wrapper["format"] = 1;
        wrapper["stamp"] = stamp;
        wrapper["versions"] = nlohmann::json::parse(versions);
        writeVersionsDb(wrapper.dump(2));
    }
    std::string readVersionsDb() {
        return xlings::platform::read_file_to_string(
            xlings::versions_db_path(home_).string());
    }
    static std::string dumpOf(std::string_view content) {
        return nlohmann::json::parse(content).dump();
    }
};

// ── Partial capture ─────────────────────────────────────────────────

TEST_F(ShimViewTest, AbortCaptureStopsAtVersionsAndKeepsTheRest) {
    writeHomeConfig(kHomeConfig);
    auto cap = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    ASSERT_NE(cap, nullptr);
    EXPECT_TRUE(cap->ok);
    EXPECT_TRUE(cap->truncated);
    // Everything that sorts before `versions`.
    EXPECT_EQ(cap->json.value("activeSubos", ""), "default");
    EXPECT_EQ(cap->json.value("mirror", ""), "CN");
    EXPECT_EQ(cap->json.value("lang", ""), "zh");
    EXPECT_TRUE(cap->json.contains("dbIndex"));
    EXPECT_TRUE(cap->json.contains("knownProjects"));
    // The heavy key and everything after it are absent by design.
    EXPECT_FALSE(cap->json.contains("versions"));
    EXPECT_FALSE(cap->json.contains("xim"));
}

TEST_F(ShimViewTest, SkipCaptureLosesOnlyVersions) {
    writeHomeConfig(kHomeConfig);
    auto cap = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::SkipVersions);
    ASSERT_NE(cap, nullptr);
    EXPECT_TRUE(cap->ok);
    EXPECT_FALSE(cap->truncated);
    EXPECT_EQ(cap->json.value("activeSubos", ""), "default");
    // `xim` sorts after `versions`: the CLI's index config needs it, so the
    // skip must be of `versions` alone.
    EXPECT_TRUE(cap->json.contains("xim"));
    EXPECT_FALSE(cap->json.contains("versions"));
}

TEST_F(ShimViewTest, NestedStructureSurvivesTheCapture) {
    writeHomeConfig(R"({
      "activeSubos": "dev",
      "index_repos": [{"name": "xim", "url": "https://a"}, {"name": "b", "url": "https://b"}],
      "subos": {"default": {"role": "global"}, "dev": {}},
      "tui": {"interactive": true},
      "versions": {"x": {"type": "program", "versions": {"1": {}}}}
    })");
    auto cap = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    ASSERT_NE(cap, nullptr);
    EXPECT_TRUE(cap->ok);
    ASSERT_TRUE(cap->json.contains("index_repos"));
    ASSERT_TRUE(cap->json["index_repos"].is_array());
    EXPECT_EQ(cap->json["index_repos"].size(), 2u);
    EXPECT_EQ(cap->json["index_repos"][0]["name"], "xim");
    EXPECT_EQ(cap->json["index_repos"][1]["url"], "https://b");
    ASSERT_TRUE(cap->json["subos"].is_object());
    EXPECT_EQ(cap->json["subos"]["default"]["role"], "global");
    EXPECT_TRUE(cap->json["tui"]["interactive"] == true);
    EXPECT_FALSE(cap->json.contains("versions"));
}

// An unsorted file must NOT abort: "everything before `versions` is
// captured" only holds while the writer's (sorted) key order holds.
TEST_F(ShimViewTest, UnsortedFileDegradesToFullLexInsteadOfAbort) {
    writeHomeConfig(R"({
      "versions": {"x": {"type": "program", "versions": {"1": {}}}},
      "activeSubos": "default",
      "xim": {"late": true}
    })");
    auto cap = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    ASSERT_NE(cap, nullptr);
    EXPECT_TRUE(cap->ok);
    // Degraded: the parse ran on, so nothing but `versions` is missing.
    EXPECT_FALSE(cap->truncated);
    EXPECT_EQ(cap->json.value("activeSubos", ""), "default");
    EXPECT_TRUE(cap->json.contains("xim"));
    EXPECT_FALSE(cap->json.contains("versions"));
}

TEST_F(ShimViewTest, MalformedConfigIsNotOkAndNotAnException) {
    writeHomeConfig("{\"activeSubos\": ");
    auto cap = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    ASSERT_NE(cap, nullptr);
    EXPECT_FALSE(cap->ok);
}

TEST_F(ShimViewTest, MissingFileYieldsNullCapture) {
    auto cap = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    EXPECT_EQ(cap, nullptr);
}

TEST_F(ShimViewTest, MemoServesSameFileAndInvalidatesOnChange) {
    writeHomeConfig(kHomeConfig);
    auto first = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    auto second = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first.get(), second.get());

    // A rewrite (mtime moves) must be re-parsed, not served from the memo.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    writeHomeConfig(R"({"activeSubos": "other", "versions": {}})");
    auto third = xlings::home_config_capture(
        home_ / ".xlings.json", xlings::HomeCaptureMode::AbortAtVersions);
    ASSERT_NE(third, nullptr);
    EXPECT_NE(first.get(), third.get());
    EXPECT_EQ(third->json.value("activeSubos", ""), "other");
}

// ── versions DB file: preference and fallback ───────────────────────

TEST_F(ShimViewTest, VersionsDbFileWinsWhenStampMatches) {
    writeHomeConfig(kHomeConfig);
    writeVersionsDbWrapper(R"({"solo": {"type": "program", "versions": {"2.0": {}}}})");
    auto root = xlings::load_versions_json(home_);
    ASSERT_NE(root, std::nullopt);
    EXPECT_TRUE(root->contains("solo"));
    EXPECT_FALSE(root->contains("mcpp"));
}

// THE dual-write-window case: a client that has never heard of the DB file
// (every client ≤2026.9.30.1) rewrote the home config's `versions` field
// and left file + stamp behind. The config's stat no longer matches the
// stamp, so the file -- now the STALE copy -- must be bypassed.
TEST_F(ShimViewTest, StaleStampFallsBackToHomeConfig) {
    writeHomeConfig(kHomeConfig);
    // The file's stamp names a config stat that no longer holds: the
    // fixture rewrites the config after stamping by hand.
    writeVersionsDbWrapper(R"({"solo": {"type": "program", "versions": {"2.0": {}}}})",
                           "999999:1");
    auto root = xlings::load_versions_json(home_);
    ASSERT_NE(root, std::nullopt);
    EXPECT_TRUE(root->contains("mcpp"));
    EXPECT_FALSE(root->contains("solo"));
}

TEST_F(ShimViewTest, MissingStampOrFormatFallsBackToHomeConfig) {
    writeHomeConfig(kHomeConfig);
    // An old-shape file: a bare versions map, no wrapper. Written by the
    // first released layout of data/versions.json; treat it as stale.
    writeVersionsDb(R"({"solo": {"type": "program", "versions": {"2.0": {}}}})");
    auto root = xlings::load_versions_json(home_);
    ASSERT_NE(root, std::nullopt);
    EXPECT_TRUE(root->contains("mcpp"));
}

TEST_F(ShimViewTest, CorruptDbFileFallsBackToHomeConfig) {
    writeHomeConfig(kHomeConfig);
    writeVersionsDb("{ not json ");
    auto root = xlings::load_versions_json(home_);
    ASSERT_NE(root, std::nullopt);
    EXPECT_TRUE(root->contains("mcpp"));
}

TEST_F(ShimViewTest, AbsentDbFileReadsHomeConfigField) {
    writeHomeConfig(kHomeConfig);
    auto root = xlings::load_versions_json(home_);
    ASSERT_NE(root, std::nullopt);
    EXPECT_TRUE(root->contains("mcpp"));
}

TEST_F(ShimViewTest, ReadableHomeWithoutVersionsIsObservedEmpty) {
    writeHomeConfig(R"({"activeSubos": "default"})");
    auto root = xlings::load_versions_json(home_);
    ASSERT_NE(root, std::nullopt);
    EXPECT_TRUE(root->is_object());
    EXPECT_TRUE(root->empty());
}

TEST_F(ShimViewTest, UnreadableEverywhereIsNulloptNeverEmpty) {
    // A home whose config exists but cannot be parsed: no observed state,
    // not an empty database.
    writeHomeConfig("{ broken");
    auto root = xlings::load_versions_json(home_);
    EXPECT_EQ(root, std::nullopt);
}

// ── dbIndex codec ───────────────────────────────────────────────────

TEST_F(ShimViewTest, ProgramIndexMapsNamesToFilenameStems) {
    xlings::xvm::VersionDB db;
    xlings::xvm::VInfo mcpp;
    mcpp.filename = "mcpp";
    mcpp.versions["1.0"] = xlings::xvm::VData{};
    db["mcpp"] = std::move(mcpp);
    xlings::xvm::VInfo sevenzip;   // DB key differs from the shim file name
    sevenzip.filename = "7z";
    sevenzip.versions["26.02"] = xlings::xvm::VData{};
    db["7zip"] = std::move(sevenzip);

    auto idx = xlings::xvm::program_index_to_json(db);
    ASSERT_TRUE(idx.is_object());
    EXPECT_EQ(idx["mcpp"], "mcpp");
    EXPECT_EQ(idx["7zip"], "7z");
}

// ── find_vinfo: the one-target answer ───────────────────────────────

TEST_F(ShimViewTest, HomeKnowsProgramViaIndexAndStem) {
    writeHomeConfig(kHomeConfig);
    EXPECT_TRUE(xlings::xvm::home_knows_program(home_, "mcpp"));
    EXPECT_TRUE(xlings::xvm::home_knows_program(home_, "gcc"));
    // `7zz` is the DB KEY (index hit); `7z` is the FILENAME stem of that
    // key -- the same stem comparison the slow path performs. Both must
    // answer yes, and a name the index has never heard of must not.
    EXPECT_TRUE(xlings::xvm::home_knows_program(home_, "7zz"));
    EXPECT_TRUE(xlings::xvm::home_knows_program(home_, "7z"));
    EXPECT_FALSE(xlings::xvm::home_knows_program(home_, "absent"));
}

TEST_F(ShimViewTest, HomeKnowsProgramFallsBackToVersionsDbFile) {
    // A migrated home: dbIndex absent from the config (older client wrote
    // it), the DB file carries the answer. The index miss must fall
    // through to the real database.
    writeHomeConfig(R"({"activeSubos": "default", "versions": {
      "mcpp": {"type": "program", "filename": "mcpp", "versions": {"1.0": {}}}}})");
    writeVersionsDbWrapper(R"({"mcpp": {"type": "program", "filename": "mcpp", "versions": {"1.0": {}}}})");
    EXPECT_TRUE(xlings::xvm::home_knows_program(home_, "mcpp"));
    EXPECT_FALSE(xlings::xvm::home_knows_program(home_, "nope"));
}

// ── shim view cache ─────────────────────────────────────────────────

TEST_F(ShimViewTest, ViewStoreLoadRoundTrip) {
    xlings::xvm::ShimView view;
    view.workspace["mcpp"] = "1.0";
    view.installed["mcpp"] = {"1.0", "0.9"};
    xlings::xvm::VInfo info;
    info.type = "program";
    info.filename = "mcpp";
    xlings::xvm::VData data;
    data.path = "/store/mcpp/1.0/bin";
    data.kind = "program";
    info.versions["1.0"] = data;
    view.slice["mcpp"] = std::move(info);

    std::map<std::string, xlings::xvm::ShimViewStat> fp;
    fp["/inputs/home"] = xlings::xvm::ShimViewStat{10, 100};
    fp["/inputs/subos"] = xlings::xvm::ShimViewStat{20, 200};

    xlings::xvm::store_shim_view(home_, "", "mcpp", fp, view);
    auto loaded = xlings::xvm::load_shim_view(home_, "", "mcpp", fp);
    ASSERT_NE(loaded, std::nullopt);
    EXPECT_EQ(loaded->workspace.at("mcpp"), "1.0");
    ASSERT_EQ(loaded->installed.at("mcpp").size(), 2u);
    ASSERT_EQ(loaded->slice.count("mcpp"), 1u);
    EXPECT_EQ(loaded->slice.at("mcpp").versions.at("1.0").path,
              "/store/mcpp/1.0/bin");
}

TEST_F(ShimViewTest, ViewMissesOnAnyFingerprintMismatch) {
    xlings::xvm::ShimView view;
    view.workspace["mcpp"] = "1.0";
    std::map<std::string, xlings::xvm::ShimViewStat> fp;
    fp["/inputs/home"] = xlings::xvm::ShimViewStat{10, 100};
    xlings::xvm::store_shim_view(home_, "", "mcpp", fp, view);

    auto changed = fp;
    changed["/inputs/home"] = xlings::xvm::ShimViewStat{11, 100};
    EXPECT_EQ(xlings::xvm::load_shim_view(home_, "", "mcpp", changed),
              std::nullopt);

    auto extra = fp;
    extra["/inputs/subos"] = xlings::xvm::ShimViewStat{1, 1};
    EXPECT_EQ(xlings::xvm::load_shim_view(home_, "", "mcpp", extra),
              std::nullopt);

    auto fewer = fp;
    fewer.clear();
    EXPECT_EQ(xlings::xvm::load_shim_view(home_, "", "mcpp", fewer),
              std::nullopt);

    // Identical fingerprint: hit.
    EXPECT_NE(xlings::xvm::load_shim_view(home_, "", "mcpp", fp),
              std::nullopt);
}

TEST_F(ShimViewTest, ViewMissesOnWrongProgramContextOrGarbage) {
    xlings::xvm::ShimView view;
    view.workspace["mcpp"] = "1.0";
    std::map<std::string, xlings::xvm::ShimViewStat> fp;
    fp["/inputs/home"] = xlings::xvm::ShimViewStat{10, 100};

    xlings::xvm::store_shim_view(home_, "", "mcpp", fp, view);

    // Same bytes, different program name: the file is keyed by name, and a
    // mismatch is a miss, never a hit from a file that happens to parse.
    EXPECT_EQ(xlings::xvm::load_shim_view(home_, "", "gcc", fp),
              std::nullopt);

    // Different project context: different cache key, miss.
    EXPECT_EQ(xlings::xvm::load_shim_view(home_, "deadbeef", "mcpp", fp),
              std::nullopt);

    // Corrupt the cache file: miss, not an exception.
    auto cachePath = home_ / ".shim-view" / "mcpp.json";
    xlings::platform::write_string_to_file(cachePath.string(), "{ oops");
    EXPECT_EQ(xlings::xvm::load_shim_view(home_, "", "mcpp", fp),
              std::nullopt);

    // A program name is one path component; anything carrying a separator
    // is refused outright, on store as well as load.
    xlings::xvm::store_shim_view(home_, "", "../evil", fp, view);
    std::error_code ec;
    EXPECT_FALSE(fs::is_regular_file(home_ / ".shim-view" / "..", ec));
    EXPECT_EQ(xlings::xvm::load_shim_view(home_, "", "../evil", fp),
              std::nullopt);
}

// ── save-side dual write ────────────────────────────────────────────
//
// save_versions needs a live Config, which pins this test to the shape the
// e2e suites already cover; here the CONTRACT is pinned instead: the DB
// file's wrapped `versions` IS the versions map, so versions_from_json
// consumes it unchanged and the fallback in load_versions_json stays
// lossless.
TEST_F(ShimViewTest, VersionsDbRootShapeIsTheVersionsMap) {
    writeHomeConfig(kHomeConfig);
    writeVersionsDbWrapper(dumpOf(R"({"solo": {"type": "program", "versions": {"2.0": {}}}})"));
    auto root = xlings::load_versions_json(home_);
    ASSERT_NE(root, std::nullopt);
    auto db = xlings::xvm::versions_from_json(*root);
    ASSERT_EQ(db.count("solo"), 1u);
    EXPECT_EQ(db["solo"].type, "program");
}

// The cache never stores a view resolved from a file written within the
// last two seconds (shim_dispatch's fresh-write suppression), so an
// equal-length rewrite landing in the same mtime tick cannot be laundered
// into a stale hit. The suppression lives in the dispatcher; here the
// ENCODING side is pinned: a stat pair round-trips through the fingerprint
// JSON, and the explicit "absent" mark reads back as the same sentinel.
TEST_F(ShimViewTest, FingerprintAbsentMarkRoundTrips) {
    xlings::xvm::ShimView view;
    view.workspace["mcpp"] = "1.0";
    std::map<std::string, xlings::xvm::ShimViewStat> fp;
    fp["/inputs/home"] = xlings::xvm::ShimViewStat{10, 100};
    fp["/inputs/gone"] = xlings::xvm::ShimViewStat{
        0, std::numeric_limits<std::int64_t>::min()};  // absent sentinel

    xlings::xvm::store_shim_view(home_, "", "mcpp", fp, view);
    // The absent mark is visible in the file as a word, not magic numbers.
    auto raw = xlings::platform::read_file_to_string(
        (home_ / ".shim-view" / "mcpp.json").string());
    EXPECT_NE(raw.find("\"absent\""), std::string::npos);

    // And a load whose fingerprint carries the same sentinel hits.
    auto loaded = xlings::xvm::load_shim_view(home_, "", "mcpp", fp);
    ASSERT_NE(loaded, std::nullopt);
    EXPECT_EQ(loaded->workspace.at("mcpp"), "1.0");
}

}  // namespace
