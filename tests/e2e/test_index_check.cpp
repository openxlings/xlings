// `xlings index check` (design 2026-11-27 §9.9): the format's checker over a
// recipe or an index -- each finding with its line, exit 1 on an error and 2
// on a path that is neither, notes with -v, every finding with --json.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

// The format's example of spec 3 (docs/specs/xpkg.md §10), its checksums filled.
constexpr std::string_view kGood = R"LUA(package = {
    spec = "3",
    name = "good",
    description = "A tool that does one thing",
    type = "package",
    archs = { "x86_64", "aarch64" },
    xpm = {
        linux = {
            strip = 1,
            ["latest"] = { ref = "1.2.0" },
            ["1.2.0"] = {
                url = "https://example.invalid/good-1.2.0-linux-${arch_alias}.tar.gz",
                arch_alias = { x86_64 = "amd64", aarch64 = "arm64" },
                sha256 = { x86_64 = "00", aarch64 = "11" },
            },
        },
    },
}
import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
function install()
    fs.chmod(path.join(pkginfo.install_dir(), "bin", "good"), "755")
end
)LUA";

// spec 3 with what the format refuses: an unstated architecture (line 6) and
// a name of the compatibility layer (line 11).
constexpr std::string_view kBad = R"LUA(package = {
    spec = "3", name = "bad", type = "package",
    archs = { "x86_64" },
    xpm = {
        linux = {
            ["1.0.0"] = { url = "https://example.invalid/bad.tar.gz", sha256 = "00" },
        },
    },
}
function install()
    os.mv("a", "b")
end
)LUA";

// spec 1: a note only (an older spec), and a warning for its unstated architecture.
constexpr std::string_view kOld = R"LUA(package = {
    spec = "1", name = "old", type = "package", archs = { "x86_64" },
    xpm = { linux = { ["1.0.0"] = { url = "https://example.invalid/old.tar.gz", sha256 = "00" } } },
}
)LUA";

}  // namespace

XTEST(IndexCheck, EachFindingWithItsLineAndAnErrorFails, .area = "xim", .covers = {"INDEX-CHECK"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("index-check");
    const auto index = home.root() / "idx";
    tk::write_file(index / "pkgs" / "g" / "good.lua", kGood);
    tk::write_file(index / "pkgs" / "b" / "bad.lua", kBad);
    tk::write_file(index / "pkgs" / "o" / "old.lua", kOld);

    const auto good = home.xlings({"index", "check", (index / "pkgs" / "g" / "good.lua").string()});
    EXPECT_EQ(good.exit_code, 0) << good.transcript();
    EXPECT_NE(good.transcript().find("1 recipe(s) checked: the xpkg format's rules hold"), std::string::npos)
        << good.transcript();

    const auto all = home.xlings({"index", "check", index.string()});
    const auto out = all.transcript();
    EXPECT_EQ(all.exit_code, 1) << out;
    EXPECT_NE(out.find("pkgs/b/bad.lua:6: error arch-unstated"), std::string::npos) << out;
    EXPECT_NE(out.find("pkgs/b/bad.lua:11: error compat-name"), std::string::npos) << out;
    EXPECT_NE(out.find("fs.move"), std::string::npos) << out;   // what replaces it
    EXPECT_NE(out.find("pkgs/o/old.lua:3: warning arch-unstated"), std::string::npos) << out;
    EXPECT_EQ(out.find("spec-old"), std::string::npos) << out;  // a note: -v only
    EXPECT_NE(out.find("3 recipe(s) checked"), std::string::npos) << out;

    const auto verbose = home.xlings({"index", "check", index.string(), "-v"});
    EXPECT_NE(verbose.transcript().find("info spec-old"), std::string::npos) << verbose.transcript();

    const auto json = home.xlings({"index", "check", index.string(), "--json"});
    EXPECT_EQ(json.exit_code, 1);
    const auto doc = nlohmann::json::parse(json.out, nullptr, false);
    ASSERT_TRUE(doc.is_object()) << json.transcript();
    EXPECT_EQ(doc.value("checked", 0), 3);
    EXPECT_GE(doc.value("errors", 0), 2);
    bool note = false;
    for (const auto& f : doc["findings"]) note = note || f.value("code", "") == "spec-old";
    EXPECT_TRUE(note) << json.out;

    const auto neither = home.xlings({"index", "check", (home.root() / "nothing-here").string()});
    EXPECT_EQ(neither.exit_code, 2) << neither.transcript();
    EXPECT_NE(neither.transcript().find("neither a recipe nor an index"), std::string::npos) << neither.transcript();
}
