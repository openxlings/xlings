#include <gtest/gtest.h>

import std;
import xlings.platform;
import xlings.testkit;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace p = xlings::platform;

TEST(PlatformAtomicWrite, ReplacesAnExistingFileAndDoesNotOverwriteTheOldFixedScratchName) {
    auto home = tk::Home::isolated("atomic-file");
    const auto target = home.root() / "policy.json";
    const auto sentinel = home.root() / (".policy.json.xlings-tmp." + std::to_string(p::get_pid()));
    tk::write_file(target, "old complete content");
    tk::write_file(sentinel, "unowned scratch name");
    EXPECT_NO_THROW(p::write_file_atomic(target.string(), "new complete content"));
    EXPECT_EQ(tk::read_file(target), "new complete content");
    EXPECT_EQ(tk::read_file(sentinel), "unowned scratch name");
}

TEST(PlatformAtomicWrite, PublicationFailurePreservesThePreviousDestination) {
    auto home = tk::Home::isolated("atomic-failure");
    const auto target = home.root() / "policy.json";
    fs::create_directories(target);
    tk::write_file(target / "user-file", "old complete content");
    EXPECT_THROW(p::write_file_atomic(target.string(), "new content"), std::runtime_error);
    EXPECT_TRUE(fs::is_directory(target));
    EXPECT_EQ(tk::read_file(target / "user-file"), "old complete content");
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(home.root()))
        if (entry.path().filename().string().starts_with(".policy.json.xlings-tmp.")) ++entries;
    EXPECT_EQ(entries, 0u) << "only an owned, unpublished scratch may be cleaned";
}

TEST(PlatformAtomicWrite, FollowsAUserSymlinkWithoutReplacingTheLink) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "symlink privileges are not available on every Windows host";
    auto home = tk::Home::isolated("atomic-link");
    const auto original = home.root() / "user-rc";
    const auto link = home.root() / "linked-rc";
    tk::write_file(original, "old rc");
    fs::create_symlink(original.filename(), link);
    EXPECT_NO_THROW(p::write_file_atomic(link.string(), "new rc"));
    EXPECT_TRUE(fs::is_symlink(link));
    EXPECT_EQ(tk::read_file(original), "new rc");
}

TEST(PlatformAtomicWrite, PublicationWithoutReplacementPreservesEveryExistingDestination) {
    auto home = tk::Home::isolated("atomic-publish");
    const auto source = home.root() / "scratch";
    const auto existing = home.root() / "existing";
    tk::write_file(source, "new artefact");
    tk::write_file(existing, "old artefact");
    EXPECT_FALSE(p::rename_no_replace(source, existing));
    EXPECT_EQ(tk::read_file(source), "new artefact");
    EXPECT_EQ(tk::read_file(existing), "old artefact");
    const auto directory = home.root() / "empty-dir";
    const auto scratch_directory = home.root() / "scratch-dir";
    fs::create_directories(directory);
    fs::create_directories(scratch_directory);
    EXPECT_FALSE(p::rename_no_replace(scratch_directory, directory));
    EXPECT_TRUE(fs::is_directory(scratch_directory));
    EXPECT_TRUE(fs::is_empty(directory));
    if constexpr (tk::is_posix) {
        const auto dangling = home.root() / "dangling";
        fs::create_symlink("missing", dangling);
        EXPECT_FALSE(p::rename_no_replace(source, dangling));
        EXPECT_EQ(fs::read_symlink(dangling), "missing");
        EXPECT_EQ(tk::read_file(source), "new artefact");
    }
    const auto published = home.root() / "published";
    auto result = p::rename_no_replace(source, published);
    ASSERT_TRUE(result) << result.error();
    EXPECT_FALSE(fs::exists(source));
    EXPECT_EQ(tk::read_file(published), "new artefact");
}
