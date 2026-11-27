// `self doctor --fix` walks every subos that owns a finding, each in a
// process of its own, so a user never has to `subos use` first; an entry no
// subos claims is pruned rather than downloaded again, unless an unreadable
// subos might claim it; a subos whose name is no safe token is never run;
// a child that hangs is a failure, not a wait (E2E-104 and E2E-110 of
// 2026.9.12, once doctor_cross_subos_fix_test.sh and
// doctor_cross_subos_child_timeout_test.sh; #592).
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

// 9.9.9 is declared and never installed: an entry for it written by hand is
// one the catalog resolves and no workspace claims.
hc::Program xsf() { return { .name = "xsf-plain", .versions = {"1.0.0", "9.9.9"} }; }

fs::path payload(const tk::Home& home, std::string_view version) {
    return home.dir() / "data" / "xpkgs" / "xim-x-xsf-plain" / std::string(version);
}

bool in_workspace(const tk::Home& home, std::string_view subos, std::string_view name) {
    const auto ws = hc::read_json(home.dir() / "subos" / std::string(subos) / ".xlings.json");
    return ws.is_object() && ws.contains("workspace") && ws["workspace"].is_object()
        && ws["workspace"].contains(std::string(name));
}

bool has_reinstall(std::string_view text) {
    static const std::regex re(R"(install (xim:)?xsf-plain@9\.9\.9\s+\(\d+ entr)");
    const auto plain = hc::plain(text);
    return std::regex_search(plain, re);
}

}  // namespace

XTEST(DoctorCrossSubos, AFindingOfAnotherSubosIsRepairedThere, .area = "self",
      .covers = {"DOCTOR-CROSS-SUBOS"}, .requires_ = {"xlings-bin"}) {
    auto home = hc::seeded("doctor-cross-subos", {{"xsf-plain", hc::recipe(xsf())}});
    ASSERT_EQ(hc::in(home, "default", {"subos", "new", "other"}).exit_code, 0);
    ASSERT_EQ(hc::in(home, "other", {"install", "xsf-plain@1.0.0", "-y"}).exit_code, 0);
    ASSERT_TRUE(fs::is_directory(payload(home, "1.0.0")));
    fs::remove_all(payload(home, "1.0.0"));

    // I3: another subos's broken payload does not fail this one; the report
    // names it, and --fix repairs it in its own workspace.
    const auto report = hc::in(home, "default", {"self", "doctor"});
    EXPECT_EQ(report.exit_code, 0) << report.transcript();
    EXPECT_TRUE(hc::has(report.transcript(), "broken payload [subos: other]")) << report.transcript();
    const auto fix = hc::in(home, "default", {"self", "doctor", "--fix"});
    EXPECT_EQ(fix.exit_code, 0) << fix.transcript();
    EXPECT_TRUE(fs::exists(payload(home, "1.0.0") / "bin" / "xsf-plain")) << fix.transcript();
    EXPECT_TRUE(in_workspace(home, "other", "xsf-plain"));
    EXPECT_TRUE(hc::has(fix.transcript(), "repairing subos other")) << fix.transcript();
    const auto again = hc::in(home, "default", {"self", "doctor", "--fix", "--dry-run"});
    EXPECT_EQ(again.exit_code, 0) << again.transcript();
    EXPECT_FALSE(hc::has_nocase(again.transcript(), "would run")) << again.transcript();

    // I9: an entry of the versions database no subos claims is pruned, not reinstalled.
    hc::edit_versions(home, [&](Json& versions) {
        versions["xsf-plain"]["versions"]["9.9.9"] = Json{{"kind", "program"},
            {"path", (payload(home, "9.9.9") / "bin").string()}};
    });
    const auto i9 = hc::in(home, "default", {"self", "doctor", "--fix", "--dry-run"});
    EXPECT_NE(i9.exit_code, 0) << i9.transcript();
    EXPECT_TRUE(hc::has(i9.transcript(), "prune xsf-plain@9.9.9")) << i9.transcript();
    EXPECT_FALSE(has_reinstall(i9.transcript())) << i9.transcript();

    // I9b: an unreadable subos might claim it: not pruned, reinstalled, and named.
    tk::write_file(home.dir() / "subos" / "ghostly" / ".xlings.json", "{garbage");
    const auto i9b = hc::in(home, "default", {"self", "doctor", "--fix", "--dry-run"});
    EXPECT_NE(i9b.exit_code, 0) << i9b.transcript();
    EXPECT_FALSE(hc::has(i9b.transcript(), "prune xsf-plain@9.9.9")) << i9b.transcript();
    EXPECT_TRUE(has_reinstall(i9b.transcript())) << i9b.transcript();
    EXPECT_TRUE(hc::has_nocase(i9b.transcript(), "ghostly")) << i9b.transcript();
}

// I5: the subos that owns a finding is named "-bad": never run as a child
// (no `--subos -bad`), reported as not run, the fix fails, verifiedBy is not written.
XTEST(DoctorCrossSubos, ASubosNamedNoSafeTokenIsNeverRun, .area = "self",
      .covers = {"DOCTOR-CROSS-SUBOS"}, .requires_ = {"xlings-bin"}) {
    auto home = hc::seeded("doctor-cross-subos-unsafe", {{"xsf-plain", hc::recipe(xsf())}});
    ASSERT_EQ(hc::in(home, "default", {"install", "xsf-plain@1.0.0", "-y"}).exit_code, 0);
    hc::edit_workspace(home, "default", [](Json& ws) { ws.erase("xsf-plain"); });
    fs::remove_all(payload(home, "1.0.0"));
    tk::write_file(home.dir() / "subos" / "-bad" / ".xlings.json", R"({ "workspace": { "xsf-plain": "1.0.0" } })");

    const auto r = hc::in(home, "default", {"self", "doctor", "--fix"});
    EXPECT_EQ(r.exit_code, 1) << r.transcript();
    EXPECT_TRUE(hc::has(r.transcript(), "-bad")) << r.transcript();
    EXPECT_TRUE(hc::has_nocase(r.transcript(), "not a safe shell token")) << r.transcript();
    EXPECT_FALSE(hc::has(r.transcript(), "--subos -bad")) << r.transcript();
    EXPECT_TRUE(hc::verified_by(home).empty());
}

// E2E-110: the repair of another subos hangs (its install() sleeps): with
// XLINGS_DOCTOR_CHILD_TIMEOUT=1 the child is killed, the fix fails naming
// the subos and the timeout, verifiedBy is not written, and it ends soon.
XTEST(DoctorCrossSubos, AChildThatHangsIsAFailure, .area = "self",
      .covers = {"DOCTOR-CROSS-SUBOS"}, .requires_ = {"xlings-bin"}) {
    constexpr std::string_view kSleepy = R"LUA(package = {
    spec = "1", name = "sleepy", description = "install() hangs",
    type = "package", archs = {"x86_64", "aarch64"}, status = "stable", programs = {"sleepy"},
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} }, windows = { ["1.0.0"] = {} } },
}
function install()
    if os.host() == "windows" then os.exec("ping -n 7 127.0.0.1") else os.exec("sleep 6") end
    return false
end
function uninstall() return true end
)LUA";
    auto home = hc::seeded("doctor-child-timeout", {{"sleepy", std::string(kSleepy)}});
    ASSERT_EQ(hc::in(home, "default", {"subos", "new", "other"}).exit_code, 0);
    hc::edit_versions(home, [&](Json& versions) {
        versions["sleepy"] = Json{{"filename", "sleepy"}, {"type", "program"},
            {"versions", Json{{"1.0.0", Json{{"kind", "program"},
                {"path", (home.dir() / "data" / "xpkgs" / "xim-x-sleepy" / "1.0.0" / "bin").string()}}}}}};
    });
    hc::edit_workspace(home, "other", [](Json& ws) {
        ws["sleepy"] = Json{{"active", "1.0.0"}, {"installed", Json::array({"1.0.0"})}};
    });
    const std::map<std::string, std::string> env{{"XLINGS_DOCTOR_CHILD_TIMEOUT", "1"}};

    const auto report = hc::in(home, "default", {"self", "doctor"}, env);
    EXPECT_TRUE(hc::has_nocase(report.transcript(), "sleepy")) << report.transcript();

    const auto start = std::chrono::steady_clock::now();
    const auto fix = hc::in(home, "default", {"self", "doctor", "--fix", "-y"}, env);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(30)) << "the hung child was never killed\n" << fix.transcript();
    EXPECT_NE(fix.exit_code, 0) << fix.transcript();
    EXPECT_TRUE(hc::has_nocase(fix.transcript(), "timed out")) << fix.transcript();
    EXPECT_TRUE(hc::has(fix.transcript(), "other")) << fix.transcript();
    EXPECT_TRUE(hc::verified_by(home).empty());
}
