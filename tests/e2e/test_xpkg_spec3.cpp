// Recipes of spec "3" installed by the client (design 2026-11-27 §9.6, §9.7):
// the download is in install_dir, laid out as the entry's `strip` says, when
// install() starts; install() only adjusts it, or is absent; `fs.extract` is
// the client's extractor; a recipe of spec "2" with the same archive keeps the
// old path. The archives are served by a loopback server.
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

struct Fixture {
    fs::path primary, index, served;
    std::string base;
};

// `<name>-1.0.0.tar.gz`: one top-level directory `<name>-1.0.0/` with
// bin/<name> and README, and `extra` members at the top level.
std::string archive_(Fixture& f, std::string_view name, std::vector<xim::TarEntry> extra = {}) {
    const auto top = std::format("{}-1.0.0", name);
    std::vector<xim::TarEntry> entries{
        {.path = top, .mode = 0755, .directory = true},
        {.path = top + "/bin", .mode = 0755, .directory = true},
        {.path = top + "/bin/" + std::string(name), .content = "#!/bin/sh\necho probe\n", .mode = 0755},
        {.path = top + "/README", .content = "read me\n"}};
    for (auto& e : extra) entries.push_back(std::move(e));
    const auto file = f.served / (top + ".tar.gz");
    fs::create_directories(f.served);
    auto written = xim::write_tar_gz_entries(file, entries);
    if (!written) throw std::runtime_error(written.error());
    return xlings::sha256::hex(tk::read_file(file));
}

// A recipe of `spec` whose every platform downloads `<name>-1.0.0.tar.gz`;
// `strip` goes in the platform's part, `body` after the imports.
void recipe_(Fixture& f, std::string_view name, std::string_view spec, std::string_view strip,
             std::string_view sha, std::string_view body) {
    const auto entry = std::format(
        "{{ {} ['1.0.0'] = {{ url = '{}/{}-1.0.0.tar.gz', sha256 = {{ x86_64 = '{}', aarch64 = '{}' }} }} }}",
        strip.empty() ? std::string() : std::format("strip = {},", strip), f.base, name, sha, sha);
    tk::write_file(f.index / "pkgs" / name.substr(0, 1) / std::format("{}.lua", name), std::format(R"LUA(
package = {{
    spec = '{1}', name = '{0}', type = 'package', archs = {{ 'x86_64', 'aarch64' }},
    xpm = {{ linux = {2}, macosx = {2}, windows = {2} }},
}}
{3}
)LUA", name, spec, entry, body));
}

Fixture make_fixture_(const fs::path& root, std::string base) {
    Fixture f{root / "primary", root / "index", root / "served", std::move(base)};
    tk::write_file(f.primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(f.index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    // What an install on a Linux host without patchelf takes first,
    // payload free here: the closure is the same whatever the host has.
    tk::write_file(f.primary / "pkgs" / "p" / "patchelf.lua", R"LUA(
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
    constexpr std::string_view kImports =
        "import('xim.libxpkg.fs')\nimport('xim.libxpkg.path')\nimport('xim.libxpkg.pkginfo')\n";

    // install() sees the content before it runs, and adjusts it.
    recipe_(f, "landed", "3", "1", archive_(f, "landed"), std::string(kImports) + R"LUA(
function install()
    if not fs.is_file(path.join(pkginfo.install_dir(), 'bin', 'landed')) then
        error('install() ran before the download was placed')
    end
    fs.write(path.join(pkginfo.install_dir(), 'install-ran'), 'yes')
end
)LUA");
    // No install() at all.
    recipe_(f, "bare", "3", "1", archive_(f, "bare"), kImports);
    // install() fails: nothing installed.
    recipe_(f, "failing", "3", "1", archive_(f, "failing"), std::string(kImports) + R"LUA(
function install()
    error('simulated install failure')
end
)LUA");
    // fs.extract: the same extractor, as an action of the hook.
    recipe_(f, "extracting", "3", "1", archive_(f, "extracting"), std::string(kImports) + R"LUA(
function install()
    fs.extract(pkginfo.install_file(), path.join(pkginfo.install_dir(), 'again'), { strip = 1 })
end
)LUA");
    // The archive is not laid out as the entry says.
    recipe_(f, "badlayout", "3", "1",
            archive_(f, "badlayout", {{.path = "LICENSE", .content = "MIT\n"}}), kImports);
    // spec "2", the same shape of archive: extracted beside it for the hook.
    recipe_(f, "oldpath", "2", "", archive_(f, "oldpath"), R"LUA(
import('xim.libxpkg.pkginfo')
function install()
    local dir = pkginfo.install_file():replace('.tar.gz', '')
    os.tryrm(pkginfo.install_dir())
    os.mv(dir, pkginfo.install_dir())
    return true
end
)LUA");
    return f;
}

void init_home_(const tk::Home& home, const Fixture& f) {
    const nlohmann::json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", f.primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", f.primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", f.index.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"),
                  fs::copy_options::overwrite_existing);
    const auto init = home.xlings({"self", "init"});
    ASSERT_EQ(init.exit_code, 0) << init.transcript();
}

fs::path payload_(const tk::Home& home, std::string_view name) {
    return home.dir() / "data" / "xpkgs" / std::format("fixture-x-{}", name) / "1.0.0";
}

std::set<std::string> tree_(const fs::path& root) {
    std::set<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        const auto rel = fs::relative(e.path(), root).generic_string();
        if (rel.starts_with(".xpkg") || rel.starts_with(".xlings")) continue;   // the client's records
        out.insert(rel + (e.is_directory() ? "/" : ""));
    }
    return out;
}

struct Served {
    tk::Home home;
    Fixture f;
    tk::fixture::Server server;
};

std::unique_ptr<Served> serve_(std::string_view name) {
    auto home = tk::Home::isolated(name);
    const auto root = home.root() / "fixture";
    fs::create_directories(root / "served");
    auto server = tk::fixture::Server::start(root / "served");
    if (!server) throw std::runtime_error(server.error());
    auto f = make_fixture_(root, server->url());
    auto s = std::make_unique<Served>(Served{std::move(home), std::move(f), std::move(*server)});
    init_home_(s->home, s->f);
    return s;
}

}  // namespace

XTEST(XpkgSpec3, TheDownloadIsInPlaceBeforeInstallRunsAndWithoutOne, .area = "xim",
      .covers = {"LAND-BEFORE-INSTALL", "SPEC-GATE"}, .requires_ = {"xlings-bin"}) {
    auto s = serve_("spec3-landing");
    const auto landed = s->home.xlings({"install", "fixture:landed@1.0.0", "-y"});
    ASSERT_EQ(landed.exit_code, 0) << landed.transcript();
    EXPECT_EQ(tree_(payload_(s->home, "landed")),
              (std::set<std::string>{"bin/", "bin/landed", "README", "install-ran"}))
        << landed.transcript();

    const auto bare = s->home.xlings({"install", "fixture:bare@1.0.0", "-y"});
    ASSERT_EQ(bare.exit_code, 0) << bare.transcript();
    EXPECT_EQ(tree_(payload_(s->home, "bare")), (std::set<std::string>{"bin/", "bin/bare", "README"}))
        << bare.transcript();

    // A recipe of spec "2" with the same shape of archive: its hook moves the
    // directory extracted beside the archive, as before.
    const auto old = s->home.xlings({"install", "fixture:oldpath@1.0.0", "-y"});
    ASSERT_EQ(old.exit_code, 0) << old.transcript();
    EXPECT_EQ(tree_(payload_(s->home, "oldpath")), (std::set<std::string>{"bin/", "bin/oldpath", "README"}))
        << old.transcript();
}

XTEST(XpkgSpec3, AFailedInstallLeavesNothingInstalled, .area = "xim",
      .covers = {"LAND-BEFORE-INSTALL"}, .requires_ = {"xlings-bin"}) {
    auto s = serve_("spec3-failing");
    const auto failed = s->home.xlings({"install", "fixture:failing@1.0.0", "-y"});
    const auto out = failed.transcript();
    EXPECT_NE(failed.exit_code, 0) << out;
    EXPECT_NE(out.find("simulated install failure"), std::string::npos) << out;
    const auto stamp = payload_(s->home, "failing") / ".xpkg-install.json";
    std::error_code ec;
    if (fs::exists(stamp, ec))
        EXPECT_NE(tk::read_file(stamp).find("\"incomplete\": true"), std::string::npos) << tk::read_file(stamp);
}

XTEST(XpkgSpec3, AnArchiveNotLaidOutAsTheEntrySaysFailsNamingTheMember, .area = "xim",
      .covers = {"LAND-STRIP"}, .requires_ = {"xlings-bin"}) {
    auto s = serve_("spec3-badlayout");
    const auto bad = s->home.xlings({"install", "fixture:badlayout@1.0.0", "-y"});
    const auto out = bad.transcript();
    EXPECT_NE(bad.exit_code, 0) << out;
    EXPECT_NE(out.find("LICENSE"), std::string::npos) << out;
    EXPECT_NE(out.find("strip = 1"), std::string::npos) << out;
}

XTEST(XpkgSpec3, FsExtractIsTheClientsExtractor, .area = "xim",
      .covers = {"FS-EXTRACT"}, .requires_ = {"xlings-bin"}) {
    auto s = serve_("spec3-extract");
    const auto r = s->home.xlings({"install", "fixture:extracting@1.0.0", "-y"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(tree_(payload_(s->home, "extracting")),
              (std::set<std::string>{"bin/", "bin/extracting", "README", "again/", "again/bin/",
                                     "again/bin/extracting", "again/README"}))
        << r.transcript();
}
