// A payload already on disk, installed into a second subos, is registered
// there too (E2E-107, once second_subos_install_test.sh); a package `--fix`
// removed and could not reinstall fails the fix, names the command that
// finishes it and withholds verifiedBy (E2E-108, once
// doctor_removed_not_reinstalled_test.sh). Contracts of 2026.9.12; #592.
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

constexpr std::string_view kLibfixture = R"LUA(package = {
    spec = "1", name = "libfixture", description = "a program, a library and a header",
    type = "package", archs = {"x86_64", "aarch64"}, status = "stable",
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
local SONAME = "libfixture.so.1"
function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "lib"))
    os.mkdir(path.join(dir, "include"))
    io.writefile(path.join(dir, "bin", "libfixture"), "#!/bin/sh\necho libfixture\n")
    io.writefile(path.join(dir, "lib", SONAME), "GEN 1\n")
    io.writefile(path.join(dir, "include", "fixture.h"), "#define FIXTURE_GEN \"1\"\n")
    return true
end
function config()
    local dir = pkginfo.install_dir()
    xvm.add("libfixture", { bindir = path.join(dir, "bin") })
    xvm.add(SONAME, { type = "lib", bindir = path.join(dir, "lib"), filename = SONAME, alias = SONAME,
                      binding = "libfixture@" .. pkginfo.version() })
    table.insert(_XVM_OPS, { op = "headers", includedir = path.join(dir, "include") })
    return true
end
function uninstall()
    return true
end
)LUA";

}  // namespace

XTEST(SecondSubosInstall, APayloadOnDiskIsRegisteredInTheSecondSubosToo, .area = "xim",
      .covers = {"SECOND-SUBOS-INSTALL"}, .requires_ = {"xlings-bin"}) {
    auto home = hc::seeded("second-subos-install", {{"libfixture", std::string(kLibfixture)}});
    ASSERT_EQ(hc::in(home, "default", {"install", "libfixture@1.0.0", "-y"}).exit_code, 0);
    ASSERT_TRUE(fs::is_directory(home.dir() / "data" / "xpkgs" / "xim-x-libfixture" / "1.0.0"));
    ASSERT_EQ(hc::in(home, "default", {"subos", "new", "other"}).exit_code, 0);

    const auto r = hc::in(home, "other", {"install", "libfixture@1.0.0", "-y"});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_FALSE(hc::has_nocase(r.transcript(), "is not installed in this subos")) << r.transcript();

    const auto ws = hc::read_json(home.dir() / "subos" / "other" / ".xlings.json");
    ASSERT_TRUE(ws.is_object() && ws.contains("workspace")) << ws.dump();
    const auto& entry = ws["workspace"].value("libfixture", Json());
    const auto active = entry.is_object() ? entry.value("active", std::string{})
                                          : entry.is_string() ? entry.get<std::string>() : std::string{};
    EXPECT_EQ(active, "1.0.0") << ws.dump();
    bool installed = entry.is_string() && entry.get<std::string>() == "1.0.0";
    if (entry.is_object() && entry.contains("installed"))
        for (const auto& v : entry["installed"]) installed = installed || v == "1.0.0";
    EXPECT_TRUE(installed) << ws.dump();

    // config() ran for "other": its shim, its library link, its header.
    const auto other = home.dir() / "subos" / "other";
    EXPECT_TRUE(hc::has_shim(other / "bin", "libfixture"));
    EXPECT_TRUE(fs::exists(other / "lib" / "libfixture.so.1"));
    EXPECT_TRUE(fs::exists(other / "usr" / "include" / "fixture.h"));
}

XTEST(DoctorRemovedNotReinstalled, AFixThatCouldNotPutAPackageBackFails, .area = "self",
      .covers = {"DOCTOR-REMOVED-NOT-REINSTALLED"}, .requires_ = {"xlings-bin"}) {
    // install() succeeds once, then never again: its marker lies outside the
    // payload, where no removal reaches.
    hc::Program unfixable{ .name = "unfixable", .guard = "if os.isfile(\"@MARKER@\") then return false end",
                           .after = "io.writefile(\"@MARKER@\", \"installed once\\n\")", .programs = true };
    auto home = hc::seeded("doctor-removed-not-reinstalled", {{"unfixable", hc::recipe(unfixable)}});
    const auto marker = home.root() / "unfixable-installed-once.marker";
    {
        const auto file = hc::recipe_file(hc::index_of(home), "unfixable");
        auto text = tk::read_file(file);
        for (auto at = text.find("@MARKER@"); at != std::string::npos; at = text.find("@MARKER@"))
            text.replace(at, 8, marker.generic_string());
        tk::write_file(file, text);
        std::error_code ec;
        fs::remove(hc::index_of(home) / ".xlings-index-cache.json", ec);
    }
    const auto run = [&](std::vector<std::string> args) { return hc::in(home, "default", std::move(args)); };
    ASSERT_EQ(run({"install", "unfixable@1.0.0", "-y"}).exit_code, 0);
    ASSERT_TRUE(fs::exists(marker));
    const auto payload = home.dir() / "data" / "xpkgs" / "xim-x-unfixable" / "1.0.0";
    ASSERT_TRUE(fs::exists(payload / "bin" / "unfixable"));
    fs::remove_all(payload);

    const auto deep = run({"self", "doctor", "--deep"});
    EXPECT_NE(deep.exit_code, 0) << deep.transcript();
    EXPECT_TRUE(hc::has(deep.transcript(), "unfixable")) << deep.transcript();

    const auto fix = run({"self", "doctor", "--fix", "-y"});
    EXPECT_NE(fix.exit_code, 0) << fix.transcript();
    EXPECT_TRUE(hc::has_nocase(fix.transcript(), "REMOVED but could not reinstall")) << fix.transcript();
    EXPECT_TRUE(hc::has(fix.transcript(), "xlings install unfixable@1.0.0")
                || hc::has(fix.transcript(), "xlings install xim:unfixable@1.0.0")) << fix.transcript();
    EXPECT_TRUE(hc::verified_by(home).empty());
    EXPECT_FALSE(hc::registered(home, "unfixable", "1.0.0")) << "the removal of R3 did not happen";

    const auto again = run({"self", "doctor", "--fix", "-y"});
    hc::expect_survives("a second --fix", again);
    EXPECT_FALSE(hc::has_nocase(again.transcript(), "REMOVED but could not reinstall")) << again.transcript();
}
