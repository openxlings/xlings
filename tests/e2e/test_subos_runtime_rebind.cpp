// `xlings subos runtime <binding>`: the declaration and the active version
// change together (#607 item 2, design 2026-11-27 E3, §6.2). The store holds
// two versions of a runtime and the SubOS declares one; rebinding to the other
// used to be refused ("nothing was changed") -- the activation was judged
// against the old declaration -- and the new declaration was written anyway,
// with exit code 0 and the view on the old version.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.subos.manifest;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

// A libc in name: a loader and a libc.so.6 per version, registered as the
// index's glibc registers them.
void write_runtime_(const fs::path& repo) {
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/g/glibc.lua", R"LUA(
package = { spec = '1', name = 'glibc', type = 'package', archs = { 'x86_64', 'aarch64' },
    xpm = { linux = { ['2.44.2'] = {}, ['2.44.3'] = {} } },
    exports = { runtime = { loader = 'lib64/ld-linux-x86-64.so.2', abi = 'linux-x86_64-glibc', libdirs = { 'lib64' } } } }
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function install()
    local lib = path.join(pkginfo.install_dir(), 'lib64')
    os.mkdir(lib)
    io.writefile(path.join(lib, 'ld-linux-x86-64.so.2'), 'loader ' .. pkginfo.version())
    io.writefile(path.join(lib, 'libc.so.6'), 'libc ' .. pkginfo.version())
    return true
end
function config()
    xvm.add('glibc')
    xvm.add('libc.so.6', { type = 'lib', bindir = path.join(pkginfo.install_dir(), 'lib64'),
                           filename = 'libc.so.6', binding = 'glibc@' .. pkginfo.version() })
    return true
end
)LUA");
}

std::string declared_(const tk::Home& home) {
    const auto doc = Json::parse(tk::read_file(home.dir() / "subos" / "default" / ".xlings.json"));
    return std::string(xlings::subos::manifest::parse(doc).runtime);
}

std::string active_(const tk::Home& home) {
    const auto doc = Json::parse(tk::read_file(home.dir() / "subos" / "default" / ".xlings.json"));
    // The workspace's entry for a target: {"active": <version>, ...}.
    const auto workspace = doc.value("workspace", Json::object());
    if (!workspace.contains("glibc")) return "<none>";
    const auto& entry = workspace["glibc"];
    if (entry.is_string()) return entry.get<std::string>();
    return entry.value("active", std::string("<none>"));
}

}  // namespace

XTEST(SubosRuntimeRebind, TheDeclarationAndTheActiveVersionMoveTogether, .area = "subos",
      .covers = {"RESULT-OUTCOME"}, .requires_ = {"xlings-bin", "linux"}) {
    auto home = tk::Home::isolated("subos-runtime-rebind");
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    const auto repo = home.root() / "index";
    write_runtime_(repo);
    const Json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / "xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    // The store holds both; the SubOS declares and runs 2.44.3.
    for (const auto* v : {"fixture:glibc@2.44.2", "fixture:glibc@2.44.3"}) {
        const auto r = home.xlings({"install", v, "-y"});
        ASSERT_EQ(r.exit_code, 0) << r.transcript();
    }
    const auto first = home.xlings({"subos", "runtime", "glibc@2.44.3", "default"});
    ASSERT_EQ(first.exit_code, 0) << first.transcript();
    ASSERT_EQ(declared_(home), "glibc@2.44.3") << first.transcript();

    // E3: rebinding to the other installed version succeeds, and the view
    // follows the declaration.
    const auto rebound = home.xlings({"subos", "runtime", "glibc@2.44.2", "default"});
    const auto out = rebound.transcript();
    EXPECT_EQ(rebound.exit_code, 0) << out;
    EXPECT_EQ(out.find("refused"), std::string::npos) << out;
    EXPECT_EQ(declared_(home), "glibc@2.44.2") << out;
    // The stored key keeps the package's index: fixture:<version>.
    EXPECT_EQ(active_(home), "fixture:2.44.2") << out;
}
