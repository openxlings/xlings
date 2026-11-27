// `xlings remove` withdraws state before the recipe's uninstall() runs, and
// `--force` means "make it gone": a failing hook, a recipe gone from the
// index, a payload deleted by hand, a record with no binding leave no zombie
// (E2E-102 of 2026.9.12, once remove_force_contract_test.sh; #592). Every
// scenario ends with expect_gone: the versions database, every subos's
// workspace and shim, and the payload.
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

struct Remove {
    tk::Home home;
    fs::path plain_marker, hookfail_marker;
};

// The uninstall() hooks write a marker beside the home; its path is known
// once the home is, so the recipes name a placeholder until then.
constexpr std::string_view kMarkers = "@MARKERS@";

Remove make_home(std::string_view name) {
    hc::Program plain{ .name = "plain", .versions = {"1.0.0", "2.0.0"},
                       .uninstall = std::format("io.writefile(\"{}/plain-uninstall.marker\", \"ran\")\n"
                                                "    xvm.remove(\"plain\")\n    return true", kMarkers) };
    hc::Program hookfail{ .name = "hookfail",
                          .uninstall = std::format("io.writefile(\"{}/hookfail-uninstall.marker\", \"ran\")\n"
                                                   "    error(\"boom\")", kMarkers) };
    auto home = hc::seeded(name, {{"plain", hc::recipe(plain)}, {"hookfail", hc::recipe(hookfail)}});
    const auto markers = home.root() / "markers";
    fs::create_directories(markers);
    for (const auto* pkg : {"plain", "hookfail"}) {
        const auto file = hc::recipe_file(hc::index_of(home), pkg);
        auto text = tk::read_file(file);
        for (auto at = text.find(kMarkers); at != std::string::npos; at = text.find(kMarkers))
            text.replace(at, kMarkers.size(), markers.generic_string());
        tk::write_file(file, text);
    }
    std::error_code ec;
    fs::remove(hc::index_of(home) / ".xlings-index-cache.json", ec);
    return { std::move(home), markers / "plain-uninstall.marker", markers / "hookfail-uninstall.marker" };
}

fs::path plain_store(const tk::Home& home) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(home.dir() / "data" / "xpkgs", ec))
        if (e.path().filename().string().ends_with("-x-plain")) return e.path();
    return {};
}

bool db_has(const tk::Home& home, std::string_view name, std::string_view version) {
    return hc::registered(home, name, version);
}

}  // namespace

XTEST(RemoveForce, AFailingUninstallHookWithdrawsStateAndForceExitsZero, .area = "xim",
      .covers = {"REMOVE-FORCE"}, .requires_ = {"xlings-bin"}) {
    auto r = make_home("remove-force-hook");
    auto& home = r.home;

    // S1: the hook throws, no --force: exit 1, state already withdrawn.
    ASSERT_EQ(home.xlings({"install", "hookfail@1.0.0", "-y"}).exit_code, 0);
    std::error_code ec;
    fs::remove(r.hookfail_marker, ec);
    const auto s1 = home.xlings({"remove", "hookfail", "-y"});
    EXPECT_EQ(s1.exit_code, 1) << s1.transcript();
    EXPECT_TRUE(fs::exists(r.hookfail_marker)) << "uninstall() never ran\n" << s1.transcript();
    EXPECT_TRUE(hc::has(s1.transcript(), "boom")) << s1.transcript();
    hc::expect_gone(home, "hookfail", "1.0.0", "S1");

    // S2: the same hook, --force: exit 0.
    ASSERT_EQ(home.xlings({"install", "hookfail@1.0.0", "-y"}).exit_code, 0);
    fs::remove(r.hookfail_marker, ec);
    const auto s2 = home.xlings({"remove", "hookfail", "--force", "-y"});
    EXPECT_EQ(s2.exit_code, 0) << s2.transcript();
    EXPECT_TRUE(fs::exists(r.hookfail_marker)) << "uninstall() never ran\n" << s2.transcript();
    hc::expect_gone(home, "hookfail", "1.0.0", "S2");
}

XTEST(RemoveForce, APayloadDeletedByHandOrARecordWithoutABindingIsRemoved, .area = "xim",
      .covers = {"REMOVE-FORCE"}, .requires_ = {"xlings-bin"}) {
    auto r = make_home("remove-force-payload");
    auto& home = r.home;
    std::error_code ec;

    // S4: the payload was deleted by hand.
    ASSERT_EQ(home.xlings({"install", "plain@1.0.0", "-y"}).exit_code, 0);
    ASSERT_FALSE(plain_store(home).empty());
    fs::remove_all(plain_store(home) / "1.0.0", ec);
    const auto s4 = home.xlings({"remove", "plain", "-y"});
    EXPECT_EQ(s4.exit_code, 0) << s4.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S4");

    // S4b: the same, by its full coordinate with --force: a real removal,
    // not "is not installed".
    ASSERT_EQ(home.xlings({"install", "plain@1.0.0", "-y"}).exit_code, 0);
    fs::remove_all(plain_store(home) / "1.0.0", ec);
    const auto s4b = home.xlings({"remove", "xim:plain@1.0.0", "--force", "-y"});
    EXPECT_EQ(s4b.exit_code, 0) << s4b.transcript();
    EXPECT_TRUE(hc::has(s4b.transcript(), "removed")) << s4b.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S4b");

    // S5 (#578): a record of the versions database with no binding in the workspace.
    ASSERT_EQ(home.xlings({"install", "plain@1.0.0", "-y"}).exit_code, 0);
    hc::edit_workspace(home, "default", [](Json& ws) { ws.erase("plain"); });
    const auto s5 = home.xlings({"remove", "plain", "-y"});
    EXPECT_EQ(s5.exit_code, 0) << s5.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S5");
}

XTEST(RemoveForce, AllSubosAndAllRemoveEverywhereAndEveryVersion, .area = "xim",
      .covers = {"REMOVE-FORCE"}, .requires_ = {"xlings-bin"}) {
    auto r = make_home("remove-force-all");
    auto& home = r.home;

    // S6: --all-subos.
    ASSERT_EQ(home.xlings({"subos", "new", "other"}).exit_code, 0);
    ASSERT_EQ(hc::in(home, "default", {"install", "plain@1.0.0", "-y"}).exit_code, 0);
    ASSERT_EQ(hc::in(home, "other", {"install", "plain@1.0.0", "-y"}).exit_code, 0);
    const auto s6 = home.xlings({"remove", "plain", "--all-subos", "-y"});
    EXPECT_EQ(s6.exit_code, 0) << s6.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S6");

    // S7: --all, every version.
    ASSERT_EQ(home.xlings({"install", "plain@1.0.0", "-y"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"install", "plain@2.0.0", "-y"}).exit_code, 0);
    const auto s7 = home.xlings({"remove", "plain", "--all", "-y"});
    EXPECT_EQ(s7.exit_code, 0) << s7.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S7 (1.0.0)");
    hc::expect_gone(home, "plain", "2.0.0", "S7 (2.0.0)");
}

// S8: an unreadable sibling subos might still use the payload: remove
// detaches only, names every unreadable one, and --force does not override it.
XTEST(RemoveForce, AnUnreadableSiblingKeepsThePayloadAndForceDoesNotOverrideIt, .area = "xim",
      .covers = {"REMOVE-FORCE"}, .requires_ = {"xlings-bin"}) {
    auto r = make_home("remove-force-unreadable");
    auto& home = r.home;
    ASSERT_EQ(home.xlings({"subos", "new", "other2"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "other3"}).exit_code, 0);
    for (const auto* subos : {"default", "other2", "other3"})
        ASSERT_EQ(hc::in(home, subos, {"install", "plain@1.0.0", "-y"}).exit_code, 0) << subos;
    const auto ws2 = home.dir() / "subos" / "other2" / ".xlings.json";
    const auto ws3 = home.dir() / "subos" / "other3" / ".xlings.json";
    const auto saved2 = tk::read_file(ws2), saved3 = tk::read_file(ws3);
    tk::write_file(ws2, "{garbage");
    tk::write_file(ws3, "{garbage");
    const auto store = plain_store(home);
    ASSERT_FALSE(store.empty());

    const auto a = hc::in(home, "default", {"remove", "plain", "-y"});
    EXPECT_EQ(a.exit_code, 0) << a.transcript();
    EXPECT_TRUE(fs::is_directory(store / "1.0.0")) << "an unreadable subos might still use it\n" << a.transcript();
    EXPECT_TRUE(hc::has(a.transcript(), "other2")) << a.transcript();
    EXPECT_TRUE(hc::has(a.transcript(), "other3")) << a.transcript();

    const auto b = hc::in(home, "default", {"remove", "plain", "--force", "-y"});
    EXPECT_EQ(b.exit_code, 0) << b.transcript();
    EXPECT_TRUE(fs::is_directory(store / "1.0.0")) << "--force deleted a payload another subos may need\n" << b.transcript();
    EXPECT_TRUE(hc::has(b.transcript(), "does not override")) << b.transcript();

    tk::write_file(ws2, saved2);
    tk::write_file(ws3, saved3);
    const auto c = hc::in(home, "other2", {"remove", "plain", "-y"});
    EXPECT_EQ(c.exit_code, 0) << c.transcript();
    EXPECT_TRUE(fs::is_directory(store / "1.0.0")) << "other3 still uses it\n" << c.transcript();
    const auto d = hc::in(home, "other3", {"remove", "plain", "-y"});
    EXPECT_EQ(d.exit_code, 0) << d.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S8d");
}

// S9: absent here, present in one other subos: without -y remove stays and
// offers --all-subos; with -y it reaches the subos that has it.
XTEST(RemoveForce, YesReachesTheOneSubosThatHasIt, .area = "xim",
      .covers = {"REMOVE-FORCE"}, .requires_ = {"xlings-bin"}) {
    auto r = make_home("remove-force-elsewhere");
    auto& home = r.home;
    ASSERT_EQ(home.xlings({"subos", "new", "other9"}).exit_code, 0);
    ASSERT_EQ(hc::in(home, "other9", {"install", "plain@1.0.0", "-y"}).exit_code, 0);

    const auto a = hc::in(home, "default", {"remove", "plain"});
    EXPECT_EQ(a.exit_code, 0) << a.transcript();
    EXPECT_TRUE(hc::has(a.transcript(), "other9")) << a.transcript();
    EXPECT_TRUE(hc::has(a.transcript(), "remove plain --all-subos")) << a.transcript();
    EXPECT_TRUE(db_has(home, "plain", "1.0.0")) << "nothing should have been removed";

    const auto b = hc::in(home, "default", {"remove", "plain", "-y"});
    EXPECT_EQ(b.exit_code, 0) << b.transcript();
    EXPECT_TRUE(hc::has(b.transcript(), "other9")) << b.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S9b");
}

// S3: the recipe has left the index: remove --force works from the record.
XTEST(RemoveForce, ARecipeGoneFromTheIndexIsRemovedFromItsRecord, .area = "xim",
      .covers = {"REMOVE-FORCE"}, .requires_ = {"xlings-bin"}) {
    auto r = make_home("remove-force-gone");
    auto& home = r.home;
    ASSERT_EQ(home.xlings({"install", "plain@1.0.0", "-y"}).exit_code, 0);
    std::error_code ec;
    fs::remove(hc::recipe_file(hc::index_of(home), "plain"), ec);
    fs::remove(hc::index_of(home) / ".xlings-index-cache.json", ec);
    const auto s3 = home.xlings({"remove", "plain", "--force", "-y"});
    EXPECT_EQ(s3.exit_code, 0) << s3.transcript();
    EXPECT_TRUE(hc::has(s3.transcript(), "no index provides")) << s3.transcript();
    hc::expect_gone(home, "plain", "1.0.0", "S3");
}
