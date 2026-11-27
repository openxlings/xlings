// #736 (design 2026-11-27 §8.4): a node's archive is extracted into a
// directory of its own. Two flat archives (no wrapper directory) installed one
// after the other used to merge into one shared tree, and a recipe moving
// `lib/` from beside its archive took the other package's `lib/` with it.
#include <gtest/gtest.h>
import xlings.testkit;
import xlings.testkit.fixture;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.core.config;
import xlings.core.xim.extract;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xim = xlings::xim;

namespace {

// A flat `<name>-1.0.0.tar.gz` (lib/lib<name>.so at its top level), and a
// recipe of spec "2" whose install() moves `lib/` from beside the archive.
// `take` is how install() takes its files: "mv" the whole `lib/`, or "cp" the
// one library, leaving the extracted tree where it was (as ollama's did).
void flat_(const fs::path& index, const fs::path& served, const std::string& base, const std::string& name,
           std::string_view take) {
    const auto archive = served / (name + "-1.0.0.tar.gz");
    const std::vector<xim::TarEntry> entries{
        {.path = "lib", .mode = 0755, .directory = true},
        {.path = "lib/lib" + name + ".so", .content = name + "\n"}};
    auto written = xim::write_tar_gz_entries(archive, entries);
    if (!written) throw std::runtime_error(written.error());
    const auto sha = xlings::sha256::hex(tk::read_file(archive));
    const auto entry = std::format("{{ ['1.0.0'] = {{ url = '{}/{}-1.0.0.tar.gz', sha256 = {{ x86_64 = '{}', aarch64 = '{}' }} }} }}",
                                   base, name, sha, sha);
    tk::write_file(index / "pkgs" / name.substr(0, 1) / (name + ".lua"), std::format(R"LUA(
package = {{
    spec = '2', name = '{0}', type = 'package', archs = {{ 'x86_64', 'aarch64' }},
    xpm = {{ linux = {1}, macosx = {1}, windows = {1} }},
}}
import('xim.libxpkg.pkginfo')
function install()
    local here = path.directory(pkginfo.install_file())
    os.mkdir(pkginfo.install_dir())
    if '{2}' == 'mv' then
        os.mv(path.join(here, 'lib'), path.join(pkginfo.install_dir(), 'lib'))
    else
        os.mkdir(path.join(pkginfo.install_dir(), 'lib'))
        os.cp(path.join(here, 'lib', 'lib{0}.so'), path.join(pkginfo.install_dir(), 'lib', 'lib{0}.so'))
    end
    return true
end
)LUA", name, entry, take));
}

std::set<std::string> files_(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir)) out.insert(e.path().filename().string());
    return out;
}

}  // namespace

XTEST(ExtractOwnDir, AFlatArchiveSeesOnlyItsOwnFiles, .area = "xim",
      .covers = {"EXTRACT-OWN-DIR"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("extract-own-dir");
    const auto root = home.root() / "fixture";
    const auto primary = root / "primary", index = root / "index", served = root / "served";
    fs::create_directories(served);
    auto server = tk::fixture::Server::start(served);
    ASSERT_TRUE(server) << server.error();
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
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
)LUA");
    tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    flat_(index, served, server->url(), "flata", "cp");
    flat_(index, served, server->url(), "flatb", "mv");
    const nlohmann::json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", index.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    for (const auto* name : {"fixture:flata@1.0.0", "fixture:flatb@1.0.0"}) {
        const auto r = home.xlings({"install", name, "-y"});
        ASSERT_EQ(r.exit_code, 0) << r.transcript();
    }
    const auto store = home.dir() / "data" / "xpkgs";
    EXPECT_EQ(files_(store / "fixture-x-flata" / "1.0.0" / "lib"), (std::set<std::string>{"libflata.so"}));
    EXPECT_EQ(files_(store / "fixture-x-flatb" / "1.0.0" / "lib"), (std::set<std::string>{"libflatb.so"}));
    // And nothing of either is left for the next package.
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(home.dir() / "data" / "runtimedir", ec))
        EXPECT_NE(e.path().filename(), "lib") << e.path();
}
