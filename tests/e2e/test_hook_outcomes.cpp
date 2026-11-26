// What a recipe of spec 1 or 2 now gets from the client (design 2026-11-27
// §9.6): the compatibility layer is frozen, and the calls of it that did not
// do what they name come back with the hook's result. Each is a finding
// (`hook-call-failed`); an install() that reported success with such a call
// and left install_dir empty is a failed install (E16) -- the payload that was
// to be moved is not there.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

void recipe_(const fs::path& repo, std::string_view name, std::string_view body) {
    tk::write_file(repo / "pkgs" / name.substr(0, 1) / std::format("{}.lua", name), std::format(R"LUA(
package = {{
    spec = '1', name = '{0}', type = 'package', archs = {{ 'x86_64', 'aarch64' }},
    xpm = {{ linux = {{ ['1.0.0'] = {{}} }}, macosx = {{ ['1.0.0'] = {{}} }}, windows = {{ ['1.0.0'] = {{}} }} }},
}}
import('xim.libxpkg.pkginfo')
{1}
)LUA", name, body));
}

tk::Home home_(std::string_view name) {
    auto home = tk::Home::isolated(name);
    const auto primary = home.root() / "primary";
    const auto repo = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    // E16: the move fails (its source is not there), the hook says true.
    recipe_(repo, "emptied", R"LUA(
function install()
    os.tryrm(pkginfo.install_dir())
    os.mv(path.join(pkginfo.install_dir() .. '-extracted', 'nothing'), pkginfo.install_dir())
    return true
end
)LUA");
    // A failed call, and a payload: installed, with a finding.
    recipe_(repo, "partly", R"LUA(
function install()
    os.mkdir(pkginfo.install_dir())
    io.writefile(path.join(pkginfo.install_dir(), 'payload'), 'x')
    os.cp(path.join(pkginfo.install_dir(), 'absent'), path.join(pkginfo.install_dir(), 'copy'))
    return true
end
)LUA");
    // No failed call and nothing in install_dir: a payload that lives
    // elsewhere, installed as before.
    recipe_(repo, "elsewhere", R"LUA(
function install() return true end
)LUA");
    const nlohmann::json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    const auto init = home.xlings({"self", "init"});
    EXPECT_EQ(init.exit_code, 0) << init.transcript();
    return home;
}

}  // namespace

XTEST(HookOutcomes, AnEmptyPayloadAfterAFailedCallIsAFailedInstall, .area = "xim",
      .covers = {"HOOK-EMPTY-PAYLOAD", "HOOK-COMPAT-FROZEN"}, .requires_ = {"xlings-bin"}) {
    auto home = home_("hook-outcomes-empty");
    const auto r = home.xlings({"install", "fixture:emptied@1.0.0", "-y"});
    const auto out = r.transcript();
    EXPECT_NE(r.exit_code, 0) << out;
    EXPECT_NE(out.find("its payload is empty and these calls failed"), std::string::npos) << out;
    EXPECT_NE(out.find("os.mv("), std::string::npos) << out;

    // The control: nothing failed, nothing in install_dir -- installed.
    const auto ok = home.xlings({"install", "fixture:elsewhere@1.0.0", "-y"});
    EXPECT_EQ(ok.exit_code, 0) << ok.transcript();
}

XTEST(HookOutcomes, AFailedCallIsAFindingAndTheInstallStands, .area = "xim",
      .covers = {"HOOK-COMPAT-FROZEN"}, .requires_ = {"xlings-bin"}) {
    auto home = home_("hook-outcomes-finding");
    const auto r = home.xlings({"install", "fixture:partly@1.0.0", "-y"});
    const auto out = r.transcript();
    EXPECT_EQ(r.exit_code, 0) << out;
    EXPECT_NE(out.find("1 finding(s) about the installed payloads"), std::string::npos) << out;
    const auto log = tk::read_file(home.dir() / "logs" / "findings.ndjson");
    EXPECT_NE(log.find("\"rule\":\"hook-call-failed\""), std::string::npos) << log;
    EXPECT_NE(log.find("os.cp"), std::string::npos) << log;
    EXPECT_TRUE(fs::is_regular_file(home.dir() / "data" / "xpkgs" / "fixture-x-partly" / "1.0.0" / "payload"));
}
