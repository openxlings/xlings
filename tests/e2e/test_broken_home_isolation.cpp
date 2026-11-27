// One broken package or subos changes nothing for the rest of the home: an
// unreadable subos, a versions record of a package never installed, a
// deleted overlay recipe, and a registration pointing at a missing directory
// at once, while an untouched sibling package keeps working, byte for byte
// (E2E-106 of 2026.9.12, once broken_home_isolation_test.sh; #592).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.core.config;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

#include "home_contract_support.hpp"

namespace hc = home_contract;

namespace {

// The payload's files with their contents, and the subos's shim names.
std::string snapshot(const fs::path& payload, const fs::path& bin) {
    std::vector<std::string> rows;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(payload, ec))
        if (e.is_regular_file(ec))
            rows.push_back(fs::relative(e.path(), payload).generic_string() + " "
                           + xlings::sha256::hex(tk::read_file(e.path())));
    for (const auto& e : fs::directory_iterator(bin, ec)) rows.push_back("shim " + e.path().filename().string());
    std::ranges::sort(rows);
    std::string out;
    for (const auto& r : rows) out += r + "\n";
    return out;
}

std::string overlay_recipe(std::string_view name) {
    return std::format(R"LUA(package = {{
    spec = "1", name = "{0}", description = "an overlay fixture, never installed",
    type = "package", archs = {{"x86_64", "aarch64"}}, status = "stable",
    xpm = {{ linux = {{ ["1.0.0"] = {{}} }}, macosx = {{ ["1.0.0"] = {{}} }}, windows = {{ ["1.0.0"] = {{}} }} }},
}}
)LUA", name);
}

}  // namespace

XTEST(BrokenHomeIsolation, FourDamagesAtOnceLeaveAnUntouchedPackageAsItWas, .area = "xim",
      .covers = {"BROKEN-HOME-ISOLATION"}, .requires_ = {"xlings-bin"}) {
    hc::Program plain{ .name = "isoplain", .programs = true };
    hc::Program hookfail{ .name = "isohookfail", .uninstall = "xvm.remove(\"isohookfail\")\n    error(\"boom\")",
                          .programs = true };
    auto home = hc::seeded("broken-home-isolation", {{"isoplain", hc::recipe(plain)},
                                                     {"isohookfail", hc::recipe(hookfail)}});
    const auto run = [&](std::vector<std::string> args) { return hc::in(home, "default", std::move(args)); };
    for (const auto* pkg : {"isoplain", "isohookfail"}) {
        ASSERT_EQ(run({"install", std::string(pkg) + "@1.0.0", "-y"}).exit_code, 0) << pkg;
        ASSERT_EQ(run({"use", pkg, "1.0.0"}).exit_code, 0) << pkg;
    }
    const auto bin = home.dir() / "subos" / "default" / "bin";
    const auto payload = home.dir() / "data" / "xpkgs" / "xim-x-isoplain" / "1.0.0";
    ASSERT_TRUE(hc::has_shim(bin, "isoplain"));
    ASSERT_TRUE(fs::is_directory(payload));
    const auto before = snapshot(payload, bin);

    // 1. An unreadable subos.
    tk::write_file(home.dir() / "subos" / "broken" / ".xlings.json", "{garbage");
    // 2. A record of a package never installed.
    hc::edit_versions(home, [&](Json& versions) {
        versions["ghost"] = Json{{"filename", "ghost"}, {"type", "program"},
            {"versions", Json{{"1.0", Json{{"kind", "program"},
                {"path", (home.dir() / "data" / "xpkgs" / "xim-x-ghost" / "1.0" / "bin").string()}}}}}};
    });
    // 3. An overlay recipe whose file is gone, beside a healthy one.
    const auto src = home.root() / "local-src";
    for (const auto* name : {"isolocal", "isolocalhealthy"}) {
        tk::write_file(src / (std::string(name) + ".lua"), overlay_recipe(name));
        const auto add = run({"config", "--add-xpkg", (src / (std::string(name) + ".lua")).string()});
        ASSERT_EQ(add.exit_code, 0) << add.transcript();
    }
    const auto overlay_file = home.dir() / "data" / "xim-pkgindex-local" / "pkgs" / "i" / "isolocal.lua";
    ASSERT_TRUE(fs::exists(overlay_file));
    fs::remove(overlay_file);
    // 4. A real package's registration pointing at a missing directory.
    hc::edit_versions(home, [&](Json& versions) {
        versions["isohookfail"]["versions"]["1.0.0"]["path"] =
            (home.dir() / "data" / "xpkgs" / "xim-x-isohookfail" / "1.0.0" / "does-not-exist").string();
    });

    hc::expect_survives("list", run({"list"}));
    hc::expect_survives("install isoplain", run({"install", "isoplain@1.0.0", "-y"}));
    hc::expect_survives("use isoplain", run({"use", "isoplain", "1.0.0"}));
    // The program is a shell script: its shim runs it where there is a /bin/sh.
    if (!tk::is_windows) {
        tk::RunOptions o;
        o.argv = {(bin / "isoplain").string(), "--version"};
        o.env = home.env();
        o.env["XLINGS_ACTIVE_SUBOS"] = "default";
        const auto shim = tk::run(o);
        hc::expect_survives("isoplain shim", shim);
        EXPECT_TRUE(hc::has(shim.transcript(), "isoplain@1.0.0")) << shim.transcript();
    }
    const auto doctor = run({"self", "doctor"});
    hc::expect_survives("self doctor", doctor);
    EXPECT_TRUE(hc::has(doctor.transcript(), "broken")) << doctor.transcript();
    hc::expect_survives("self doctor --deep", run({"self", "doctor", "--deep"}));
    const auto all = run({"list", "--all"});
    hc::expect_survives("list --all", all);
    EXPECT_TRUE(hc::has(all.transcript(), "could not be read")) << all.transcript();
    EXPECT_TRUE(hc::has(all.transcript(), "broken")) << all.transcript();
    const auto overlay = run({"config", "--list-xpkg"});
    hc::expect_survives("config --list-xpkg", overlay);
    EXPECT_TRUE(hc::has(overlay.transcript(), "isolocalhealthy")) << overlay.transcript();

    EXPECT_EQ(snapshot(payload, bin), before) << "isoplain's payload or this subos's shims changed";
}
