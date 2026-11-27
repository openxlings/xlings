// A registration rewrite -- remove and reinstall of the same target@version --
// refreshes the sysroot of every subos that pins that version, not only the
// one the command ran in, and never pushes the package into a subos that
// never had it; a corrupted pinning subos is named and does not block the
// rewrite here (E2E-105 of 2026.9.12, #586, once
// sysroot_refresh_pinning_subos_test.sh; #592).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

#include "home_contract_support.hpp"

namespace hc = home_contract;

namespace {

constexpr std::string_view kMarker = "@MARKER@";

// The layout the hooks lay out and register is gen1, or gen2 while the
// marker file exists: a reinstall then moves every registration.
constexpr std::string_view kLibfixture = R"LUA(package = {
    spec = "1", name = "libfixture", description = "a library whose registration moves",
    type = "package", archs = {"x86_64", "aarch64"}, status = "stable",
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
local SONAME = "libfixture.so.1"
local function gen()
    return os.isfile("@MARKER@") and "gen2" or "gen1"
end
function install()
    local dir = pkginfo.install_dir()
    local g = gen()
    os.tryrm(dir)
    os.mkdir(path.join(dir, g, "bin"))
    os.mkdir(path.join(dir, g, "lib"))
    os.mkdir(path.join(dir, g, "include"))
    io.writefile(path.join(dir, g, "bin", "libfixture"), "#!/bin/sh\necho libfixture " .. g .. "\n")
    io.writefile(path.join(dir, g, "lib", SONAME), "GEN " .. g .. "\n")
    io.writefile(path.join(dir, g, "include", "fixture.h"), "#define FIXTURE_GEN \"" .. g .. "\"\n")
    return true
end
function config()
    local dir = pkginfo.install_dir()
    local g = gen()
    xvm.add("libfixture", { bindir = path.join(dir, g, "bin") })
    xvm.add(SONAME, { type = "lib", bindir = path.join(dir, g, "lib"), filename = SONAME, alias = SONAME,
                      binding = "libfixture@" .. pkginfo.version() })
    table.insert(_XVM_OPS, { op = "headers", includedir = path.join(dir, g, "include") })
    return true
end
function uninstall()
    return true
end
)LUA";

// "gen1"/"gen2" of a sysroot library, or empty when it does not resolve.
std::string lib_gen(const fs::path& lib) {
    std::error_code ec;
    if (!fs::exists(lib, ec)) return {};
    const auto text = tk::read_file(lib);
    return text.starts_with("GEN ") ? text.substr(4, text.find('\n') - 4) : std::string{};
}

std::vector<fs::path> dangling(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_symlink(ec) && !fs::exists(e.path(), ec)) out.push_back(e.path());
    return out;
}

}  // namespace

// POSIX, where the script ran: a subos's library entry is a symbolic link
// there, and after the payload is rewritten it still names the recorded
// source. On Windows it is a hard link, which nothing proves xlings' once the
// file it shared is deleted: the refresh preserves it as unobserved
// (design 2026-11-27 §19.6, the row about this test).
XTEST(SysrootRefreshPinning, ARewriteReachesEverySubosThatPinsTheVersionAndNoOther, .area = "xvm",
      .covers = {"SYSROOT-REFRESH-PINNING"}, .requires_ = {"xlings-bin", "posix"}) {
    auto home = hc::seeded("sysroot-refresh-pinning", {{"libfixture", std::string(kLibfixture)}});
    const auto marker = home.root() / "relocate-marker";
    {
        const auto file = hc::recipe_file(hc::index_of(home), "libfixture");
        auto text = tk::read_file(file);
        text.replace(text.find(kMarker), kMarker.size(), marker.generic_string());
        tk::write_file(file, text);
        std::error_code ec;
        fs::remove(hc::index_of(home) / ".xlings-index-cache.json", ec);
    }
    ASSERT_EQ(hc::in(home, "default", {"subos", "new", "other"}).exit_code, 0);
    ASSERT_EQ(hc::in(home, "default", {"subos", "new", "bystander"}).exit_code, 0);
    const auto subos = home.dir() / "subos";
    const auto lib_default = subos / "default" / "lib" / "libfixture.so.1";
    const auto lib_other = subos / "other" / "lib" / "libfixture.so.1";
    const auto lib_bystander = subos / "bystander" / "lib" / "libfixture.so.1";
    const auto hdr_other = subos / "other" / "usr" / "include" / "fixture.h";
    const auto payload = home.dir() / "data" / "xpkgs" / "xim-x-libfixture" / "1.0.0";

    // S1: installed in default, pinned (and used) in other.
    const auto s1 = hc::in(home, "default", {"install", "libfixture@1.0.0", "-u", "-y"});
    ASSERT_EQ(s1.exit_code, 0) << s1.transcript();
    hc::edit_workspace(home, "other", [](Json& ws) {
        ws["libfixture"] = Json{{"installed", Json::array({"1.0.0"})}};
        ws["libfixture.so.1"] = Json{{"installed", Json::array({"1.0.0"})}};
    });
    const auto use = hc::in(home, "other", {"use", "libfixture", "1.0.0"});
    ASSERT_EQ(use.exit_code, 0) << use.transcript();
    EXPECT_EQ(lib_gen(lib_default), "gen1");
    EXPECT_EQ(lib_gen(lib_other), "gen1");
    EXPECT_FALSE(fs::exists(lib_bystander)) << "bystander never had it";

    // S2: the rewrite -- remove detaches (other still uses it), the payload
    // is gone, the reinstall lays out and registers gen2.
    tk::write_file(marker, "");
    const auto rm = hc::in(home, "default", {"remove", "libfixture", "-y"});
    ASSERT_EQ(rm.exit_code, 0) << rm.transcript();
    EXPECT_TRUE(hc::has_nocase(rm.transcript(), "still used by other")) << rm.transcript();
    fs::remove_all(payload);
    const auto s2 = hc::in(home, "default", {"install", "libfixture@1.0.0", "-u", "-y"});
    ASSERT_EQ(s2.exit_code, 0) << s2.transcript();
    EXPECT_EQ(lib_gen(lib_default), "gen2");

    // S3: other followed it, nothing dangles; S4: bystander still has nothing.
    EXPECT_TRUE(dangling(subos / "other" / "lib").empty());
    EXPECT_EQ(lib_gen(lib_other), "gen2") << s2.transcript();
    EXPECT_NE(tk::read_file(hdr_other).find("FIXTURE_GEN \"gen2\""), std::string::npos);
    EXPECT_FALSE(fs::exists(lib_bystander)) << "the refresh pushed it into a subos that never had it";

    // S5: other's workspace cannot be read: removal and the next rewrite name
    // it (`doctor --subos other`), and default's own sysroot still follows.
    tk::write_file(subos / "other" / ".xlings.json", "{garbage");
    fs::remove(marker);
    fs::remove_all(payload);
    const auto rm2 = hc::in(home, "default", {"remove", "libfixture", "-y"});
    ASSERT_EQ(rm2.exit_code, 0) << rm2.transcript();
    EXPECT_TRUE(hc::has(rm2.transcript(), "doctor --subos other")) << rm2.transcript();
    const auto s5 = hc::in(home, "default", {"install", "libfixture@1.0.0", "-u", "-y"});
    ASSERT_EQ(s5.exit_code, 0) << s5.transcript();
    EXPECT_TRUE(hc::has(s5.transcript(), "doctor --subos other")) << s5.transcript();
    EXPECT_EQ(lib_gen(lib_default), "gen1");
    EXPECT_FALSE(fs::exists(lib_bystander));
}
