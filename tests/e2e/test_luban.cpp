// The luban command against a real xlings (Luban design §B3): where it is,
// the environments, a new one from a template, and what a person and an
// agent are each told.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

// luban beside the xlings under test: in a release `bin/luban`; in a
// development build `bin/luban-tool/luban`.
std::optional<fs::path> luban_binary() {
    if (const char* forced = std::getenv("LUBAN_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& candidate : {xlings.parent_path() / "luban", xlings.parent_path() / "luban-tool" / "luban"})
        if (fs::is_regular_file(candidate)) return candidate;
    return std::nullopt;
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

// A home with a fixture index (one template that declares a root and one
// package), xlings and luban in its bin/, and a bwrap of its own that says yes.
tk::Home luban_home(std::string_view name, const fs::path& luban) {
    auto home = tk::Home::isolated(name);
    const auto primary = home.root() / "primary";
    const auto repo = home.root() / "index";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/h/hello.lua", R"LUA(
package = { spec = '1', name = 'hello', archs = {'x86_64', 'aarch64'}, xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_dir()
    os.mkdir(path.join(dir, 'bin'))
    io.writefile(path.join(dir, 'bin', 'hello'), '#!/bin/sh\necho hello\n')
    return true
end
)LUA");
    tk::write_file(repo / "pkgs/m/my-os.lua", template_recipe("my-os",
        Json{{"subos_kind", "rootfs"}, {"packages", {"fixture:hello@1.0.0"}}, {"workspace", Json::object()}}));
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    fs::copy_file(luban, home.dir() / "bin/luban");
    const auto bwrap = home.dir() / "data/xpkgs/xim-x-bwrap/0.11.2/bin/bwrap";
    tk::write_file(bwrap, "#!/bin/sh\nexit 0\n");
    fs::permissions(bwrap, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    auto initialized = home.xlings({"self", "init"});
    EXPECT_EQ(initialized.exit_code, 0) << initialized.transcript();
    return home;
}

tk::RunResult luban(const tk::Home& home, std::vector<std::string> args, bool pty = false) {
    tk::RunOptions o;
    o.argv = {(home.dir() / "bin/luban").string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["XLINGS_TOOLS_SEARCH"] = "home";
    o.env.erase("XLINGS_SUBOS_MODE");
    o.env.erase("XLINGS_ACTIVE_SUBOS");
    o.pty = pty;
    o.timeout = std::chrono::seconds(90);
    return tk::run(o);
}

}  // namespace

XTEST(Luban, ANewEnvironmentFromATemplateIsListedAndDescribed,
      .area = "luban", .cost = tk::Cost::Medium,
      .covers = {"LUBAN-CLI-E2E", "LUBAN-CLI-MAP"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-new", *bin);

    auto empty = luban(home, {});
    ASSERT_EQ(empty.exit_code, 0) << empty.transcript();
    EXPECT_NE(empty.out.find("on this host"), std::string::npos) << empty.out;
    EXPECT_NE(empty.out.find("no environments yet"), std::string::npos) << empty.out;

    auto made = luban(home, {"new", "box", "fixture:my-os@1.0.0"}, /*pty=*/true);
    ASSERT_FALSE(made.timed_out) << made.transcript();
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "data/xpkgs/fixture-x-hello/1.0.0/bin/hello"));

    auto overview = luban(home, {"--json"});
    ASSERT_EQ(overview.exit_code, 0) << overview.transcript();
    const auto j = Json::parse(overview.out);
    EXPECT_EQ(j["place"], "host");
    ASSERT_EQ(j["environments"].size(), 1u) << overview.out;
    EXPECT_EQ(j["environments"][0]["name"], "box");

    auto listed = luban(home, {"ls"});
    ASSERT_EQ(listed.exit_code, 0) << listed.transcript();
    EXPECT_NE(listed.out.find("box"), std::string::npos) << listed.out;

    auto status = luban(home, {"status", "box", "--json"});
    ASSERT_EQ(status.exit_code, 0) << status.transcript();
    EXPECT_TRUE(Json::parse(status.out, nullptr, false).is_object()) << status.out;
}

XTEST(Luban, AnAgentIsNeverAskedAndAPersonIsToldTheNearestCommand,
      .area = "luban", .covers = {"LUBAN-CLI-E2E", "UX-AGENT-NO-PROMPT", "LUBAN-CLI-SUGGEST"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "a Luban root is a Linux root";
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto home = luban_home("luban-agent", *bin);
    auto made = luban(home, {"new", "box", "fixture:my-os@1.0.0"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();

    // Removing asks first. An agent, with nobody to ask and no --yes: exit 2,
    // nothing removed, nothing waited for.
    auto refused = luban(home, {"--agent", "rm", "box"});
    ASSERT_FALSE(refused.timed_out) << "an agent waited on a question\n" << refused.transcript();
    EXPECT_EQ(refused.exit_code, 2) << refused.transcript();
    EXPECT_TRUE(fs::exists(home.dir() / "subos/box"));

    auto typo = luban(home, {"entr", "box"});
    EXPECT_EQ(typo.exit_code, 2);
    EXPECT_NE(typo.err.find("did you mean `luban enter`"), std::string::npos) << typo.transcript();
    auto typoJson = luban(home, {"--json", "entr"});
    EXPECT_EQ(Json::parse(typoJson.out)["suggestions"][0], "enter") << typoJson.out;

    auto removed = luban(home, {"rm", "box", "-y"});
    EXPECT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box"));
}
