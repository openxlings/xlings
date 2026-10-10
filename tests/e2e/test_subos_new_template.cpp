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
import xlings.platform.target;

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

tk::Home fixture_home(std::string_view name) {
    auto home = tk::Home::isolated(name);
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
    EXPECT_EQ(initialized.exit_code, 0) << initialized.transcript();
    return home;
}

}  // namespace

XTEST(SubosNewTemplate, AtATerminalTheChainAndItsPackagesAreOnePlanAndNothingHangs,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"UX-NO-HANG", "UX-ONE-PLAN"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a rootfs SubOS is a Linux root";
    auto home = fixture_home("subos-new-template");
    // Whatever this machine's own bwrap can do, the home's says yes: the
    // host check is the other test's subject, not this one's.
    const auto bwrap = home.dir() / "data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap";
    tk::write_file(bwrap, "#!/bin/sh\nexit 0\n");
    fs::permissions(bwrap, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    // A terminal, as a person has one: the nested installs must neither be
    // stopped for touching it nor print a plan of their own.
    tk::RunOptions run;
    run.argv = {"subos", "new", "box", "--from", "fixture:t-top@1.0.0"};
    run.pty = true;
    run.env = {{"XLINGS_TOOLS_SEARCH", "home"}};
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

XTEST(SubosNewTemplate, AHostThatCannotMakeARootIsToldBeforeAnythingIsFetched,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"UX-PREFLIGHT"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a rootfs SubOS is a Linux root";
    if (tk::current_user() == "root") GTEST_SKIP() << "root is not who the user-namespace restriction is for";
    auto home = fixture_home("subos-new-preflight");
    // The only bwrap this home can see fails the way Ubuntu's AppArmor
    // restriction makes it fail.
    const auto bwrap = home.dir() / "data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap";
    tk::write_file(bwrap, "#!/bin/sh\necho 'bwrap: setting up uid map: Permission denied' >&2\nexit 1\n");
    fs::permissions(bwrap, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    // Nobody at a terminal (an agent): the one-time setup cannot be asked.
    auto created = home.xlings({"subos", "new", "box", "--from", "fixture:t-top@1.0.0"},
                               {{"XLINGS_TOOLS_SEARCH", "home"}}, std::chrono::seconds(90));
    ASSERT_FALSE(created.timed_out) << created.transcript();
    std::ifstream sysctl("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
    std::string restricted;
    std::getline(sysctl, restricted);
    if (restricted == "1") {
        EXPECT_FALSE(fs::exists(home.dir() / "data/xpkgs/fixture-x-hello"))
            << "the declared packages were fetched for a root this host cannot run\n" << created.transcript();
        EXPECT_EQ(created.exit_code, 2) << "a question nobody can answer: exit 2\n" << created.transcript();
        EXPECT_NE(created.transcript().find("one-time setup"), std::string::npos) << created.transcript();
        EXPECT_NE(created.transcript().find("-y"), std::string::npos) << "the answer, spelled\n" << created.transcript();
    } else {
        // Nothing the setup lifts: said before the download, and the root is
        // still made (an export does not need to enter it).
        EXPECT_NE(created.transcript().find("cannot enter a root yet"), std::string::npos)
            << "the cause, first\n" << created.transcript();
    }
}

XTEST(SubosNewTemplate, AnEditionDeclaresItsAbiAndOneThisMachineCannotRunIsRefusedFirst,
      .area = "luban", .cost = tk::Cost::Medium, .covers = {"LUBAN-ABI", "LUBAN-EDITION-MIN-CLIENT"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a rootfs SubOS is a Linux root";
    auto home = fixture_home("subos-new-abi");
    const auto bwrap = home.dir() / "data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap";
    tk::write_file(bwrap, "#!/bin/sh\nexit 0\n");
    fs::permissions(bwrap, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    const auto repo = home.root() / "index";
    const std::string host = xlings::platform::host().arch;
    const std::string other = host == "x86_64" ? "aarch64" : "x86_64";
    auto edition = [&](std::string name, nlohmann::json abi) {
        tk::write_file(repo / "pkgs" / name.substr(0, 1) / (name + ".lua"), template_recipe(name,
            Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0"}}, {"abi", abi},
                 {"boot", {{"init", "/sbin/init"}, {"kernel", "xim:linux-kernel-virt"}, {"kernel_min", "5.10"}}},
                 {"workspace", Json::object()}}));
    };
    edition("abi-other", other + "-linux-gnu");
    edition("abi-musl", Json{{"arch", host}, {"kernel", "linux"}, {"libc", "musl"}});
    edition("abi-here", host + "-linux-gnu");
    tk::write_file(repo / "pkgs/n/newer.lua", template_recipe("newer",
        Json{{"subos_kind", "rootfs"}, {"min_client", "9999.1.1.1"}, {"packages", {"fixture:hello@1.0.0"}},
             {"abi", host + "-linux-gnu"}, {"workspace", Json::object()}}));
    const std::map<std::string, std::string> env{{"XLINGS_TOOLS_SEARCH", "home"}};
    // An edition that uses what a newer client reads says so: refused first.
    auto newer = home.xlings({"subos", "new", "n", "--from", "fixture:newer@1.0.0"}, env);
    EXPECT_NE(newer.exit_code, 0) << newer.transcript();
    EXPECT_NE(newer.transcript().find("needs xlings >= 9999.1.1.1"), std::string::npos) << newer.transcript();
    EXPECT_NE(newer.transcript().find("xlings self update"), std::string::npos) << newer.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/n")) << "half-made";
    auto refused = home.xlings({"subos", "new", "a", "--from", "fixture:abi-other@1.0.0"}, env);
    EXPECT_NE(refused.exit_code, 0) << refused.transcript();
    EXPECT_NE(refused.transcript().find("built for " + other), std::string::npos) << refused.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "data/xpkgs/fixture-x-hello")) << "fetched for a root it then refused";
    EXPECT_FALSE(fs::exists(home.dir() / "subos/a")) << "half-made";
    auto musl = home.xlings({"subos", "new", "b", "--from", "fixture:abi-musl@1.0.0"}, env);
    EXPECT_NE(musl.exit_code, 0);
    EXPECT_NE(musl.transcript().find("libc=musl"), std::string::npos) << musl.transcript();
    auto made = home.xlings({"subos", "new", "c", "--from", "fixture:abi-here@1.0.0"}, env);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto instance = Json::parse(tk::read_file(home.dir() / "config/subos/c/instance.json"));
    EXPECT_EQ(instance["root_abi"]["arch"], host);
    EXPECT_EQ(instance["root_abi"]["libc"], "gnu");
    EXPECT_EQ(instance["boot"]["kernel"], "xim:linux-kernel-virt") << "a hint for an image, recorded";
    EXPECT_FALSE(fs::exists(home.dir() / "data/xpkgs/xim-x-linux-kernel-virt")) << "never installed into the root";
}
