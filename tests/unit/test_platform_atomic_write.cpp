#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

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

// Where the rename flag is missing (NFSv3 answers EINVAL; mcpp#809), a file
// or a link is published as a hard link: the same refusal of an existing name,
// the scratch name gone, a symbolic link published as itself. A directory is
// renamed onto a name that is not there, never over what is.
XTEST(AtomicPublication, WithoutTheRenameFlagEveryEntryIsStillPublishedWithoutReplacing,
      .area = "platform", .covers = {"ATOMIC-PUBLICATION"}, .requires_ = {"posix"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "link(2)";
    auto home = tk::Home::isolated("link-publication");
    const auto file = home.root() / "file-scratch";
    tk::write_file(file, "bytes");
    const auto taken = home.root() / "taken";
    tk::write_file(taken, "old");
    EXPECT_FALSE(p::link_no_replace(file, taken));
    EXPECT_EQ(tk::read_file(taken), "old");
    ASSERT_TRUE(p::link_no_replace(file, home.root() / "file"));
    EXPECT_FALSE(fs::exists(file));
    EXPECT_EQ(tk::read_file(home.root() / "file"), "bytes");
    if constexpr (tk::is_linux) {   // Linux's link(2) never follows a symbolic link
        const auto link = home.root() / "link-scratch";
        fs::create_symlink("/nowhere/target", link);
        ASSERT_TRUE(p::link_no_replace(link, home.root() / "link"));
        EXPECT_EQ(fs::read_symlink(home.root() / "link"), "/nowhere/target");
        EXPECT_FALSE(fs::exists(fs::symlink_status(link)));
    }
    const auto directory = home.root() / "dir-scratch";
    fs::create_directories(directory / "inside");
    const auto full = home.root() / "full";
    fs::create_directories(full / "kept");
    EXPECT_FALSE(p::link_no_replace(directory, full));
    EXPECT_TRUE(fs::is_directory(full / "kept"));
    const auto empty = home.root() / "empty";
    fs::create_directories(empty);
    EXPECT_FALSE(p::link_no_replace(directory, empty)) << "an existing name, even an empty directory, is refused";
    ASSERT_TRUE(p::link_no_replace(directory, home.root() / "dir"));
    EXPECT_TRUE(fs::is_directory(home.root() / "dir/inside"));
    EXPECT_FALSE(fs::exists(directory));
}
