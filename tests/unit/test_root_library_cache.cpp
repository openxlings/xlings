#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.library_cache;

namespace tk = xlings::testkit;
namespace lc = xlings::subos::library_cache;

XTEST(RootLibraryCache, GeneratorSeesTheRootReadOnlyAndWritesOnlyOwnedStaging,
      .area = "subos", .covers = {"ROOT-LDCACHE"}) {
    const auto argv = lc::command("/owned/root", {"/owned/home"}, "/owned/staging", "/measured/bwrap");
    const auto has = [&](std::initializer_list<std::string> words) {
        return std::ranges::search(argv, words).begin() != argv.end();
    };
    EXPECT_TRUE(has({"--ro-bind", "/owned/root", "/"}));
    EXPECT_TRUE(has({"--ro-bind", "/owned/home", "/owned/home"}));
    EXPECT_TRUE(has({"--bind", "/owned/staging", "/run/xlings-ldcache"}));
    EXPECT_FALSE(has({"--bind", "/owned/root", "/"}));
    EXPECT_TRUE(has({"--", "/usr/bin/ldconfig", "-X", "-i", "-C", "/run/xlings-ldcache/ld.so.cache"}));
    EXPECT_TRUE(has({"-f", "/etc/ld.so.conf", "/usr/lib", "/usr/lib64"}));
    EXPECT_TRUE(has({"--unshare-all"}));
}

XTEST(RootLibraryCache, MissingGeneratorAndUnknownExistingCacheNeverOverwriteUserFiles,
      .area = "subos", .covers = {"ROOT-LDCACHE"}) {
    auto home = tk::Home::isolated("library-cache");
    const auto root = home.root() / "root";
    tk::write_file(root / "etc/ld.so.cache", "user-owned bytes");
    auto missing = lc::refresh(root, {home.root()}, "probe");
    ASSERT_TRUE(missing.has_value()) << missing.error();
    EXPECT_FALSE(*missing);
    tk::write_file(root / "usr/bin/ldconfig", "generator placeholder");
    auto malformed = lc::refresh(root, {home.root()}, "probe");
    ASSERT_FALSE(malformed.has_value());
    EXPECT_NE(malformed.error().find("unknown format"), std::string::npos);
    EXPECT_EQ(tk::read_file(root / "etc/ld.so.cache"), "user-owned bytes");
    EXPECT_FALSE(std::filesystem::exists(home.root() / "run/subos/probe"));
}

XTEST(RootLibraryCache, ARecognisableCacheStillNeedsProofThatXlingsOwnsIt,
      .area = "subos", .covers = {"ROOT-LDCACHE"}) {
    auto home = tk::Home::isolated("unowned-library-cache");
    const auto root = home.root() / "root";
    const auto bytes = std::string("glibc-ld.so.cache1.1") + std::string(64, '\0');
    tk::write_file(root / "etc/ld.so.cache", bytes);
    tk::write_file(root / "usr/bin/ldconfig", "generator placeholder");
    const auto result = lc::refresh(root, {home.root()}, "probe");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("ownership record"), std::string::npos);
    EXPECT_EQ(tk::read_file(root / "etc/ld.so.cache"), bytes);
    EXPECT_FALSE(std::filesystem::exists(root / "etc/xlings/ldcache.json"));
}
