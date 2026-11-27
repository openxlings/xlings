// A payload keeps the recipe it was installed with as an index of that one
// recipe, with the index libs it imports (design 2026-11-27 §20.1, #743): its
// hooks run with the libs the install had, whatever the index holds later. A
// payload an older client kept as `.xpkg.lua` alone is brought forward the
// first time it is read for a removal; a lib that is nowhere fails the hook
// that calls it, by name, instead of doing nothing.
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

constexpr std::string_view kMarkers = "@MARKERS@";

// config() and uninstall() write what the index lib answers; the lib asks a
// second lib, so the kept copy must be the closure, not the one file.
constexpr std::string_view kRecipe = R"LUA(package = {
    spec = "1", name = "probe", description = "a recipe that imports index libs",
    type = "package", archs = {"x86_64", "aarch64"}, status = "stable",
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.pkgindex.probelib")
function install()
    os.tryrm(pkginfo.install_dir())
    os.mkdir(path.join(pkginfo.install_dir(), "share"))
    io.writefile(path.join(pkginfo.install_dir(), "share", "probe.txt"), "probe")
    return true
end
function config()
    io.writefile("@MARKERS@/config.marker", probelib.word())
    return true
end
function uninstall()
    io.writefile("@MARKERS@/uninstall.marker", probelib.word())
    return true
end
)LUA";

std::string help_lib(std::string_view word) {
    return std::format("return {{ word = function() return \"{}\" end }}\n", word);
}

constexpr std::string_view kProbeLib =
    "import(\"xim.pkgindex.probehelp\")\n"
    "local M = {}\n"
    "function M.word() return probehelp.word() end\n"
    "return M\n";

struct Probe {
    tk::Home home;
    fs::path markers;
    fs::path libs;
};

Probe make_home(std::string_view name) {
    auto home = hc::seeded(name, {});
    const auto index = hc::index_of(home);
    const auto markers = home.root() / "markers";
    fs::create_directories(markers);
    auto text = std::string(kRecipe);
    for (auto at = text.find(kMarkers); at != std::string::npos; at = text.find(kMarkers))
        text.replace(at, kMarkers.size(), markers.generic_string());
    tk::write_file(hc::recipe_file(index, "probe"), text);
    tk::write_file(index / "libs" / "probelib.lua", std::string(kProbeLib));
    tk::write_file(index / "libs" / "probehelp.lua", help_lib("installed"));
    std::error_code ec;
    fs::remove(index / ".xlings-index-cache.json", ec);
    return { std::move(home), markers, index / "libs" };
}

fs::path payload_of(const tk::Home& home) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(home.dir() / "data" / "xpkgs", ec))
        if (e.path().filename().string().ends_with("-x-probe")) return e.path() / "1.0.0";
    return {};
}

std::string marker(const Probe& p, std::string_view which) {
    const auto file = p.markers / (std::string(which) + ".marker");
    return fs::exists(file) ? tk::read_file(file) : std::string("<none>");
}

}  // namespace

XTEST(KeptRecipe, TheRemovalRunsWithTheLibsTheInstallHad, .area = "xim",
      .covers = {"XPKG-SNAPSHOT-LIBS"}, .requires_ = {"xlings-bin"}) {
    auto p = make_home("kept-recipe-libs");
    auto& home = p.home;

    const auto install = home.xlings({"install", "probe@1.0.0", "-y"});
    ASSERT_EQ(install.exit_code, 0) << install.transcript();
    EXPECT_EQ(marker(p, "config"), "installed");

    // The payload keeps an index of its one recipe: the recipe where the index
    // had it, and the closure of the libs it imports.
    const auto payload = payload_of(home);
    ASSERT_FALSE(payload.empty());
    EXPECT_TRUE(fs::is_regular_file(payload / ".xpkg" / "pkgs" / "p" / "probe.lua"));
    EXPECT_TRUE(fs::is_regular_file(payload / ".xpkg" / "libs" / "probelib.lua"));
    EXPECT_TRUE(fs::is_regular_file(payload / ".xpkg" / "libs" / "probehelp.lua"));
    EXPECT_FALSE(fs::exists(payload / ".xpkg.lua"));

    // The index's lib breaks after the install; the removal does not read it.
    tk::write_file(p.libs / "probehelp.lua", "error(\"this lib is broken\")\n");
    const auto remove = home.xlings({"remove", "probe", "-y"});
    EXPECT_EQ(remove.exit_code, 0) << remove.transcript();
    EXPECT_EQ(marker(p, "uninstall"), "installed") << remove.transcript();
    hc::expect_gone(home, "probe", "1.0.0", "kept");
}

XTEST(KeptRecipe, AnOlderClientsRecipeAloneIsBroughtForward, .area = "xim",
      .covers = {"XPKG-SNAPSHOT-LIBS"}, .requires_ = {"xlings-bin"}) {
    auto p = make_home("kept-recipe-legacy");
    auto& home = p.home;
    ASSERT_EQ(home.xlings({"install", "probe@1.0.0", "-y"}).exit_code, 0);
    const auto payload = payload_of(home);
    ASSERT_FALSE(payload.empty());

    // What a client before §20.1 left: the recipe alone, at .xpkg.lua.
    const auto recipe = tk::read_file(payload / ".xpkg" / "pkgs" / "p" / "probe.lua");
    fs::remove_all(payload / ".xpkg");
    tk::write_file(payload / ".xpkg.lua", recipe);
    tk::write_file(p.libs / "probehelp.lua", help_lib("current"));

    // Brought forward with the libs the index has now, and its uninstall()
    // finds them (it found none from .xpkg.lua and did nothing).
    const auto remove = home.xlings({"remove", "probe", "-y"});
    EXPECT_EQ(remove.exit_code, 0) << remove.transcript();
    EXPECT_EQ(marker(p, "uninstall"), "current") << remove.transcript();
    hc::expect_gone(home, "probe", "1.0.0", "legacy");
}

XTEST(KeptRecipe, ALibThatIsNowhereFailsTheHookByName, .area = "xim",
      .covers = {"XPKG-SNAPSHOT-LIBS"}, .requires_ = {"xlings-bin"}) {
    auto p = make_home("kept-recipe-nowhere");
    auto& home = p.home;
    ASSERT_EQ(home.xlings({"install", "probe@1.0.0", "-y"}).exit_code, 0);
    const auto payload = payload_of(home);
    ASSERT_FALSE(payload.empty());
    const auto recipe = tk::read_file(payload / ".xpkg" / "pkgs" / "p" / "probe.lua");
    fs::remove_all(payload / ".xpkg");
    tk::write_file(payload / ".xpkg.lua", recipe);
    fs::remove_all(p.libs);

    // The hook fails, naming the call and the lib; the removal still withdraws
    // the package (as for any failing uninstall()).
    const auto remove = home.xlings({"remove", "probe", "-y"});
    EXPECT_EQ(remove.exit_code, 1) << remove.transcript();
    EXPECT_TRUE(hc::has(remove.transcript(), "probelib.word: import(\"xim.pkgindex.probelib\") did not load"))
        << remove.transcript();
    EXPECT_EQ(marker(p, "uninstall"), "<none>");
    hc::expect_gone(home, "probe", "1.0.0", "nowhere");
}

XTEST(KeptRecipe, AKeptRecipeThatCannotBeReadFailsTheHookNotSwapped, .area = "xim",
      .covers = {"XPKG-SNAPSHOT-LIBS"}, .requires_ = {"xlings-bin"}) {
    auto p = make_home("kept-recipe-unreadable");
    auto& home = p.home;
    ASSERT_EQ(home.xlings({"install", "probe@1.0.0", "-y"}).exit_code, 0);
    const auto payload = payload_of(home);
    ASSERT_FALSE(payload.empty());

    // The kept index cannot be read (its pkgs/ is no directory): the removal
    // does not run the index's recipe in its place; the hook fails, naming
    // it, and the package is still withdrawn (as for any failing uninstall()).
    fs::remove_all(payload / ".xpkg" / "pkgs");
    tk::write_file(payload / ".xpkg" / "pkgs", "not a directory\n");
    const auto remove = home.xlings({"remove", "probe", "-y"});
    EXPECT_EQ(remove.exit_code, 1) << remove.transcript();
    EXPECT_TRUE(hc::has(remove.transcript(), "cannot read the kept recipe")) << remove.transcript();
    EXPECT_EQ(marker(p, "uninstall"), "<none>");
    hc::expect_gone(home, "probe", "1.0.0", "unreadable");
}
