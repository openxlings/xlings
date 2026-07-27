// tests/unit/test_platform_atomic.cpp — platform::write_string_to_file — atomic replace.
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
// platform::write_string_to_file — atomic replace
//
// Every persisted xlings state file goes through this function:
// ~/.xlings.json (the whole version database), each subos workspace,
// profile generations, and the shell rc files that xself edits by
// read-modify-write. It used to be fopen("w") + fwrite + fclose, which
// truncates the destination before any new byte is written — an
// interruption at that point loses the file's entire previous content.
// These tests pin the replace-or-leave-untouched contract.

namespace {

class AtomicWriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        dir_ = fs::temp_directory_path() /
               ("xlings_atomic_write_" +
                std::string(::testing::UnitTest::GetInstance()
                                ->current_test_info()
                                ->name()));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
#if !defined(_WIN32)
        // A test may leave the directory unwritable; restore before cleanup.
        std::filesystem::permissions(dir_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
#endif
        std::filesystem::remove_all(dir_, ec);
    }

    // Anything left beside the destination is a leaked staging file.
    std::size_t entry_count() const {
        std::size_t n = 0;
        for ([[maybe_unused]] auto& e : std::filesystem::directory_iterator(dir_)) ++n;
        return n;
    }

    std::filesystem::path dir_;
};

}  // namespace

TEST_F(AtomicWriteTest, WritesContentAndLeavesNoStagingFile) {
    auto target = dir_ / "state.json";
    xlings::platform::write_string_to_file(target.string(), "{\"a\":1}");

    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "{\"a\":1}");
    EXPECT_EQ(entry_count(), 1u) << "staging file leaked into the directory";
}

TEST_F(AtomicWriteTest, OverwriteReplacesContentAndLeavesNoStagingFile) {
    auto target = dir_ / "state.json";
    xlings::platform::write_string_to_file(target.string(), "old-and-longer");
    xlings::platform::write_string_to_file(target.string(), "new");

    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "new");
    EXPECT_EQ(entry_count(), 1u);
}

TEST_F(AtomicWriteTest, FailedWriteLeavesPreviousContentIntact) {
#if defined(_WIN32)
    GTEST_SKIP() << "directory permission semantics differ on Windows";
#else
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses directory permission checks";
    }
    namespace fs = std::filesystem;
    auto target = dir_ / "state.json";
    xlings::platform::write_string_to_file(target.string(), "committed");

    // Staging cannot be created in a non-writable directory. The point of the
    // test is what happens to the destination when the write cannot complete:
    // it must still hold the previously committed bytes. The old truncating
    // implementation would have destroyed them before discovering the failure.
    std::error_code ec;
    fs::permissions(dir_, fs::perms::owner_write, fs::perm_options::remove, ec);
    ASSERT_FALSE(ec);

    EXPECT_THROW(
        xlings::platform::write_string_to_file(target.string(), "replacement"),
        std::runtime_error);

    fs::permissions(dir_, fs::perms::owner_write, fs::perm_options::add, ec);
    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "committed");
    EXPECT_EQ(entry_count(), 1u);
#endif
}

TEST_F(AtomicWriteTest, ReplacesTheInodeInsteadOfMutatingTheCommittedOne) {
#if defined(_WIN32)
    GTEST_SKIP() << "hard link semantics differ on Windows";
#else
    namespace fs = std::filesystem;
    auto target = dir_ / "state.json";
    auto witness = dir_ / "witness.json";
    xlings::platform::write_string_to_file(target.string(), "committed");

    // The witness shares the destination's inode. If the write mutated that
    // inode in place -- truncate, then fill -- the witness would follow along,
    // which is exactly the window where an interruption leaves an empty file.
    // An atomic replace writes a *new* inode and renames it over the name, so
    // the committed bytes are never touched.
    std::error_code ec;
    fs::create_hard_link(target, witness, ec);
    ASSERT_FALSE(ec) << "cannot hard link on this filesystem: " << ec.message();

    xlings::platform::write_string_to_file(target.string(), "replacement");

    EXPECT_EQ(xlings::platform::read_file_to_string(target.string()), "replacement");
    EXPECT_EQ(xlings::platform::read_file_to_string(witness.string()), "committed")
        << "the committed inode was mutated in place";
#endif
}

TEST_F(AtomicWriteTest, PreservesExistingPermissionBits) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX permission bits do not apply on Windows";
#else
    namespace fs = std::filesystem;
    auto target = dir_ / "hook.sh";
    xlings::platform::write_string_to_file(target.string(), "#!/bin/sh\n");

    std::error_code ec;
    fs::permissions(target, fs::perms::owner_all | fs::perms::group_read |
                                fs::perms::others_read,
                    fs::perm_options::replace, ec);
    ASSERT_FALSE(ec);
    const auto before = fs::status(target).permissions();

    xlings::platform::write_string_to_file(target.string(), "#!/bin/sh\necho hi\n");

    // Replacing through a fresh staging file must not silently drop the
    // executable bit (or widen the mode) of the file it replaces.
    EXPECT_EQ(fs::status(target).permissions(), before);
#endif
}

TEST_F(AtomicWriteTest, WritesThroughSymlinkInsteadOfReplacingIt) {
#if defined(_WIN32)
    GTEST_SKIP() << "symlink semantics differ on Windows";
#else
    namespace fs = std::filesystem;
    auto real = dir_ / "real.rc";
    auto link = dir_ / "link.rc";
    xlings::platform::write_string_to_file(real.string(), "original\n");
    std::error_code ec;
    fs::create_symlink(real, link, ec);
    ASSERT_FALSE(ec);

    xlings::platform::write_string_to_file(link.string(), "updated\n");

    // xself edits shell rc files by read-modify-write. Those are commonly
    // symlinks into a dotfiles repo; a rename-based replace that did not
    // resolve the link would swap the symlink for a regular file and
    // silently detach the user's dotfiles.
    EXPECT_TRUE(fs::is_symlink(link)) << "symlink was replaced by a regular file";
    EXPECT_EQ(xlings::platform::read_file_to_string(real.string()), "updated\n");
#endif
}

TEST_F(AtomicWriteTest, ThrowsWhenParentDirectoryIsMissing) {
    auto target = dir_ / "nope" / "state.json";
    EXPECT_THROW(xlings::platform::write_string_to_file(target.string(), "x"),
                 std::runtime_error);
    EXPECT_EQ(entry_count(), 0u) << "staging file leaked on failure";
}
