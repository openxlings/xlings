// The conformance cases of the xpkg format (libxpkg tests/conformance/,
// design 2026-11-27 §9.9) through xlings' whole install: each case that names
// an install is an index of its own, its package is installed, and the case's
// outcome and payload hold -- the same expectations libxpkg checks through
// its executor, decided by the same code (mcpplibs.xpkg.conformance).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;
import mcpplibs.xpkg.conformance;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace cf = mcpplibs::xpkg::conformance;

namespace {

std::string repo_name(std::size_t i) { return std::format("case{}", i); }

}  // namespace

XTEST(XpkgConformance, EveryCaseInstalledThroughXlingsEndsAsItSays, .area = "xim",
      .covers = {"XPKG-CONFORMANCE"}, .requires_ = {"xlings-bin"}) {
    std::vector<const cf::Case*> cases;
    for (const auto& c : cf::cases())
        if (!c.install.empty()) cases.push_back(&c);
    ASSERT_GE(cases.size(), 8u);

    auto home = tk::Home::isolated("xpkg-conformance");
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    // What an install on a Linux host without patchelf takes first.
    tk::write_file(primary / "pkgs" / "p" / "patchelf.lua", R"LUA(
package = {
    spec = '1', name = 'patchelf', type = 'package', archs = {'x86_64', 'aarch64'},
    xpm = { linux = { ['0.18.0'] = {} }, macosx = { ['0.18.0'] = {} }, windows = { ['0.18.0'] = {} } },
}
import('xim.libxpkg.pkginfo')
function install()
    os.mkdir(pkginfo.install_dir())
    io.writefile(path.join(pkginfo.install_dir(), 'marker'), 'x')
    return true
end
function config() return true end
)LUA");
    auto repos = nlohmann::json::array();
    repos.push_back({{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                     {"url", primary.generic_string()}, {"source", "git"}});
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto index = home.root() / "cases" / repo_name(i);
        cf::materialize(*cases[i], index);
        tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
        repos.push_back({{"name", repo_name(i)}, {"url", index.generic_string()}, {"source", "git"}});
    }
    const nlohmann::json config = {{"mirror", "GLOBAL"},
                                   {"xim", {{"index-repo", primary.generic_string()}}},
                                   {"index_repos", repos}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"),
                  fs::copy_options::overwrite_existing);
    const auto init = home.xlings({"self", "init"});
    ASSERT_EQ(init.exit_code, 0) << init.transcript();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = *cases[i];
        SCOPED_TRACE(std::format("case {} ({}): {}", c.name, c.chapter, c.summary));
        const auto target = std::format("{}:{}@1.0.0", repo_name(i), c.package);
        const auto r = home.xlings({"install", target, "-y"});
        const auto out = r.transcript();
        const auto payload = home.dir() / "data" / "xpkgs" / std::format("{}-x-{}", repo_name(i), c.package) / "1.0.0";
        if (c.install == "installed") {
            EXPECT_EQ(r.exit_code, 0) << out;
            for (const auto& problem : cf::unmet(c.payload, payload)) ADD_FAILURE() << problem << "\n" << out;
        } else {
            EXPECT_NE(r.exit_code, 0) << out;
            if (c.install == "refused") EXPECT_NE(out.find("spec"), std::string::npos) << out;
            const auto stamp = payload / ".xpkg-install.json";
            std::error_code ec;
            if (fs::exists(stamp, ec))
                EXPECT_NE(tk::read_file(stamp).find("\"incomplete\": true"), std::string::npos) << out;
        }
    }
}
