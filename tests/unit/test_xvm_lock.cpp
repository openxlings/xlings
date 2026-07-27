// tests/unit/test_xvm_lock.cpp — the per-home state lock and update_home_config.
//
// Split out of the former single 12.7k-line test_main.cpp. Section order
// and contents are unchanged; only the file boundary is new.

#include <gtest/gtest.h>
#include <iomanip>
#ifdef __unix__
#include <sys/wait.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>  // geteuid — AtomicWriteTest skips permission cases as root
#endif

import std;
import xlings.core.i18n;
import xlings.core.log;
import xlings.core.utils;
import xlings.ui;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.xim.installer;
import xlings.core.xim.commands;
import xlings.core.xim.repo;
import xlings.core.xim.extract;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.xvm.inspect;
import xlings.core.xvm.lock;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.core.xvm.commands;
import xlings.core.compact;
import xlings.core.config;
import xlings.core.home_config;
import xlings.platform;
import xlings.libs.json;
import xlings.core.xself;
import xlings.core.profile;
import xlings.core.subos.gpu;
import xlings.core.xim.downloader;
import xlings.runtime;
import xlings.capabilities;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import mcpplibs.cmdline;

namespace {

struct ScopedEnvVar {
    std::string name;
    bool had_prev{false};
    std::string prev_value;

    ScopedEnvVar(std::string_view key, std::string_view value) : name(key) {
        if (auto* prev = std::getenv(name.c_str())) {
            had_prev = true;
            prev_value = prev;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_prev) set(prev_value);
        else set("");
    }

    void set(std::string_view value) {
        xlings::platform::set_env_variable(name, std::string(value));
    }
};

std::optional<std::filesystem::path> find_pkgindex_repo() {
    namespace fs = std::filesystem;

    if (auto env = std::getenv("XIM_PKGINDEX_DIR")) {
        fs::path path(env);
        if (fs::exists(path / "pkgs")) return path;
    }

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures/xim-pkgindex",
        fs::current_path() / "../xim-pkgindex",
        fs::current_path() / "../d2learn/xim-pkgindex",
        fs::current_path() / "../../xim-pkgindex",
        fs::current_path() / "../../d2learn/xim-pkgindex",
    };

    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) return fs::weakly_canonical(path, ec);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> find_fixture_repo(std::string_view name) {
    namespace fs = std::filesystem;

    const std::vector<fs::path> candidates = {
        fs::current_path() / "tests/fixtures" / name,
        fs::current_path() / "../../tests/fixtures" / name,
    };
    for (auto& path : candidates) {
        std::error_code ec;
        if (fs::exists(path / "pkgs", ec)) {
            return fs::weakly_canonical(path, ec);
        }
    }
    return std::nullopt;
}

}  // namespace

// ============================================================
// State lock — install/remove/use must not overwrite each other

namespace {

std::filesystem::path lock_test_home_(std::string_view name) {
    namespace fs = std::filesystem;
    auto home = fs::temp_directory_path() / ("xlings_lock_" + std::string(name));
    std::error_code ec;
    fs::remove_all(home, ec);
    fs::create_directories(home);
    return home;
}

}  // namespace

// Windows will not delete a file that is still open, so every lock must be
// out of scope before the directory is removed -- and the removal itself
// must not throw, or a stray handle turns cleanup into a test failure that
// says nothing about the lock. (POSIX allows unlinking an open file, which
// is why this only ever showed up on Windows.)
void drop_lock_home_(const std::filesystem::path& home) {
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

TEST(XvmStateLock, IsHeldForTheHomeNotTheSubos) {
    const auto home = lock_test_home_("scope");
    // The version database is shared by every subos under a home, so the
    // lock has to live at the home root to protect it.
    EXPECT_EQ(xlings::xvm::state_lock_path(home), home / ".xlings.lock");
    drop_lock_home_(home);
}

TEST(XvmStateLock, AnUnrelatedHolderIsExcludedWithAnActionableMessage) {
    const auto home = lock_test_home_("contended");
    {
        auto first = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        ASSERT_TRUE(first.has_value()) << first.error();
        EXPECT_TRUE(first->held());
        EXPECT_FALSE(first->bypassed());
        EXPECT_FALSE(first->inherited());

        // Re-entry is by design for a child of the holder, so clear the
        // marker the holder exported to stand in for an unrelated process:
        // one that shares nothing but the home directory.
        xlings::platform::set_env_variable("XLINGS_STATE_LOCK_HELD", "");

        auto other = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        ASSERT_FALSE(other.has_value())
            << "the lock did not exclude an unrelated holder";
        // The message has to be actionable: what is happening and what to do.
        EXPECT_NE(other.error().find("another xlings process"),
                  std::string::npos);
        EXPECT_NE(other.error().find("XLINGS_NO_LOCK"), std::string::npos);
    }
    drop_lock_home_(home);
}

TEST(XvmStateLock, ReleasesWhenTheHolderGoesOutOfScope) {
    const auto home = lock_test_home_("released");
    {
        auto held = xlings::xvm::acquire_state_lock(home, std::chrono::milliseconds{50});
        ASSERT_TRUE(held.has_value()) << held.error();
    }
    {
        auto again = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        EXPECT_TRUE(again.has_value())
            << "the lock outlived its holder: " << again.error();
    }
    drop_lock_home_(home);
}

// Recipes shell out to xlings from their hooks -- d2mcpp's config hook runs
// `xlings install`. The child would otherwise wait 30s for a lock its own
// parent is holding and cannot release until the child returns.
TEST(XvmStateLock, AChildOfTheHolderProceedsWithoutWaiting) {
    const auto home = lock_test_home_("reentrant");
    {
        auto outer = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        ASSERT_TRUE(outer.has_value()) << outer.error();
        EXPECT_FALSE(outer->inherited());

        // Standing in for a spawned child: it inherits the marker the
        // holder exported.
        const auto start = std::chrono::steady_clock::now();
        auto nested = xlings::xvm::acquire_state_lock(
            home, std::chrono::seconds{30});
        const auto waited = std::chrono::steady_clock::now() - start;

        ASSERT_TRUE(nested.has_value())
            << "a child of the holder deadlocked: " << nested.error();
        EXPECT_TRUE(nested->inherited());
        EXPECT_TRUE(nested->held());
        EXPECT_LT(waited, std::chrono::seconds{5}) << "the child waited";
    }
    // The marker must not outlive the holder, or the next unrelated command
    // in this process would silently skip locking.
    {
        auto after = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        ASSERT_TRUE(after.has_value()) << after.error();
        EXPECT_FALSE(after->inherited())
            << "the re-entrancy marker outlived the lock that set it";
    }
    drop_lock_home_(home);
}

// The marker names the home it covers. A child working on a different home
// must still lock it -- inheriting a bare flag would leave that home
// unlocked while nobody holds it.
TEST(XvmStateLock, InheritanceDoesNotLeakToAnotherHome) {
    const auto held = lock_test_home_("scoped-held");
    const auto other = lock_test_home_("scoped-other");
    {
        auto outer = xlings::xvm::acquire_state_lock(
            held, std::chrono::milliseconds{50});
        ASSERT_TRUE(outer.has_value()) << outer.error();

        // Same home: covered by the ancestor.
        auto sameHome = xlings::xvm::acquire_state_lock(
            held, std::chrono::milliseconds{50});
        ASSERT_TRUE(sameHome.has_value()) << sameHome.error();
        EXPECT_TRUE(sameHome->inherited());

        // Different home: must take a lock of its own, not inherit.
        auto otherHome = xlings::xvm::acquire_state_lock(
            other, std::chrono::milliseconds{50});
        ASSERT_TRUE(otherHome.has_value()) << otherHome.error();
        EXPECT_FALSE(otherHome->inherited())
            << "a different home inherited a lock nobody holds for it";
        EXPECT_TRUE(std::filesystem::exists(
            xlings::xvm::state_lock_path(other)));
    }
    drop_lock_home_(held);
    drop_lock_home_(other);
}

// Locking a second home while holding the first must not erase the first
// one's marker, or a later child of the first would try to re-lock it.
TEST(XvmStateLock, NestingRestoresTheOuterMarker) {
    const auto outerHome = lock_test_home_("nest-outer");
    const auto innerHome = lock_test_home_("nest-inner");
    {
        auto outer = xlings::xvm::acquire_state_lock(
            outerHome, std::chrono::milliseconds{50});
        ASSERT_TRUE(outer.has_value()) << outer.error();
        {
            auto inner = xlings::xvm::acquire_state_lock(
                innerHome, std::chrono::milliseconds{50});
            ASSERT_TRUE(inner.has_value()) << inner.error();
            EXPECT_FALSE(inner->inherited());
        }
        auto childOfOuter = xlings::xvm::acquire_state_lock(
            outerHome, std::chrono::milliseconds{50});
        ASSERT_TRUE(childOfOuter.has_value())
            << "the outer marker was lost: " << childOfOuter.error();
        EXPECT_TRUE(childOfOuter->inherited());
    }
    drop_lock_home_(outerHome);
    drop_lock_home_(innerHome);
}

TEST(XvmStateLock, LockFileContentsAreNeverRewritten) {
    const auto home = lock_test_home_("inode");
    const auto path = xlings::xvm::state_lock_path(home);
    {
        auto held = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        ASSERT_TRUE(held.has_value()) << held.error();

        // Writing anything to the lock file through the normal path would
        // rename a new inode over it, leaving the lock name unlocked while
        // this process still believes it holds the lock. Nothing may be
        // written, and in particular the file must stay empty.
        ASSERT_TRUE(std::filesystem::exists(path));
        EXPECT_EQ(std::filesystem::file_size(path), 0u)
            << "something wrote to the lock file; the lock is held on the "
               "old inode";
    }
    drop_lock_home_(home);
}

// ============================================================
// update_home_config — one file, several owners
//
// `~/.xlings.json` carries `versions` and `workspace` (install/remove/use),
// `subos` and `activeSubos` (the subos commands), `lang` and `mirror`
// (`xlings config`), `index_repos` (the MCP repo capabilities) and `version`
// (`self install`). Every one of them rewrites the whole document.
//
// install/remove/use took the state lock; nobody else did. So a subos
// command that read the config, spent seconds in mkfs.ext4 and wrote back
// what it read would put the file back to its pre-install state -- with the
// installed payload still on disk and no record of it.
// ============================================================

namespace {

std::filesystem::path home_config_test_home_(const char* name) {
    namespace fs = std::filesystem;
    auto home = fs::temp_directory_path()
        / ("xlings_homecfg_" + std::string(name));
    std::error_code ec;
    fs::remove_all(home, ec);
    fs::create_directories(home);
    // Locks are re-entrant for a child of the holder; an earlier test in this
    // binary may have left the marker set. Clear it so each case starts as an
    // unrelated process.
    xlings::platform::set_env_variable("XLINGS_STATE_LOCK_HELD", "");
    return home;
}

void write_home_config_(const std::filesystem::path& home,
                        const nlohmann::json& json) {
    xlings::platform::write_string_to_file(
        (home / ".xlings.json").string(), json.dump(2));
}

}  // namespace

TEST(HomeConfig, ReadsAMissingOrCorruptConfigAsEmpty) {
    const auto home = home_config_test_home_("read");
    EXPECT_TRUE(xlings::read_home_config(home).empty());

    xlings::platform::write_string_to_file(
        (home / ".xlings.json").string(), "{not json");
    EXPECT_TRUE(xlings::read_home_config(home).empty())
        << "a corrupt config must read as no config, not throw";

    // A JSON document that is valid but not an object is equally unusable as
    // a config: every caller indexes it by key.
    xlings::platform::write_string_to_file(
        (home / ".xlings.json").string(), "[1, 2, 3]");
    EXPECT_TRUE(xlings::read_home_config(home).empty());

    drop_lock_home_(home);
}

// The regression this module exists for. A caller reads the config, does slow
// work, and commits. Anything another process wrote in between must survive,
// and the caller must not resurrect the values it read at the start.
TEST(HomeConfig, CommitsAgainstTheDocumentAsItIsNowNotAsItWasRead) {
    const auto home = home_config_test_home_("lost_update");
    write_home_config_(home, nlohmann::json{
        {"versions", {{"gcc", "15.1.0"}}},
    });

    // What a subos command would have read before starting its slow work.
    const auto stale = xlings::read_home_config(home);
    ASSERT_EQ(stale["versions"]["gcc"], "15.1.0");

    // An install commits while that slow work is running.
    write_home_config_(home, nlohmann::json{
        {"versions", {{"gcc", "16.1.0"}}},
        {"workspace", {{"active", {{"gcc", "16.1.0"}}}}},
    });

    auto committed = xlings::update_home_config(
        home, [](nlohmann::json& json) {
            json["subos"]["sandbox"] = {{"dir", ""}};
            return true;
        });
    ASSERT_TRUE(committed.has_value()) << committed.error();
    EXPECT_TRUE(*committed);

    const auto after = xlings::read_home_config(home);
    EXPECT_EQ(after["subos"]["sandbox"]["dir"], "");
    EXPECT_EQ(after["versions"]["gcc"], "16.1.0")
        << "the update reverted `versions` to the value it read before the "
           "install committed";
    EXPECT_TRUE(after.contains("workspace"))
        << "`workspace` was dropped; the version database and the workspace "
           "are two halves of one release and cannot diverge";

    drop_lock_home_(home);
}

TEST(HomeConfig, DecliningToCommitLeavesTheFileByteForByte) {
    const auto home = home_config_test_home_("declined");
    write_home_config_(home, nlohmann::json{{"lang", "zh"}});
    const auto before = xlings::platform::read_file_to_string(
        (home / ".xlings.json").string());

    auto committed = xlings::update_home_config(
        home, [](nlohmann::json& json) {
            json["lang"] = "en";  // discarded: the callback declines
            return false;
        });
    ASSERT_TRUE(committed.has_value()) << committed.error();
    EXPECT_FALSE(*committed);

    EXPECT_EQ(xlings::platform::read_file_to_string(
                  (home / ".xlings.json").string()),
              before)
        << "a declined update rewrote the file anyway";

    drop_lock_home_(home);
}

// The mutation must not run at all while another process holds the lock --
// otherwise the re-read it depends on is a re-read of a document that is
// being replaced underneath it.
TEST(HomeConfig, RefusesWhileAnUnrelatedProcessHoldsTheLock) {
    const auto home = home_config_test_home_("contended");
    write_home_config_(home, nlohmann::json{{"lang", "zh"}});

    {
        auto held = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        ASSERT_TRUE(held.has_value()) << held.error();

        // Stand in for an unrelated process: drop the re-entrancy marker the
        // holder exported, so the update below is not treated as its child.
        xlings::platform::set_env_variable("XLINGS_STATE_LOCK_HELD", "");

        bool ran = false;
        auto committed = xlings::update_home_config(
            home,
            [&](nlohmann::json& json) { ran = true; json["lang"] = "en"; return true; },
            std::chrono::milliseconds{50});
        ASSERT_FALSE(committed.has_value())
            << "the update did not take the state lock";
        EXPECT_NE(committed.error().find("another xlings process"),
                  std::string::npos);
        EXPECT_FALSE(ran) << "the mutation ran without the lock";
    }

    EXPECT_EQ(xlings::read_home_config(home)["lang"], "zh");
    drop_lock_home_(home);
}

// A command that legitimately runs under an ancestor's lock -- a recipe hook
// shelling out to xlings -- still gets to commit.
TEST(HomeConfig, CommitsUnderAnAncestorsLock) {
    const auto home = home_config_test_home_("reentrant");
    {
        auto outer = xlings::xvm::acquire_state_lock(
            home, std::chrono::milliseconds{50});
        ASSERT_TRUE(outer.has_value()) << outer.error();

        auto committed = xlings::update_home_config(
            home,
            [](nlohmann::json& json) { json["lang"] = "en"; return true; },
            std::chrono::milliseconds{50});
        ASSERT_TRUE(committed.has_value()) << committed.error();
        EXPECT_TRUE(*committed);
    }
    EXPECT_EQ(xlings::read_home_config(home)["lang"], "en");
    drop_lock_home_(home);
}
