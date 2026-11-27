// A SubOS's declaration names the version installed and active (design
// 2026-11-27 §7.2, #607 item 1): `subos runtime glibc@~2.44` declares the
// version the request resolved to and keeps the request beside it; a home an
// older client left declaring a version the store lacks, while its view
// serves another, is brought to what is served once, recorded.
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

// A libc in name, in the default index (the one a runtime is resolved in).
tk::Home home_(std::string_view name) {
    auto home = tk::Home::isolated(name);
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(primary / "pkgs/g/glibc.lua", R"LUA(
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
    const Json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / "xlings");
    const auto init = home.xlings({"self", "init"});
    EXPECT_EQ(init.exit_code, 0) << init.transcript();
    return home;
}

Json block_(const tk::Home& home) {
    const auto doc = Json::parse(tk::read_file(home.dir() / "subos" / "default" / ".xlings.json"));
    return doc.value(std::string(xlings::subos::manifest::BLOCK), Json::object());
}

}  // namespace

XTEST(VersionDeclared, ARebindToAVersionLineDeclaresWhatItResolvedTo, .area = "subos",
      .covers = {"VERSION-DECLARED"}, .requires_ = {"xlings-bin", "linux"}) {
    auto home = home_("version-declared-rebind");
    // `self init` declared the index's newest; start from the other one,
    // installed by the rebind itself.
    const auto start = home.xlings({"subos", "runtime", "glibc@2.44.2", "default"});
    ASSERT_EQ(start.exit_code, 0) << start.transcript();
    EXPECT_EQ(block_(home).value("runtime", ""), "glibc@2.44.2");
    EXPECT_FALSE(block_(home).contains("runtime_request")) << block_(home).dump(2);

    const auto r = home.xlings({"subos", "runtime", "glibc@~2.44", "default"});
    const auto out = r.transcript();
    ASSERT_EQ(r.exit_code, 0) << out;
    const auto block = block_(home);
    EXPECT_EQ(block.value("runtime", ""), "glibc@2.44.3") << block.dump(2);
    EXPECT_EQ(block.value("runtime_request", ""), "glibc@~2.44") << block.dump(2);
    EXPECT_NE(out.find("now declares glibc@2.44.3 (asked glibc@~2.44)"), std::string::npos) << out;
    // The view serves the declared version.
    EXPECT_EQ(xlings::subos::manifest::sysroot_runtime(home.dir() / "subos" / "default"), "glibc@2.44.3");

}

XTEST(VersionDeclared, AnOlderDeclarationOfAVersionNotInTheStoreBecomesWhatIsServedOnce, .area = "subos",
      .covers = {"VERSION-DECLARED"}, .requires_ = {"xlings-bin", "linux"}) {
    auto home = home_("version-declared-format");
    const auto installed = home.xlings({"install", "glibc@2.44.3", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    ASSERT_EQ(xlings::subos::manifest::sysroot_runtime(home.dir() / "subos" / "default"), "glibc@2.44.3");
    // What `subos runtime glibc@2.44` used to write: the request, a version
    // the store does not have, while the view serves 2.44.3.
    const auto manifest = home.dir() / "subos" / "default" / ".xlings.json";
    auto doc = Json::parse(tk::read_file(manifest));
    doc[std::string(xlings::subos::manifest::BLOCK)]["runtime"] = "glibc@2.44";
    tk::write_file(manifest, doc.dump(2));

    const auto fix = home.xlings({"self", "doctor", "--fix", "-y"});
    EXPECT_EQ(block_(home).value("runtime", ""), "glibc@2.44.3") << fix.transcript();
    EXPECT_EQ(block_(home).value("runtime_request", ""), "glibc@2.44") << fix.transcript();
    // Once: a second run has nothing to bring.
    const auto again = home.xlings({"self", "doctor", "--fix", "-y"});
    EXPECT_EQ(again.transcript().find("runtime-declaration"), std::string::npos) << again.transcript();
    EXPECT_EQ(block_(home).value("runtime", ""), "glibc@2.44.3");
}
