#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

// xlings#711: a payload carrying the sweep fingerprint that the version
// database does not know -- the shape homes migrated from 0.4.x are left in.
// `self doctor --fix` reported it repaired and the disk did not change: the
// removal ran the recipe's uninstall hook, whose `xvm.remove` reclaims nothing
// without a record, and the reinstall then found the directory non-empty and
// called it installed. Every run repeated the cycle and exited 1. The existing
// test (doctor_swept_payload_test.sh) seeds the fingerprint into a REGISTERED
// install, which the xvm path does empty; this one builds the unregistered
// payload directly.

namespace {

void write_recipe(const fs::path& repo) {
    tk::write_file(repo / "pkgs/s/swept.lua", R"LUA(
        package = {
            spec = '1', name = 'swept', type = 'package', archs = {'x86_64', 'aarch64'},
            programs = {'swept'},
            xpm = {linux = {['1.0.0'] = {}}, macosx = {['1.0.0'] = {}}, windows = {['1.0.0'] = {}}}
        }
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            local dir = pkginfo.install_dir()
            os.mkdir(path.join(dir, 'bin'))
            io.writefile(path.join(dir, 'bin', 'swept'), '#!/bin/sh\necho swept\n')
            return true
        end
        function config()
            xvm.add('swept', {bindir = path.join(pkginfo.install_dir(), 'bin')})
            return true
        end
        -- The shape of pkgs/z/zlib.lua: the hook reclaims through xvm, which
        -- has no record of this payload.
        function uninstall()
            xvm.remove('swept')
            return true
        end
    )LUA");
}

tk::Home seeded_home() {
    auto home = tk::Home::isolated("doctor-swept-unregistered");
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    write_recipe(repo);
    const auto primary = home.root() / "empty-primary-index";
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(home.dir() / ".xlings.json", nlohmann::json{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}}.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    EXPECT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    return home;
}

}  // namespace

XTEST(DoctorSweptUnregistered, OneFixConvergesAndTheDirectoryIsAFreshInstall,
      .area = "self", .cost = tk::Cost::Medium, .covers = {"DOCTOR-SWEPT-UNREGISTERED", "DOCTOR-LADDER"},
      .requires_ = {"xlings-bin"}) {
    auto home = seeded_home();
    const auto payload = home.dir() / "data/xpkgs/fixture-x-swept/1.0.0";
    // The payload as the sweep left it, never installed through this home: no
    // version record, a foreign archive and its download lock at the top.
    tk::write_file(payload / "bin/swept", "stale\n");
    tk::write_file(payload / "zlib-1.3.2.tar.gz", "foreign archive");
    tk::write_file(payload / "zlib-1.3.2.tar.gz.lock", "");

    const auto found = home.xlings({"self", "doctor"});
    ASSERT_NE(found.exit_code, 0) << found.transcript();
    ASSERT_NE(found.transcript().find("swept payload"), std::string::npos) << found.transcript();

    const auto fixed = home.xlings({"self", "doctor", "--fix", "-y"});
    EXPECT_EQ(fixed.exit_code, 0) << fixed.transcript();
    // The directory is a fresh install's: the sweep's files are gone, the
    // package's own is back.
    EXPECT_FALSE(fs::exists(payload / "zlib-1.3.2.tar.gz")) << fixed.transcript();
    EXPECT_FALSE(fs::exists(payload / "zlib-1.3.2.tar.gz.lock")) << fixed.transcript();
    ASSERT_TRUE(fs::exists(payload / "bin/swept")) << fixed.transcript();
    EXPECT_NE(tk::read_file(payload / "bin/swept").find("echo swept"), std::string::npos);

    const auto again = home.xlings({"self", "doctor"});
    EXPECT_EQ(again.exit_code, 0) << again.transcript();
    EXPECT_EQ(again.transcript().find("swept payload"), std::string::npos) << again.transcript();
}

// The installer, not the recipe, deletes the directory a removal takes out:
// after the hook, whatever the hook did. A recipe whose hook reclaims nothing
// leaves nothing behind.
XTEST(DoctorSweptUnregistered, RemovalDeletesTheDirectoryAfterTheHook,
      .area = "xim", .covers = {"DOCTOR-SWEPT-UNREGISTERED"}, .requires_ = {"xlings-bin"}) {
    auto home = seeded_home();
    const auto payload = home.dir() / "data/xpkgs/fixture-x-swept/1.0.0";
    tk::write_file(payload / "bin/swept", "stale\n");
    tk::write_file(payload / "leftover.txt", "x");
    const auto removed = home.xlings({"remove", "fixture:swept@1.0.0", "--force", "-y"});
    EXPECT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(payload)) << removed.transcript();
}
