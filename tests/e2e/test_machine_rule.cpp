// Rule M at install (design 2026-11-27 §5.4): a payload that gives this machine
// a program of another machine fails, before anything is registered, and is
// not kept; another machine's files outside what the host runs or loads (a
// cross toolchain's target libraries) do not matter. The archives are served
// by a loopback server.
#include <gtest/gtest.h>
import xlings.testkit;
import xlings.testkit.fixture;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.core.config;
import xlings.core.xim.extract;
import xlings.platform.target;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xim = xlings::xim;

namespace {

bool host_is_x86_64() { return std::string_view(xlings::platform::build_arch()) == "x86_64"; }
std::uint16_t host_machine() { return host_is_x86_64() ? 0x3e : 0xb7; }
std::uint16_t other_machine() { return host_is_x86_64() ? 0xb7 : 0x3e; }
std::string other_name() { return host_is_x86_64() ? "aarch64" : "x86_64"; }

std::string elf_bytes_(const fs::path& scratch, std::uint16_t machine) {
    const auto file = tk::write_elf(scratch / std::format("elf-{}", machine), { .machine = machine });
    return tk::read_file(file);
}

// `<name>-1.0.0.tar.gz` with `<name>-1.0.0/` and the given files in it, served;
// a recipe of spec "3" that lands it and registers `bin/<name>`, with `linux`
// (fields of its linux block) besides.
void package_(const fs::path& index, const fs::path& served, const std::string& base, const std::string& name,
              const std::vector<std::pair<std::string, std::string>>& files, std::string_view linux = {}) {
    const auto top = name + "-1.0.0";
    std::vector<xim::TarEntry> entries{{.path = top, .mode = 0755, .directory = true}};
    std::set<std::string> dirs;
    for (const auto& [path, content] : files) {
        for (auto d = fs::path(path).parent_path(); !d.empty(); d = d.parent_path())
            dirs.insert(d.generic_string());
    }
    for (const auto& d : dirs) entries.push_back({.path = top + "/" + d, .mode = 0755, .directory = true});
    std::ranges::sort(entries, {}, &xim::TarEntry::path);
    for (const auto& [path, content] : files)
        entries.push_back({.path = top + "/" + path, .content = content, .mode = 0755});
    const auto archive = served / (top + ".tar.gz");
    auto written = xim::write_tar_gz_entries(archive, entries);
    if (!written) throw std::runtime_error(written.error());
    const auto sha = xlings::sha256::hex(tk::read_file(archive));
    tk::write_file(index / "pkgs" / name.substr(0, 1) / (name + ".lua"), std::format(R"LUA(
package = {{
    spec = '3', name = '{0}', type = 'package', archs = {{ 'x86_64', 'aarch64' }},
    xpm = {{ linux = {{ strip = 1, {3} ['1.0.0'] = {{ url = '{1}/{0}-1.0.0.tar.gz', sha256 = {{ x86_64 = '{2}', aarch64 = '{2}' }} }} }} }},
}}
import('xim.libxpkg.path')
import('xim.libxpkg.pkginfo')
import('xim.libxpkg.xvm')
function config()
    xvm.add('{0}', {{ bindir = path.join(pkginfo.install_dir(), 'bin') }})
end
)LUA", name, base, sha, linux));
}

}  // namespace

XTEST(MachineRule, AProgramOfAnotherMachineFailsTheInstallAndIsNotKept, .area = "xim",
      .covers = {"ARCH-MACHINE-RULE", "ARCH-DOCTOR"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("machine-rule-install");
    const auto root = home.root() / "fixture";
    const auto primary = root / "primary", index = root / "index", served = root / "served";
    fs::create_directories(served);
    auto server = tk::fixture::Server::start(served);
    ASSERT_TRUE(server) << server.error();
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    // What an install on a host without patchelf takes first, payload free.
    tk::write_file(primary / "pkgs" / "p" / "patchelf.lua", R"LUA(
package = {
    spec = '1', name = 'patchelf', type = 'package', archs = {'x86_64', 'aarch64'},
    xpm = { linux = { ['0.18.0'] = {} } },
}
import('xim.libxpkg.pkginfo')
function install()
    os.mkdir(pkginfo.install_dir())
    io.writefile(path.join(pkginfo.install_dir(), 'marker'), 'x')
    return true
end
)LUA");
    const auto scratch = home.root() / "elves";
    const auto ours = elf_bytes_(scratch, host_machine());
    const auto theirs = elf_bytes_(scratch, other_machine());
    // The program it registers is another machine's.
    package_(index, served, server->url(), "foreign", {{"bin/foreign", theirs}});
    // Another machine's library where the host does not load it (a cross
    // toolchain's target sysroot), and a program of this machine.
    package_(index, served, server->url(), "crossish",
             {{"bin/crossish", ours}, {"target/lib/libc.so.6", theirs}});
    // Another machine's libraries in lib/: what consumers and the host load
    // when the package declares no libdirs; a sysroot of another machine says
    // `libdirs = {}`, it exports none.
    package_(index, served, server->url(), "liblike", {{"bin/liblike", ours}, {"lib/libc.so.6", theirs}});
    package_(index, served, server->url(), "sysrootish", {{"bin/sysrootish", ours}, {"lib/libc.so.6", theirs}},
             "exports = { runtime = { libdirs = {} } },");
    const nlohmann::json config = {
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
             {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", index.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / "xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);

    const auto refused = home.xlings({"install", "fixture:foreign@1.0.0", "-y"});
    const auto out = refused.transcript();
    EXPECT_NE(refused.exit_code, 0) << out;
    EXPECT_NE(out.find("bin/foreign is for " + other_name()), std::string::npos) << out;
    EXPECT_NE(out.find("rule M"), std::string::npos) << out;
    EXPECT_NE(out.find("xim-pkgindex/issues"), std::string::npos) << out;
    // Not kept, not registered.
    EXPECT_FALSE(fs::exists(home.dir() / "data" / "xpkgs" / "fixture-x-foreign" / "1.0.0")) << out;
    const auto state = tk::read_file(home.dir() / ".xlings.json");
    EXPECT_EQ(state.find("\"foreign\""), std::string::npos) << state;
    EXPECT_FALSE(fs::exists(fs::symlink_status(home.dir() / "subos" / "default" / "bin" / "foreign"))) << out;

    // An interface client reads the code.
    const auto wire = home.xlings({"interface", "install_packages", "--args",
                                   nlohmann::json{{"targets", {"fixture:foreign@1.0.0"}}, {"yes", true}}.dump()});
    bool coded = false;
    for (const auto& line : wire.json_lines())
        if (line.value("kind", "") == "error" && line.value("code", "") == "E_FOREIGN_MACHINE") coded = true;
    EXPECT_TRUE(coded) << wire.transcript();

    const auto cross = home.xlings({"install", "fixture:crossish@1.0.0", "-y"});
    EXPECT_EQ(cross.exit_code, 0) << cross.transcript();
    EXPECT_TRUE(fs::is_regular_file(home.dir() / "data" / "xpkgs" / "fixture-x-crossish" / "1.0.0"
                                    / "target" / "lib" / "libc.so.6")) << cross.transcript();
    const auto liblike = home.xlings({"install", "fixture:liblike@1.0.0", "-y"});
    EXPECT_NE(liblike.exit_code, 0) << liblike.transcript();
    EXPECT_NE(liblike.transcript().find("lib/libc.so.6 is for " + other_name()), std::string::npos)
        << liblike.transcript();
    const auto sysroot = home.xlings({"install", "fixture:sysrootish@1.0.0", "-y"});
    EXPECT_EQ(sysroot.exit_code, 0) << sysroot.transcript();
    EXPECT_TRUE(fs::is_regular_file(home.dir() / "data" / "xpkgs" / "fixture-x-sysrootish" / "1.0.0"
                                    / "lib" / "libc.so.6")) << sysroot.transcript();
    const auto healthy = home.xlings({"self", "doctor"});
    EXPECT_EQ(healthy.transcript().find("foreign machine"), std::string::npos) << healthy.transcript();

    // A payload installed before the rule, its program another machine's: the
    // doctor reports it with the command, and leaves it.
    const auto program = home.dir() / "data" / "xpkgs" / "fixture-x-crossish" / "1.0.0" / "bin" / "crossish";
    tk::write_file(program, theirs);
    const auto doctor = home.xlings({"self", "doctor"});
    const auto said = doctor.transcript();
    EXPECT_NE(doctor.exit_code, 0) << said;
    EXPECT_NE(said.find("foreign machine"), std::string::npos) << said;
    EXPECT_NE(said.find("bin/crossish is for " + other_name()), std::string::npos) << said;
    EXPECT_NE(said.find("xlings remove fixture:crossish@1.0.0 -y"), std::string::npos) << said;
    const auto fixed = home.xlings({"self", "doctor", "--fix", "-y"});
    EXPECT_TRUE(fs::is_regular_file(program)) << fixed.transcript();
}
