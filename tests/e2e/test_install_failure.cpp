#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {
nlohmann::json fixture_indexes(const fs::path& fixture) {
    const auto primary = fixture.parent_path() / "empty-primary-index";
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    return {{"mirror", "GLOBAL"},
            {"xim", {{"index-repo", primary.generic_string()}}},
            {"index_repos",
             nlohmann::json::array(
                 {{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                   {"url", primary.generic_string()},
                   {"source", "git"}},
                  {{"name", "fixture"}, {"url", fixture.generic_string()}, {"source", "git"}}})}};
}

// A base package that fails (or not) and a package on top of it whose install
// hook leaves a marker. Both promise a program, so "registered none of its
// programs" is what the old installer reported for each of them.
void write_pair(const fs::path& repo, std::string_view base, bool baseFails) {
    tk::write_file(repo / "pkgs" / base.substr(0, 1) / std::format("{}.lua", base),
                   std::format(R"LUA(
        package = {{
            spec = '1', name = '{0}', type = 'package', archs = {{'x86_64', 'aarch64'}},
            programs = {{'{0}-prog'}},
            xpm = {{linux = {{['1.0.0'] = {{}}}}, macosx = {{['1.0.0'] = {{}}}},
                   windows = {{['1.0.0'] = {{}}}}}}
        }}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            {1}
            os.mkdir(pkginfo.install_dir())
            return true
        end
        function config()
            xvm.add('{0}-prog', {{bindir = pkginfo.install_dir()}})
            return true
        end
    )LUA", base, baseFails ? "error('simulated base failure')" : ""));
    const auto top = std::format("on-{}", base);
    tk::write_file(repo / "pkgs" / "o" / std::format("{}.lua", top), std::format(R"LUA(
        package = {{
            spec = '1', name = '{0}', type = 'package', archs = {{'x86_64', 'aarch64'}},
            programs = {{'{0}-prog'}},
            xpm = {{linux = {{deps = {{'fixture:{1}'}}, ['1.0.0'] = {{}}}},
                   macosx = {{deps = {{'fixture:{1}'}}, ['1.0.0'] = {{}}}},
                   windows = {{deps = {{'fixture:{1}'}}, ['1.0.0'] = {{}}}}}}
        }}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            os.mkdir(pkginfo.install_dir())
            io.writefile(path.join(pkginfo.install_dir(), 'ran'), 'x')
            return true
        end
        function config()
            xvm.add('{0}-prog', {{bindir = pkginfo.install_dir()}})
            return true
        end
    )LUA", top, base));
}
} // namespace

// One failure is reported once. The packages that depend on it are reported
// as not installed BECAUSE of it, and none of their hooks run: a dependent's
// hook running against a payload that is not there produced errors of its
// own, and the program check then reported every package of the closure as
// "installed but registered none of its programs" -- the original cause was
// the first of four error lines, and the last one, the one a user reads,
// blamed the wrong package.
XTEST(InstallFailure, ADependentOfAFailedPackageIsNotRunAndNotBlamed, .area = "xim",
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("install-failure-cascade");
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    write_pair(repo, "basebroken", true);
    write_pair(repo, "basegood", false);
    tk::write_file(home.dir() / ".xlings.json", fixture_indexes(repo).dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(),
                  home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    // The control: the same pair with a base that installs. The dependent's
    // hook runs (its marker exists), so the fixture itself is sound.
    const auto good = home.xlings({"install", "fixture:on-basegood@1.0.0", "-y"});
    ASSERT_EQ(good.exit_code, 0) << good.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "data" / "xpkgs" / "fixture-x-on-basegood" / "1.0.0" / "ran"))
        << good.transcript();

    const auto bad = home.xlings({"install", "fixture:on-basebroken@1.0.0", "-y"});
    const auto out = bad.transcript();
    EXPECT_NE(bad.exit_code, 0) << out;
    EXPECT_NE(out.find("simulated base failure"), std::string::npos) << out;
    EXPECT_NE(out.find("not installed: its dependency fixture:basebroken@1.0.0 failed"),
              std::string::npos) << out;
    EXPECT_EQ(out.find("registered none"), std::string::npos) << out;
    EXPECT_FALSE(fs::exists(home.dir() / "data" / "xpkgs" / "fixture-x-on-basebroken" / "1.0.0" / "ran"))
        << out;
}
