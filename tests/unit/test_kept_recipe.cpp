// What a payload keeps of the recipe it was installed with (design 2026-11-27
// §20.1, #743): the recipe as an index of one, and the closure of the index
// libs it imports -- a lib's own imports included, each once, a name that is
// not a lib of the index left out.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xim.payload;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xim = xlings::xim;

namespace {

fs::path scratch(std::string_view name) {
    const auto dir = fs::temp_directory_path() / std::format("xlings-kept-{}-{}", name,
        std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

}  // namespace

XTEST(KeptRecipe, TheLibsAreTheClosureOfWhatTheRecipeImports, .area = "xim", .covers = {"XPKG-SNAPSHOT-LIBS"}) {
    const auto dir = scratch("closure");
    const auto libs = dir / "libs";
    tk::write_file(libs / "a.lua", "import(\"xim.pkgindex.b\")\nreturn {}\n");
    tk::write_file(libs / "b.lua", "import(\"xim.pkgindex.a\")\nimport(\"xim.pkgindex.c.d\")\nreturn {}\n");
    tk::write_file(libs / "c.d.lua", "return {}\n");
    tk::write_file(libs / "unused.lua", "return {}\n");
    const auto recipe = "import(\"xim.libxpkg.pkginfo\")\nimport(\"xim.pkgindex.a\")\n"
                        "-- xim.pkgindex.gone is not a lib of this index\n";
    EXPECT_EQ(xim::imported_libs(recipe, libs), (std::vector<std::string>{"a", "b", "c.d"}));
    EXPECT_TRUE(xim::imported_libs(recipe, {}).empty());
    fs::remove_all(dir);
}

XTEST(KeptRecipe, KeepingReplacesEitherLayoutWithTheIndexOfOne, .area = "xim", .covers = {"XPKG-SNAPSHOT-LIBS"}) {
    const auto dir = scratch("layout");
    const auto payload = dir / "payload";
    const auto libs = dir / "libs";
    tk::write_file(libs / "a.lua", "return {}\n");
    tk::write_file(payload / ".xpkg.lua", "-- an older client's copy\n");
    EXPECT_EQ(xim::kept_recipe(payload), payload / ".xpkg.lua");

    const std::string text = "import(\"xim.pkgindex.a\")\n";
    ASSERT_TRUE(xim::keep_recipe(payload, text, fs::path("p") / "probe.lua", libs));
    const auto kept = payload / ".xpkg" / "pkgs" / "p" / "probe.lua";
    EXPECT_EQ(xim::kept_recipe(payload), kept);
    EXPECT_EQ(tk::read_file(kept), text);
    EXPECT_TRUE(fs::is_regular_file(payload / ".xpkg" / "libs" / "a.lua"));
    EXPECT_FALSE(fs::exists(payload / ".xpkg.lua"));

    // Kept again (a reinstall): the previous copy is replaced, not merged.
    ASSERT_TRUE(xim::keep_recipe(payload, "-- no imports\n", fs::path("p") / "probe.lua", libs));
    EXPECT_FALSE(fs::exists(payload / ".xpkg" / "libs" / "a.lua"));
    for (const auto& e : fs::directory_iterator(payload))
        EXPECT_FALSE(e.path().filename().string().starts_with(".xpkg.new-")) << e.path();
    fs::remove_all(dir);
}
