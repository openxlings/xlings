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

// Every filesystem a home may be on publishes without replacing (design
// 2026-10-15-filesystem-capabilities): each strategy, chosen by what the
// filesystem was measured to do, is driven here on this one by stating the
// capabilities -- the NFSv3 lane proves the measurement on the real thing.
XTEST(AtomicPublication, EveryStrategyPublishesAndNoneReplaces,
      .area = "platform", .covers = {"ATOMIC-PUBLICATION"}) {
    auto home = tk::Home::isolated("publication-strategies");
    const auto root = home.root();
    p::FsCaps noFlag;                         // NFSv3: no rename flags, hard links
    noFlag.rename_no_replace = false;
    noFlag.rename_exchange = false;
    p::FsCaps noLinks = noFlag;               // and no hard links either
    noLinks.hard_links = false;
    for (const auto& [label, caps] : {std::pair{"native", p::FsCaps{}}, std::pair{"no-flag", noFlag},
                                      std::pair{"no-links", noLinks}}) {
        if (std::string_view(label) == "native" && !p::fs_caps(root).rename_no_replace) continue;
        const auto dir = root / label;
        fs::create_directories(dir);
        tk::write_file(dir / "file-scratch", "bytes");
        tk::write_file(dir / "taken", "old");
        EXPECT_FALSE(p::publish_no_replace(dir / "file-scratch", dir / "taken", caps)) << label;
        EXPECT_EQ(tk::read_file(dir / "taken"), "old") << label;
        ASSERT_TRUE(p::publish_no_replace(dir / "file-scratch", dir / "file", caps)) << label;
        EXPECT_FALSE(fs::exists(dir / "file-scratch")) << label;
        EXPECT_EQ(tk::read_file(dir / "file"), "bytes") << label;
        if constexpr (tk::is_posix) {         // a symbolic link is published as itself
            fs::create_symlink("/nowhere/target", dir / "link-scratch");
            EXPECT_FALSE(p::publish_no_replace(dir / "link-scratch", dir / "taken", caps)) << label;
            EXPECT_EQ(tk::read_file(dir / "taken"), "old") << label;
            ASSERT_TRUE(p::publish_no_replace(dir / "link-scratch", dir / "link", caps)) << label;
            EXPECT_EQ(fs::read_symlink(dir / "link"), "/nowhere/target") << label;
        }
        fs::create_directories(dir / "dir-scratch/inside");
        fs::create_directories(dir / "full/kept");
        fs::create_directories(dir / "empty");
        EXPECT_FALSE(p::publish_no_replace(dir / "dir-scratch", dir / "full", caps)) << label;
        EXPECT_TRUE(fs::is_directory(dir / "full/kept")) << label;
        EXPECT_FALSE(p::publish_no_replace(dir / "dir-scratch", dir / "empty", caps))
            << label << ": an existing name, even an empty directory, is refused";
        ASSERT_TRUE(p::publish_no_replace(dir / "dir-scratch", dir / "dir", caps)) << label;
        EXPECT_TRUE(fs::is_directory(dir / "dir/inside")) << label;
    }
}

// The measurement: what this test's own filesystem does, found by trying.
XTEST(AtomicPublication, AFilesystemIsMeasuredWhereItIs,
      .area = "platform", .covers = {"ATOMIC-PUBLICATION"}) {
    auto home = tk::Home::isolated("fs-caps");
    const auto caps = p::probe_fs_caps(home.root());
    EXPECT_TRUE(caps.measured);
    if constexpr (tk::is_posix) {
        EXPECT_TRUE(caps.symlinks);
        EXPECT_TRUE(caps.hard_links);
    }
    if constexpr (tk::is_linux) EXPECT_FALSE(caps.type.empty());
    for (const auto& e : fs::directory_iterator(home.root()))
        EXPECT_FALSE(e.path().filename().string().starts_with(".xlings-fsprobe-")) << "the probe left " << e.path();
}
