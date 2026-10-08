#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.library_cache;
import xlings.subos.caps;
import xlings.platform.root_mount;
import xlings.core.elfread;

namespace tk = xlings::testkit;
namespace lc = xlings::subos::library_cache;
namespace fs = std::filesystem;

XTEST(RootLibraryCache, GeneratorSeesTheRootReadOnlyAndWritesOnlyOwnedStaging, .area = "subos",
      .covers = {"ROOT-LDCACHE"}) {
    const std::vector<xlings::platform::root_mount::Binding> bindings{
        {"/private/skeleton", "/owned/home"},
        {"/owned/home/data/xpkgs/xim-x-glibc/2.42", "/owned/home/data/xpkgs/xim-x-glibc/2.42"}};
    const auto argv =
        lc::command("/owned/root", {"/owned/home"}, "/owned/staging", "/measured/bwrap", bindings);
    const auto has = [&](std::initializer_list<std::string> words) {
        return std::ranges::search(argv, words).begin() != argv.end();
    };
    EXPECT_TRUE(has({"--ro-bind", "/owned/root", "/"}));
    EXPECT_TRUE(has({"--ro-bind", "/private/skeleton", "/owned/home"}));
    EXPECT_FALSE(has({"--ro-bind", "/owned/home", "/owned/home"}));
    EXPECT_TRUE(has({"--ro-bind", bindings[1].source.string(), bindings[1].destination.string()}));
    EXPECT_TRUE(has({"--bind", "/owned/staging", "/run/xlings-ldcache"}));
    EXPECT_FALSE(has({"--bind", "/owned/root", "/"}));
    EXPECT_TRUE(
        has({"--", "/usr/bin/ldconfig", "-X", "-i", "-C", "/run/xlings-ldcache/ld.so.cache"}));
    EXPECT_TRUE(has({"-f", "/etc/ld.so.conf", "/usr/lib", "/usr/lib64"}));
    EXPECT_TRUE(has({"--unshare-all"}));
    const auto position = [&](std::initializer_list<std::string> words) {
        return std::ranges::search(argv, words).begin();
    };
    ASSERT_TRUE(has({"--tmpfs", "/run"}));
    ASSERT_TRUE(has({"--remount-ro", "/run"}));
    EXPECT_LT(position({"--ro-bind", "/owned/root", "/"}), position({"--tmpfs", "/run"}));
    EXPECT_LT(position({"--tmpfs", "/run"}), position({"--ro-bind", "/private/skeleton", "/owned/home"}));
    EXPECT_LT(position({"--ro-bind", bindings[1].source.string(), bindings[1].destination.string()}),
              position({"--bind", "/owned/staging", "/run/xlings-ldcache"}));
    EXPECT_LT(position({"--bind", "/owned/staging", "/run/xlings-ldcache"}),
              position({"--remount-ro", "/run"}));
}

XTEST(RootLibraryCache, RunningRootKeepsItsDirectGeneratorCommand, .area = "subos",
      .covers = {"ROOT-LDCACHE"}) {
    const std::vector<std::string> expected{"/usr/bin/ldconfig", "-X", "-i", "-C",
        (fs::path("/owned/staging") / "ld.so.cache").string(), "-f", "/etc/ld.so.conf",
        "/usr/lib", "/usr/lib64"};
    EXPECT_EQ(lc::command("/", {"/owned/home"}, "/owned/staging", "/unused/bwrap"), expected);
}

XTEST(RootLibraryCache, ActualGeneratorUsesPrivateRunAndPreservesMachineState,
      .area = "subos", .covers = {"ROOT-LDCACHE"}, .requires_ = {"linux", "bwrap"},
      .resources = {"sandbox"}, .proves = "isolation") {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux mount namespaces";
    if (const auto why = tk::probe("bwrap")) GTEST_SKIP() << *why;
    fs::path generator;
    for (const auto* path : {"/usr/sbin/ldconfig.real", "/sbin/ldconfig.real",
                             "/usr/sbin/ldconfig", "/sbin/ldconfig"}) {
        const auto elf = xlings::elfread::read(path);
        if (elf && elf->interpreter.empty()) { generator = fs::canonical(path); break; }
    }
    ASSERT_FALSE(generator.empty()) << "this Linux namespace lane needs its static glibc ldconfig";
    auto home = tk::Home::isolated("library-cache-namespace");
    const auto root = home.root() / "machine";
    const auto authority = home.root() / "runtime-authority";
    const auto listing = tk::run({.argv = {generator.string(), "-p"}, .env = home.env()});
    ASSERT_EQ(listing.exit_code, 0) << listing.transcript();
    fs::path libc;
    std::istringstream lines(listing.out);
    std::string line;
    while (std::getline(lines, line)) {
        const auto name = line.find_first_not_of(" \t");
        const auto separator = line.find(" => ");
        if (name != std::string::npos && line.substr(name).starts_with("libc.so.6 ") &&
            separator != std::string::npos) {
            libc = fs::canonical(line.substr(separator + 4));
            break;
        }
    }
    ASSERT_FALSE(libc.empty()) << listing.transcript();
    fs::create_directories(root / "usr/bin");
    fs::create_directories(root / "usr/lib");
    fs::create_directories(root / "usr/lib64");
    fs::create_directories(root / "proc");
    fs::create_directories(root / "dev");
    fs::create_directory(authority);
    fs::copy_file(generator, root / "usr/bin/ldconfig");
    fs::copy_file(libc, authority / "libc.so.6");
    tk::write_file(root / "etc/ld.so.conf", "/run/xlings-cache-authority\n");
    tk::write_file(root / "run/user-owned", "retain machine runtime data");
    const auto sourceBefore = tk::read_file(authority / "libc.so.6");
    const std::vector<xlings::platform::root_mount::Binding> bindings{
        {authority, "/run/xlings-cache-authority"}};
    const auto generated = lc::refresh(root, {home.dir()}, "probe", bindings);
    ASSERT_TRUE(generated) << generated.error();
    ASSERT_TRUE(*generated);
    const auto cache = tk::read_file(root / "etc/ld.so.cache");
    EXPECT_NE(cache.find("/run/xlings-cache-authority/libc.so.6"), std::string::npos);
    EXPECT_EQ(tk::read_file(root / "run/user-owned"), "retain machine runtime data");
    EXPECT_FALSE(fs::exists(root / "run/xlings-ldcache"));
    EXPECT_FALSE(fs::exists(root / "run/xlings-cache-authority"));
    EXPECT_EQ(tk::read_file(authority / "libc.so.6"), sourceBefore);
    const auto backend = xlings::subos::caps::probe({home.dir()}, {});
    ASSERT_TRUE(backend.bwrap && backend.bwrap->usable);
    const auto scratch = home.root() / "forbidden-output-staging";
    ASSERT_TRUE(fs::create_directory(scratch));
    auto forbidden = lc::command(root, {home.dir()}, scratch, backend.bwrap->bin, bindings);
    const auto cacheFlag = std::ranges::find(forbidden, "-C");
    ASSERT_NE(cacheFlag, forbidden.end());
    *(cacheFlag + 1) = "/run/unowned-cache";
    const auto denied = tk::run({.argv = std::move(forbidden), .env = home.env()});
    EXPECT_NE(denied.exit_code, 0) << denied.transcript();
    EXPECT_FALSE(denied.timed_out) << denied.transcript();
    EXPECT_EQ(denied.signal, 0) << denied.transcript();
    EXPECT_FALSE(fs::exists(root / "run/unowned-cache"));
    EXPECT_FALSE(fs::exists(scratch / "ld.so.cache"));
    tk::write_file(root / "etc/ld.so.cache", "user replacement");
    const auto changed = lc::refresh(root, {home.dir()}, "probe", bindings);
    ASSERT_FALSE(changed);
    EXPECT_EQ(tk::read_file(root / "etc/ld.so.cache"), "user replacement");
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

XTEST(RootLibraryCache, ARecognisableCacheStillNeedsProofThatXlingsOwnsIt, .area = "subos",
      .covers = {"ROOT-LDCACHE"}) {
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

XTEST(RootLibraryCache, NonRootGeneratorRefusesAnUnprovedHomeBinding, .area = "subos",
      .covers = {"ROOT-LDCACHE"}) {
    EXPECT_THROW(lc::command("/owned/root", {"/owned/home"}, "/owned/staging", "/measured/bwrap"),
                 std::runtime_error);
}
