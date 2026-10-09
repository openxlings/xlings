// `subos new --from <template>` at a terminal: one command, one plan.
//
// Measured on 2026.10.9.2 with subos:luban-core: the template and its `from`
// were each installed by a child xlings with its own plan, then the declared
// packages by another child -- which, in its own process group, was stopped
// by the kernel the moment it probed the terminal, and the command sat there
// for 30 minutes. The template also said "installed, but 'luban-core' still
// resolves to subos:0.1.0 -- `xlings use` to switch": the same version, under
// its namespaced key.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::size_t count(std::string_view text, std::string_view what) {
    std::size_t n = 0;
    for (auto at = text.find(what); at != std::string_view::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

std::string template_recipe(std::string_view name, const Json& manifest) {
    return std::format(R"LUA(
package = {{ spec = '1', name = '{}', type = 'subos', archs = {{'x86_64', 'aarch64'}},
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }} }} }}
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(dir)
    io.writefile(path.join(dir, '.xlings.json'), {})
    return true
end
)LUA", name, Json(manifest.dump()).dump());
}

}  // namespace

XTEST(SubosNewTemplate, AtATerminalTheChainAndItsPackagesAreOnePlanAndNothingHangs,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"UX-NO-HANG", "UX-ONE-PLAN"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a rootfs SubOS is a Linux root";
    auto home = tk::Home::isolated("subos-new-template");
    const auto primary = home.root() / "primary";
    const auto repo = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/h/hello.lua", R"LUA(
package = { spec = '1', name = 'hello', archs = {'x86_64', 'aarch64'},
            xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, 'bin'))
    io.writefile(path.join(dir, 'bin', 'hello'), '#!/bin/sh\necho hello\n')
    return true
end
)LUA");
    tk::write_file(repo / "pkgs/t/t-base.lua", template_recipe("t-base",
        Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0"}}, {"workspace", Json::object()}}));
    tk::write_file(repo / "pkgs/t/t-top.lua", template_recipe("t-top",
        Json{{"subos_kind", "rootfs"}, {"from", "fixture:t-base@1.0.0"}, {"packages", Json::array()},
             {"workspace", Json::object()}}));
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    auto initialized = home.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();

    // A terminal, as a person has one: the nested installs must neither be
    // stopped for touching it nor print a plan of their own.
    tk::RunOptions run;
    run.argv = {"subos", "new", "box", "--from", "fixture:t-top@1.0.0"};
    run.pty = true;
    run.timeout = std::chrono::seconds(90);
    auto created = home.xlings(run);
    ASSERT_FALSE(created.timed_out) << "subos new did not finish:\n" << created.transcript();
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    EXPECT_EQ(count(created.out, "Packages to install"), 1u)
        << "one plan: the declared packages; the templates are plumbing\n" << created.out;
    EXPECT_EQ(count(created.out, "still resolves to"), 0u) << created.out;
    EXPECT_EQ(count(created.out, "subos created"), 1u) << created.out;
    EXPECT_TRUE(fs::exists(home.dir() / "data/xpkgs/fixture-x-hello/1.0.0/bin/hello"));
}
