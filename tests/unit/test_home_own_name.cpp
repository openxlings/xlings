// A home reached by another name is the home it was created as.
//
// Measured in mcpp#949 (2026-11-17): a test home whose `registry` was a symbolic
// link to the shared home installed a toolchain through it. The shared home's
// runtime generation was written with 52 links through the test home's path,
// and the interpreter bound into clang named it too; when the test home was
// removed, no compiler of the shared home started ("No such file or
// directory") until another install rewrote the generation. What a run writes
// for later names the home as the home names itself.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.abi;
import xlings.core.home_identity;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace hi = xlings::home_identity;

namespace {
fs::path marked_home(const tk::Home& home) {
    const auto h = home.dir();
    fs::create_directories(h);
    if (!hi::has_marker(h)) EXPECT_TRUE(hi::write_marker(h).has_value());
    return h;
}
}  // namespace

XTEST(HomeOwnName, TheHomeAnswersToThePathItWasCreatedAtWhileThatPathNamesIt,
      .area = "home", .covers = {"HOME-OWN-NAME"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("own-name");
    const auto h = marked_home(home);
    EXPECT_EQ(hi::own_name(h), h);

    const auto alias = home.root() / "another-name";
    fs::create_directory_symlink(h, alias);
    EXPECT_EQ(hi::own_name(alias), h) << "a link to the home is not its name";

    // A copy is another home: it has only the name it is reached by.
    const auto copy = home.root() / "a-copy";
    fs::copy(h, copy, fs::copy_options::recursive);
    EXPECT_EQ(hi::own_name(copy), copy);

    // Moved: the path it was created at names nothing.
    const auto moved = home.root() / "moved";
    fs::remove(alias);
    fs::rename(h, moved);
    EXPECT_EQ(hi::own_name(moved), moved);
    // A directory without a marker is named as it is reached.
    EXPECT_EQ(hi::own_name(home.root()), home.root());
}

XTEST(HomeOwnName, ARuntimeGenerationMadeThroughAnotherNameOutlivesThatName,
      .area = "xim", .covers = {"HOME-OWN-NAME", "RUNTIME-SCOPE"}, .requires_ = {"posix"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "the runtime indirection is Linux's";
    auto home = tk::Home::isolated("own-name-abi");
    const auto h = marked_home(home);
    const auto lib = h / "data/xpkgs/xim-x-glibc/2.44.3/lib64";
    tk::write_file(lib / "ld-linux-x86-64.so.2", "loader");
    tk::write_file(lib / "libc.so.6", "libc");

    const auto alias = home.root() / "another-name";
    fs::create_directory_symlink(h, alias);
    auto made = xlings::abi::refresh(alias, "install through another name");
    ASSERT_TRUE(made) << made.error();
    EXPECT_TRUE(made->changed);
    const auto link = h / "abi/linux-x86_64-glibc/libc.so.6";
    const auto target = fs::read_symlink(link).generic_string();
    EXPECT_TRUE(target.starts_with(h.generic_string())) << target;
    EXPECT_EQ(target.find("another-name"), std::string::npos) << target;

    // By its own name the home asks for the same links: nothing is written.
    auto same = xlings::abi::refresh(h, "again, by its own name");
    ASSERT_TRUE(same) << same.error();
    EXPECT_FALSE(same->changed) << "two names of one home do not make two runtimes";

    fs::remove(alias);
    EXPECT_EQ(tk::read_file(link), "libc") << "the runtime is reached after the other name is gone";
    EXPECT_EQ(tk::read_file(h / "abi/linux-x86_64-glibc/ld-linux-x86-64.so.2"), "loader");
}

XTEST(HomeOwnName, ACommandGivenAnotherNameActsOnTheHomeByItsOwn,
      .area = "home", .covers = {"HOME-OWN-NAME"}, .requires_ = {"posix", "xlings-bin"}) {
    auto home = tk::Home::isolated("own-name-cli");
    ASSERT_EQ(home.xlings({"subos", "list"}).exit_code, 0);
    const auto h = marked_home(home);
    const auto alias = home.root() / "another-name";
    fs::create_directory_symlink(h, alias);
    auto config = home.xlings({"self", "config"}, {{"XLINGS_HOME", alias.string()}});
    ASSERT_EQ(config.exit_code, 0) << config.transcript();
    // The home, its data and its subos are named by the home's own path; the
    // locator is still read where the run was pointed (the raw home).
    for (std::string_view key : {"XLINGS_HOME", "XLINGS_DATA", "XLINGS_SUBOS"}) {
        const auto at = config.out.find(key);
        ASSERT_NE(at, std::string::npos) << config.out;
        const auto line = config.out.substr(at, config.out.find('\n', at) - at);
        EXPECT_NE(line.find(h.string()), std::string::npos) << line;
        EXPECT_EQ(line.find("another-name"), std::string::npos) << line;
    }
}
