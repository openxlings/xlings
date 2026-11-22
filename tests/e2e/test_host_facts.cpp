#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

// xlings#727: a configuration generated from a host fact -- links named for
// the driver version the host had loaded at install time -- stayed "installed"
// after the host changed, and nothing checked it where it was used. A recipe
// now states the host files its config() reads (`host_facts`); the install
// records their digests beside the scope's `configured` record, and
// activation, entering the SubOS and `self doctor --fix` configure it again
// when they read differently.

namespace {

struct Fixture {
    tk::Home home = tk::Home::isolated("host-facts");
    fs::path fact = home.root() / "host/driver-version";
    fs::path payload = home.dir() / "data/xpkgs/fixture-x-hostlink/1.0.0";

    Fixture() {
        tk::write_file(fact, "550.144.03\n");
        const auto repo = home.root() / "index";
        tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
        const auto factPath = fact.generic_string();
        tk::write_file(repo / "pkgs/h/hostlink.lua", std::format(R"LUA(
            package = {{
                spec = '1', name = 'hostlink', type = 'package', archs = {{'x86_64', 'aarch64'}},
                programs = {{'hostlink'}},
                host_facts = {{ linux = {{'{0}'}}, macosx = {{'{0}'}}, windows = {{'{0}'}} }},
                xpm = {{linux = {{['1.0.0'] = {{}}}}, macosx = {{['1.0.0'] = {{}}}}, windows = {{['1.0.0'] = {{}}}}}}
            }}
            import('xim.libxpkg.pkginfo')
            import('xim.libxpkg.xvm')
            function install()
                local dir = pkginfo.install_dir()
                os.mkdir(path.join(dir, 'bin'))
                io.writefile(path.join(dir, 'bin', 'hostlink'), '#!/bin/sh\necho hostlink\n')
                return true
            end
            -- What it generates is named for the fact, as the host-driver
            -- link packages name their links for the loaded driver.
            function config()
                local dir = pkginfo.install_dir()
                io.writefile(path.join(dir, 'configured-with'), io.readfile('{0}') or 'absent')
                xvm.add('hostlink', {{bindir = path.join(dir, 'bin')}})
                return true
            end
        )LUA", factPath));
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
    }
    std::string configured_with() const { return tk::read_file(payload / "configured-with"); }
};

}  // namespace

XTEST(HostFacts, TheRecordIsKeptAndAChangedFactIsConfiguredAgainWhereItIsUsed,
      .area = "xim", .cost = tk::Cost::Medium, .covers = {"XIM-HOST-FACTS"}, .requires_ = {"xlings-bin"}) {
    Fixture f;
    auto installed = f.home.xlings({"install", "fixture:hostlink@1.0.0", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    EXPECT_EQ(f.configured_with(), "550.144.03\n");

    // The scope records each declared file's digest beside `configured`.
    const auto state = nlohmann::json::parse(tk::read_file(f.home.dir() / "subos/default/.xlings.json"));
    ASSERT_TRUE(state.contains("host_facts")) << state.dump(2);
    const auto record = state["host_facts"].value("fixture:hostlink@1.0.0", nlohmann::json::object());
    ASSERT_TRUE(record.contains(f.fact.generic_string())) << state.dump(2);
    EXPECT_TRUE(record[f.fact.generic_string()].get<std::string>().starts_with("sha256:"));

    // Nothing changed: an install has nothing to do.
    auto again = f.home.xlings({"install", "fixture:hostlink@1.0.0", "-y"});
    ASSERT_EQ(again.exit_code, 0) << again.transcript();
    EXPECT_EQ(f.configured_with(), "550.144.03\n");

    // Activation: the host's driver was upgraded.
    tk::write_file(f.fact, "580.178.04\n");
    auto used = f.home.xlings({"use", "hostlink", "1.0.0"});
    ASSERT_EQ(used.exit_code, 0) << used.transcript();
    EXPECT_EQ(f.configured_with(), "580.178.04\n") << used.transcript();

    // The doctor: reported, configured again by --fix, then clean.
    tk::write_file(f.fact, "590.48.01\n");
    auto found = f.home.xlings({"self", "doctor"});
    EXPECT_NE(found.exit_code, 0) << found.transcript();
    EXPECT_NE(found.transcript().find("host facts changed"), std::string::npos) << found.transcript();
    auto fixed = f.home.xlings({"self", "doctor", "--fix", "-y"});
    EXPECT_EQ(fixed.exit_code, 0) << fixed.transcript();
    EXPECT_EQ(f.configured_with(), "590.48.01\n") << fixed.transcript();
    auto clean = f.home.xlings({"self", "doctor"});
    EXPECT_EQ(clean.exit_code, 0) << clean.transcript();

    // An install is a use site too: the same predicate decides it.
    fs::remove(f.fact);
    auto reinstalled = f.home.xlings({"install", "fixture:hostlink@1.0.0", "-y"});
    ASSERT_EQ(reinstalled.exit_code, 0) << reinstalled.transcript();
    EXPECT_EQ(f.configured_with(), "absent") << reinstalled.transcript();
}
