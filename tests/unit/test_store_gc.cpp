#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.store;
import xlings.core.profile;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace st = xlings::store;
using Json = nlohmann::json;

// The store's collection keeps what the home references (xlings#712): the
// registrations, the links of the views, the closure of the install records'
// dependencies and the roots other programs hold. Before, it read the
// registrations alone and listed for removal a payload a SubOS's
// `usr/include/linux` linked into, and every payload mcpp had wired into a
// build.

namespace {

struct Store {
    tk::Home home;
    fs::path xpkgs;
    explicit Store(std::string_view name) : home(tk::Home::isolated(name)), xpkgs(home.dir() / "data/xpkgs") {
        tk::write_file(home.dir() / ".xlings.json", R"({"versions":{}})");
        tk::write_file(home.dir() / "subos/default/.xlings.json", R"({"workspace":{}})");
    }
    fs::path payload(std::string_view store, std::string_view version) const {
        const auto dir = xpkgs / store / version;
        tk::write_file(dir / "share/sentinel", std::string(store));
        return dir;
    }
    void resolution(const fs::path& payload, const fs::path& dependency) const {
        tk::write_file(payload / ".xlings-resolution.json", Json{
            {"package", "fixture:x@1"}, {"deps", Json::array({{
                {"spec", "dep"}, {"name", "fixture:dep"}, {"version", "1"},
                {"install_dir", dependency.string()}, {"source", "plan"}, {"libdirs", Json::array()}}})}}.dump());
    }
    fs::path root(std::string_view owner, std::string_view store, std::string_view version) const {
        const auto dir = home.dir() / "data/roots" / owner;
        fs::create_directories(dir);
        const auto at = dir / std::format("{}@{}", store, version);
        // Relative, the spelling a moved home keeps (docs/specs/store-roots.md).
        fs::create_directory_symlink(fs::path("../../xpkgs") / store / version, at);
        return at;
    }
};

}  // namespace

XTEST(StoreGc, AViewLinkADependencyClosureAndARootEachKeepTheirPayload,
      .area = "store", .covers = {"STORE-GC-REFERENCES"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "the fixture's links are symbolic links";
    Store s("store-gc-references");
    const auto viewed = s.payload("scode-x-linux-headers", "5.11.1");
    const auto rooted = s.payload("xim-x-gcc", "16.1.0");
    const auto depended = s.payload("xim-x-binutils", "2.42.1");
    const auto deeper = s.payload("xim-x-glibc", "2.44.3");
    const auto garbage = s.payload("xim-x-unused", "1.0.0");

    // A view: the SubOS's header farm links into the payload no registration names.
    fs::create_directories(s.home.dir() / "subos/default/usr/include");
    fs::create_directory_symlink(viewed / "share", s.home.dir() / "subos/default/usr/include/linux");
    // A root another program holds, and what its install record depends on, to the closure.
    const auto root = s.root("mcpp", "xim-x-gcc", "16.1.0");
    s.resolution(rooted, depended);
    s.resolution(depended, deeper);
    // A root whose payload is gone.
    const auto dangling = s.root("mcpp", "xim-x-gone", "9.9.9");

    EXPECT_EQ(xlings::profile::gc(s.home.dir(), /*dryRun=*/true), 0);
    EXPECT_TRUE(fs::is_symlink(dangling)) << "a dry run changes nothing";
    EXPECT_TRUE(fs::exists(garbage));

    EXPECT_EQ(xlings::profile::gc(s.home.dir(), /*dryRun=*/false), 0);
    for (const auto& kept : {viewed, rooted, depended, deeper})
        EXPECT_TRUE(fs::exists(kept / "share/sentinel")) << kept;
    EXPECT_FALSE(fs::exists(garbage)) << "nothing references it";
    EXPECT_TRUE(fs::is_symlink(root));
    std::error_code ec;
    EXPECT_FALSE(fs::is_symlink(fs::symlink_status(dangling, ec))) << "a dangling root is removed";
}

XTEST(StoreGc, RootsThatCannotBeReadRefuseTheCollection,
      .area = "store", .covers = {"STORE-GC-REFERENCES"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "permissions are POSIX's";
    if (xlings::platform::is_root()) GTEST_SKIP() << "root reads a directory whatever its mode";
    Store s("store-gc-unreadable-roots");
    const auto rooted = s.payload("xim-x-gcc", "16.1.0");
    (void)s.root("mcpp", "xim-x-gcc", "16.1.0");
    const auto owner = s.home.dir() / "data/roots/mcpp";
    fs::permissions(owner, fs::perms::none, fs::perm_options::replace);
    const int rc = xlings::profile::gc(s.home.dir(), /*dryRun=*/false);
    fs::permissions(owner, fs::perms::owner_all, fs::perm_options::replace);
    EXPECT_EQ(rc, 1) << "could not read is not references nothing";
    EXPECT_TRUE(fs::exists(rooted / "share/sentinel"));
}

XTEST(StoreGc, AnUnreadableInstallRecordRefusesTheCollection,
      .area = "store", .covers = {"STORE-GC-REFERENCES"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "the fixture's links are symbolic links";
    Store s("store-gc-unreadable-record");
    const auto rooted = s.payload("xim-x-gcc", "16.1.0");
    const auto unused = s.payload("xim-x-unused", "1.0.0");
    (void)s.root("mcpp", "xim-x-gcc", "16.1.0");
    tk::write_file(rooted / ".xlings-resolution.json", "{not json");
    EXPECT_EQ(xlings::profile::gc(s.home.dir(), /*dryRun=*/false), 1);
    EXPECT_TRUE(fs::exists(unused / "share/sentinel")) << "a refusal removes nothing";
}

XTEST(StoreGc, ARootNamesItsPayloadByItsTarget,
      .area = "store", .covers = {"STORE-GC-REFERENCES"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "the fixture's links are symbolic links";
    Store s("store-gc-root-scan");
    const auto payload = s.payload("xim-x-gcc", "16.1.0");
    (void)s.root("mcpp", "xim-x-gcc", "16.1.0");
    // Not a root: a file, and a link to something inside a payload.
    tk::write_file(s.home.dir() / "data/roots/mcpp/notes.txt", "x");
    fs::create_symlink(payload / "share/sentinel", s.home.dir() / "data/roots/mcpp/inside@1");
    auto scan = st::read_roots(s.home.dir() / "data");
    ASSERT_TRUE(scan) << scan.error();
    ASSERT_EQ(scan->live.size(), 1u);
    EXPECT_EQ(scan->live.front().owner, "mcpp");
    EXPECT_EQ(st::payload_key(scan->live.front().payload), "xim-x-gcc/16.1.0");
    EXPECT_EQ(scan->ignored.size(), 2u);
    EXPECT_TRUE(scan->dangling.empty());
    auto none = st::read_roots(s.home.dir() / "absent");
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->live.empty());
}
