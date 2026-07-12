// Unit tests for the xlings.core.mirror module. Covers:
//   - URL classification heuristic
//   - Three URL-rewriting forms (prefix / host-replace / jsdelivr)
//   - expand() across modes (Auto / Off / Force) and resource types
//   - registry filtering by type and size
//   - Same-priority shuffle behavior

#include <gtest/gtest.h>

import std;
import xlings.core.mirror;

using namespace xlings::mirror;
namespace fs = std::filesystem;

namespace {

class MirrorTestEnvironment final : public testing::Environment {
public:
    void SetUp() override {
        home_ = fs::temp_directory_path()
            / std::format("xlings-mirror-test-{}",
                std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(home_);
#ifdef _WIN32
        _putenv_s("XLINGS_HOME", home_.string().c_str());
#else
        ::setenv("XLINGS_HOME", home_.string().c_str(), 1);
#endif
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(home_, ec);
    }

private:
    fs::path home_;
};

[[maybe_unused]] auto* mirror_test_environment_ =
    testing::AddGlobalTestEnvironment(new MirrorTestEnvironment);

// Helper: any item in the list whose URL starts with the given prefix.
bool any_starts_with(const std::vector<std::string>& urls, std::string_view prefix) {
    return std::ranges::any_of(urls, [&](const auto& u) {
        return u.starts_with(prefix);
    });
}

} // namespace

// ============================================================
// classify()
// ============================================================

TEST(MirrorClassify, ReleaseAsset) {
    EXPECT_EQ(classify("https://github.com/owner/repo/releases/download/v1.0/asset.tar.gz"),
              ResourceType::Release);
}

TEST(MirrorClassify, ObjectsHostIsRelease) {
    EXPECT_EQ(classify("https://objects.githubusercontent.com/..."),
              ResourceType::Release);
}

TEST(MirrorClassify, RawHost) {
    EXPECT_EQ(classify("https://raw.githubusercontent.com/owner/repo/main/file.txt"),
              ResourceType::Raw);
}

TEST(MirrorClassify, ArchiveByPath) {
    EXPECT_EQ(classify("https://github.com/owner/repo/archive/v1.tar.gz"),
              ResourceType::Archive);
}

TEST(MirrorClassify, CodeloadIsArchive) {
    EXPECT_EQ(classify("https://codeload.github.com/owner/repo/tar.gz/refs/heads/main"),
              ResourceType::Archive);
}

TEST(MirrorClassify, GitDotGitSuffix) {
    EXPECT_EQ(classify("https://github.com/owner/repo.git"),
              ResourceType::Git);
}

TEST(MirrorClassify, BareGithubIsUnknown) {
    // Without a /releases/, /archive/, or .git suffix, the URL is ambiguous.
    EXPECT_EQ(classify("https://github.com/owner/repo"),
              ResourceType::Unknown);
}

TEST(MirrorClassify, NonGithubIsUnknown) {
    EXPECT_EQ(classify("https://gitee.com/owner/repo"), ResourceType::Unknown);
    EXPECT_EQ(classify("https://example.com/foo.tar.gz"), ResourceType::Unknown);
}

// ============================================================
// is_github_url()
// ============================================================

TEST(MirrorIsGithub, ClassifiesGithubFamily) {
    EXPECT_TRUE(is_github_url("https://github.com/x/y"));
    EXPECT_TRUE(is_github_url("https://raw.githubusercontent.com/x/y/main/f"));
    EXPECT_TRUE(is_github_url("https://codeload.github.com/x/y/tar/v1"));
    EXPECT_TRUE(is_github_url("https://objects.githubusercontent.com/abc"));
    EXPECT_FALSE(is_github_url("https://gitee.com/x/y"));
    EXPECT_FALSE(is_github_url("https://example.com"));
    EXPECT_FALSE(is_github_url(""));
}

// ============================================================
// expand() — Off mode
// ============================================================

TEST(MirrorExpand, OffModeReturnsOnlyOriginal) {
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz",
                       {.type = ResourceType::Release, .mode = Mode::Off});
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://github.com/x/y/releases/download/v1/asset.tar.gz");
}

// ============================================================
// expand() — Auto mode
// ============================================================

TEST(MirrorExpand, AutoModeOriginalIsFirst) {
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz",
                       {.type = ResourceType::Release, .mode = Mode::Auto});
    ASSERT_FALSE(urls.empty());
    EXPECT_EQ(urls[0], "https://github.com/x/y/releases/download/v1/asset.tar.gz");
    EXPECT_GT(urls.size(), 1u) << "Auto mode should produce mirror fallbacks";
}

TEST(MirrorExpand, AutoModeProducesPrefixMirror) {
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz",
                       {.type = ResourceType::Release, .mode = Mode::Auto});
    EXPECT_TRUE(any_starts_with(urls, "https://ghfast.top/"));
}

TEST(MirrorExpand, AutoModeProducesHostReplaceMirror) {
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz",
                       {.type = ResourceType::Release, .mode = Mode::Auto});
    EXPECT_TRUE(any_starts_with(urls, "https://kkgithub.com/"));
}

TEST(MirrorExpand, RawUrlGetsJsdelivrMirror) {
    auto urls = expand("https://raw.githubusercontent.com/x/y/main/file.txt",
                       {.type = ResourceType::Raw, .mode = Mode::Auto});
    EXPECT_TRUE(any_starts_with(urls, "https://cdn.jsdelivr.net/gh/"));
}

TEST(MirrorExpand, GitTypeGetsGitMirrors) {
    auto urls = expand("https://github.com/x/y.git",
                       {.type = ResourceType::Git, .mode = Mode::Auto});
    // jsdelivr does NOT support git; only prefix + host-replace mirrors.
    EXPECT_FALSE(any_starts_with(urls, "https://cdn.jsdelivr.net/"));
    EXPECT_TRUE(any_starts_with(urls, "https://ghfast.top/"));
}

TEST(MirrorExpand, GitTypeFiltersOutJsdelivr) {
    // Even for a raw-shaped URL, if caller says it's git, jsdelivr is excluded
    // (jsdelivr doesn't handle git).
    auto urls = expand("https://github.com/x/y",
                       {.type = ResourceType::Git, .mode = Mode::Auto});
    EXPECT_FALSE(any_starts_with(urls, "https://cdn.jsdelivr.net/"));
}

// ============================================================
// expand() — Force mode
// ============================================================

TEST(MirrorExpand, ForceModeSkipsOriginal) {
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz",
                       {.type = ResourceType::Release, .mode = Mode::Force});
    EXPECT_FALSE(urls.empty());
    EXPECT_NE(urls[0], "https://github.com/x/y/releases/download/v1/asset.tar.gz");
}

// ============================================================
// expand() — non-GitHub passthrough
// ============================================================

TEST(MirrorExpand, GiteeUrlIsNotExpanded) {
    auto urls = expand("https://gitee.com/owner/repo/releases/download/v1/asset.tar.gz",
                       {.mode = Mode::Auto});
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0],
              "https://gitee.com/owner/repo/releases/download/v1/asset.tar.gz");
}

TEST(MirrorExpand, CustomHostIsNotExpanded) {
    auto urls = expand("https://internal.corp.com/foo.tar.gz",
                       {.mode = Mode::Auto});
    ASSERT_EQ(urls.size(), 1u);
}

TEST(MirrorExpand, EmptyUrlReturnsEmpty) {
    auto urls = expand("", {.mode = Mode::Auto});
    EXPECT_TRUE(urls.empty());
}

// ============================================================
// expand() — Unknown type doesn't expand
// ============================================================

TEST(MirrorExpand, UnknownTypeReturnsOnlyOriginal) {
    // Bare github.com URL with no hint → classify returns Unknown.
    auto urls = expand("https://github.com/x/y", {.mode = Mode::Auto});
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://github.com/x/y");
}

// ============================================================
// expand() — file size filtering
// ============================================================

TEST(MirrorExpand, LargeFileFiltersOutJsdelivr) {
    // jsdelivr has limit_bytes = 50 MB. A 60 MB raw file should not include it.
    auto urls = expand("https://raw.githubusercontent.com/x/y/main/big.bin",
                       {.type = ResourceType::Raw,
                        .mode = Mode::Auto,
                        .expected_size = 60u * 1024u * 1024u});
    EXPECT_FALSE(any_starts_with(urls, "https://cdn.jsdelivr.net/"));
}

TEST(MirrorExpand, SmallFileIncludesJsdelivr) {
    // 1 MB file is well under jsdelivr's 50 MB cap.
    auto urls = expand("https://raw.githubusercontent.com/x/y/main/small.txt",
                       {.type = ResourceType::Raw,
                        .mode = Mode::Auto,
                        .expected_size = 1024u * 1024u});
    EXPECT_TRUE(any_starts_with(urls, "https://cdn.jsdelivr.net/"));
}

TEST(MirrorExpand, UnknownSizeIncludesAllMirrors) {
    // expected_size = 0 means "unknown" — we must not filter by limit_bytes.
    auto urls = expand("https://raw.githubusercontent.com/x/y/main/file.txt",
                       {.type = ResourceType::Raw,
                        .mode = Mode::Auto,
                        .expected_size = 0});
    EXPECT_TRUE(any_starts_with(urls, "https://cdn.jsdelivr.net/"));
}

// ============================================================
// expand() — host-replace path rewriting for raw URLs
// ============================================================

TEST(MirrorExpand, HostReplaceRewritesRawPath) {
    auto urls = expand("https://raw.githubusercontent.com/x/y/main/path/file.txt",
                       {.type = ResourceType::Raw, .mode = Mode::Auto});
    // kkgithub host-replace must inject "/raw/" between repo and ref.
    auto kk_it = std::ranges::find_if(urls, [](const auto& u) {
        return u.starts_with("https://kkgithub.com/");
    });
    ASSERT_NE(kk_it, urls.end()) << "kkgithub mirror missing";
    EXPECT_EQ(*kk_it, "https://kkgithub.com/x/y/raw/main/path/file.txt");
}

TEST(MirrorExpand, JsdelivrRewriteShape) {
    auto urls = expand("https://raw.githubusercontent.com/x/y/main/path/file.txt",
                       {.type = ResourceType::Raw, .mode = Mode::Auto});
    auto js_it = std::ranges::find_if(urls, [](const auto& u) {
        return u.starts_with("https://cdn.jsdelivr.net/");
    });
    ASSERT_NE(js_it, urls.end());
    EXPECT_EQ(*js_it, "https://cdn.jsdelivr.net/gh/x/y@main/path/file.txt");
}

TEST(MirrorExpand, PrefixRewriteShape) {
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz",
                       {.type = ResourceType::Release, .mode = Mode::Auto});
    auto pref_it = std::ranges::find_if(urls, [](const auto& u) {
        return u.starts_with("https://ghfast.top/");
    });
    ASSERT_NE(pref_it, urls.end());
    EXPECT_EQ(*pref_it,
              "https://ghfast.top/https://github.com/x/y/releases/download/v1/asset.tar.gz");
}

// ============================================================
// expand() — no duplicates
// ============================================================

TEST(MirrorExpand, NoDuplicateUrls) {
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz",
                       {.type = ResourceType::Release, .mode = Mode::Auto});
    std::set<std::string> uniq(urls.begin(), urls.end());
    EXPECT_EQ(uniq.size(), urls.size()) << "expand() returned duplicate URLs";
}

// ============================================================
// expand() — defaults (no opts) classify automatically
// ============================================================

TEST(MirrorExpand, AutoClassifiesReleaseUrl) {
    set_mode(Mode::Auto);
    auto urls = expand("https://github.com/x/y/releases/download/v1/asset.tar.gz");
    ASSERT_FALSE(urls.empty());
    EXPECT_EQ(urls[0], "https://github.com/x/y/releases/download/v1/asset.tar.gz");
    EXPECT_GT(urls.size(), 1u);
}

// ============================================================
// Mode setter (test affordance)
// ============================================================

TEST(MirrorMode, SetModeAffectsCurrentMode) {
    auto saved = current_mode();
    set_mode(Mode::Off);
    EXPECT_EQ(current_mode(), Mode::Off);
    set_mode(Mode::Force);
    EXPECT_EQ(current_mode(), Mode::Force);
    set_mode(saved);
}
