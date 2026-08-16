// Unit tests for reading which index snapshot answered a question.
//
// `not found` and `not found YET` were the same string. The revision that
// distinguishes them is read straight out of the index directory, and the
// reader shipped in #552 had no tests at all -- which is how it acquired a
// CRLF bug on one of its two read paths and stayed blind to the
// artifact-managed index shape entirely.
//
// It is a pure function over a directory, so the fixtures are cheap. That was
// true when it was written too; the reason it had no tests is that it lived
// in a .cpp where nothing could reach it.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

import std;
import xlings.core.xim.repo;

namespace xim = xlings::xim;
namespace fs = std::filesystem;

namespace {

// A directory shaped like a synced index, with whatever git bookkeeping the
// case under test needs.
struct RepoFixture {
    fs::path dir;

    explicit RepoFixture(std::string_view tag) {
        dir = fs::temp_directory_path() / ("xlings-repo-rev-" + std::string(tag));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / ".git", ec);
    }
    ~RepoFixture() { std::error_code ec; fs::remove_all(dir, ec); }
    RepoFixture(const RepoFixture&) = delete;
    RepoFixture& operator=(const RepoFixture&) = delete;

    void write(const fs::path& rel, std::string_view body) const {
        std::error_code ec;
        fs::create_directories((dir / rel).parent_path(), ec);
        std::ofstream out(dir / rel, std::ios::binary);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
};

constexpr std::string_view kSha = "288efe5a1b2c3d4e5f60718293a4b5c6d7e8f901";

}  // namespace

TEST(RepoRevision, ReadsALooseRef) {
    RepoFixture fx{"loose"};
    fx.write(".git/HEAD", "ref: refs/heads/main\n");
    fx.write(".git/refs/heads/main", std::string(kSha) + "\n");

    EXPECT_EQ(xim::get_repo_head_hash(fx.dir), kSha);
    EXPECT_EQ(xim::get_repo_revision_label(fx.dir), "288efe5");
}

TEST(RepoRevision, FallsBackToPackedRefs) {
    RepoFixture fx{"packed"};
    fx.write(".git/HEAD", "ref: refs/heads/main\n");
    fx.write(".git/packed-refs",
             "# pack-refs with: peeled fully-peeled sorted \n"
             + std::string(kSha) + " refs/heads/main\n"
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa refs/remotes/origin/main\n");

    EXPECT_EQ(xim::get_repo_head_hash(fx.dir), kSha);
}

// THE regression. The two read paths in this function stripped `\r`
// differently, and packed-refs compared on an exact length -- so one CRLF
// turned a readable revision into "no revision", and the caller silently
// printed the older, less useful message.
TEST(RepoRevision, PackedRefsWithCrlfStillResolves) {
    RepoFixture fx{"packed-crlf"};
    fx.write(".git/HEAD", "ref: refs/heads/main\r\n");
    fx.write(".git/packed-refs",
             "# pack-refs with: peeled fully-peeled sorted \r\n"
             + std::string(kSha) + " refs/heads/main\r\n");

    EXPECT_EQ(xim::get_repo_head_hash(fx.dir), kSha)
        << "a CRLF packed-refs must not read as 'no revision'";
}

// A ref whose name is a PREFIX of another must not match it.
TEST(RepoRevision, PackedRefsMatchesTheWholeRefName) {
    RepoFixture fx{"packed-prefix"};
    fx.write(".git/HEAD", "ref: refs/heads/main\n");
    fx.write(".git/packed-refs",
             "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb refs/heads/maintenance\n"
             + std::string(kSha) + " refs/heads/main\n");

    EXPECT_EQ(xim::get_repo_head_hash(fx.dir), kSha);
}

TEST(RepoRevision, DetachedHeadIsAlreadyTheSha) {
    RepoFixture fx{"detached"};
    fx.write(".git/HEAD", std::string(kSha) + "\n");
    EXPECT_EQ(xim::get_repo_head_hash(fx.dir), kSha);
}

// The shape a packaged index install produces: no `.git` at all. A reader
// that only knows git returns nothing here and takes the whole feature with
// it -- which is exactly what the replaced one did.
TEST(RepoRevision, AnArtifactIndexHasNoGitAndStillHasAnIdentity) {
    RepoFixture fx{"artifact"};
    std::error_code ec;
    fs::remove_all(fx.dir / ".git", ec);
    fx.write(".xlings-index-version", "2026.8.14.1\n");

    EXPECT_EQ(xim::get_repo_head_hash(fx.dir), "artifact:2026.8.14.1");
    EXPECT_EQ(xim::get_repo_revision_label(fx.dir), "artifact:2026.8.14.1")
        << "an artifact identity is short already and must not be truncated "
           "into something that looks like a sha";
}

TEST(RepoRevision, ADirectoryThatIsNeitherAnswersNothing) {
    RepoFixture fx{"neither"};
    std::error_code ec;
    fs::remove_all(fx.dir / ".git", ec);
    EXPECT_TRUE(xim::get_repo_head_hash(fx.dir).empty());
    EXPECT_TRUE(xim::get_repo_revision_label(fx.dir).empty());
}

// A dangling symbolic ref is "we could not read one", not a revision. The
// truncating label must not turn `ref: re` into something sha-shaped.
TEST(RepoRevision, AnUnresolvableRefIsNotARevision) {
    RepoFixture fx{"dangling"};
    fx.write(".git/HEAD", "ref: refs/heads/gone\n");
    EXPECT_TRUE(xim::get_repo_head_hash(fx.dir).empty());
}

// Staleness, not identity -- and only this can decide whether `xlings update`
// is useful advice. A directory with no timestamps to read answers -1, which
// is "unknown" rather than "fresh".
TEST(RepoRevision, SyncAgeIsUnknownWhenThereIsNothingToRead) {
    RepoFixture fx{"age-none"};
    std::error_code ec;
    fs::remove_all(fx.dir, ec);
    EXPECT_EQ(xim::get_repo_sync_age_seconds(fx.dir), -1);
}

TEST(RepoRevision, SyncAgeReadsAFetchThatJustHappened) {
    RepoFixture fx{"age-fresh"};
    fx.write(".git/FETCH_HEAD", std::string(kSha) + "\tbranch 'main'\n");
    const auto age = xim::get_repo_sync_age_seconds(fx.dir);
    EXPECT_GE(age, 0);
    EXPECT_LT(age, 300) << "a file written moments ago read as " << age << "s old";
}
