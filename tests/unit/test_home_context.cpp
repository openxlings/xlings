// xlings.core.home (C5): which home, in which mode, at which layout.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.home;
import xlings.core.home_identity;
import xlings.libs.json;

namespace h = xlings::home;
namespace fs = std::filesystem;
namespace tk = xlings::testkit;

namespace {
fs::path fresh_home(std::string_view name) {
    auto d = fs::temp_directory_path() / std::format("xlings-home-ctx-{}-{}", name,
        std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(d);
    return d;
}
}  // namespace

XTEST(HomeContext, DeclaredModeAndLayoutComeFromTheMarker,
      .area = "home", .covers = {"HOME-CONTEXT", "HOME-MODE-DECLARED"}) {
    auto home = fresh_home("declared");
    tk::write_file(home / ".xlings-home",
                   R"({"schema":1,"id":"abc","mode":"system","layout":2,"future":{"k":1}})");
    auto ctx = h::describe(home, h::Source::Env);
    EXPECT_EQ(ctx.mode, h::Mode::System);
    EXPECT_TRUE(ctx.modeDeclared);
    EXPECT_EQ(ctx.layout, 2);
    EXPECT_EQ(ctx.id, "abc");
    EXPECT_TRUE(ctx.writable());
    fs::remove_all(home);
}

XTEST(HomeContext, AMarkerWithoutAModeIsInferredAndSaysSo,
      .area = "home", .covers = {"HOME-CONTEXT"}) {
    auto home = fresh_home("inferred");
    tk::write_file(home / ".xlings-home", R"({"schema":1,"id":"abc"})");
    auto env = h::describe(home, h::Source::Env);
    EXPECT_EQ(env.mode, h::Mode::Custom);
    EXPECT_FALSE(env.modeDeclared);
    EXPECT_EQ(env.layout, 1);
    EXPECT_EQ(h::describe(home, h::Source::SelfContained).mode, h::Mode::Portable);
    fs::remove_all(home);
}

XTEST(HomeContext, DeclareKeepsUnknownKeysAndOnlyRaisesTheLayout,
      .area = "home", .covers = {"HOME-KEEP-UNKNOWN-KEYS", "HOME-MODE-DECLARED"}) {
    auto home = fresh_home("declare");
    tk::write_file(home / ".xlings-home", R"({"schema":1,"id":"abc","layout":5,"future":true})");
    ASSERT_TRUE(h::declare(home, h::Mode::User, 2).has_value());
    auto j = nlohmann::json::parse(tk::read_file(home / ".xlings-home"));
    EXPECT_EQ(j["mode"], "user");
    EXPECT_EQ(j["layout"], 5);          // never lowered
    EXPECT_EQ(j["future"], true);       // kept
    EXPECT_EQ(j["id"], "abc");
    fs::remove_all(home);
}

XTEST(HomeContext, AHigherLayoutAllowsOnlyReadOnlyCommands,
      .area = "home", .covers = {"HOME-LAYOUT-READONLY"}) {
    using v = std::vector<std::string_view>;
    auto ro = [](v a) { return h::is_read_only_command(a); };
    EXPECT_TRUE(ro({"list"}));
    EXPECT_TRUE(ro({"--version"}));
    EXPECT_TRUE(ro({"subos", "list"}));
    EXPECT_TRUE(ro({"subos", "status", "x"}));
    EXPECT_TRUE(ro({"self", "doctor"}));
    EXPECT_FALSE(ro({"self", "doctor", "--fix"}));
    EXPECT_FALSE(ro({"install", "gcc"}));
    EXPECT_FALSE(ro({"subos", "new", "x"}));
    EXPECT_FALSE(ro({"self", "update"}));

    h::HomeContext newer{ .layout = h::kLayout + 1 };
    EXPECT_FALSE(newer.writable());
}

XTEST(HomeContext, ACommandDeclaresTheModeAndLayoutOnce,
      .area = "home", .cost = tk::Cost::Medium,
      .covers = {"HOME-MODE-DECLARED"}, .requires_ = {"xlings-bin"}) {
    // A home from before the marker: the first command adopts it (writes the
    // marker) and declares what it is. No network, no `self init`.
    auto home = tk::Home::isolated("home-ctx");
    fs::create_directories(home.dir() / "subos" / "default");
    auto r = home.xlings({"subos", "list"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    auto j = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings-home"));
    // Home::isolated is HOME=<root>, XLINGS_HOME=<root>/.xlings: a user's
    // default home, so "user" -- declared now, no longer inferred.
    EXPECT_EQ(j.value("mode", ""), "user");
    EXPECT_EQ(j.value("layout", 0), h::kLayout);
}

XTEST(HomeContext, ANewerLayoutIsReadNeverWritten,
      .area = "home", .cost = tk::Cost::Medium,
      .covers = {"HOME-LAYOUT-READONLY"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("home-newer");
    fs::create_directories(home.dir() / "subos" / "default");
    ASSERT_EQ(home.xlings({"subos", "list"}).exit_code, 0);
    auto marker = home.dir() / ".xlings-home";
    auto j = nlohmann::json::parse(tk::read_file(marker));
    j["layout"] = h::kLayout + 7;
    tk::write_file(marker, j.dump());

    auto write = home.xlings({"subos", "new", "box"});
    EXPECT_EQ(write.exit_code, 1) << write.transcript();
    EXPECT_NE(write.transcript().find("only reads it"), std::string::npos) << write.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos" / "box"));

    auto read = home.xlings({"subos", "list"});
    EXPECT_EQ(read.exit_code, 0) << read.transcript();
    // and the marker was not lowered by anyone
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(marker))["layout"], h::kLayout + 7);
}

XTEST(WriterRules, AnUnparseableDocumentIsAnErrorNotAnEmptyOne,
      .area = "home", .covers = {"HOME-KEEP-UNKNOWN-KEYS"}) {
    auto home = fresh_home("writer");
    auto missing = h::read_json_for_update(home / "absent.json");
    ASSERT_TRUE(missing.has_value());
    EXPECT_TRUE(missing->empty());

    tk::write_file(home / "kept.json", R"({"known":1,"unknown":{"x":true}})");
    auto kept = h::read_json_for_update(home / "kept.json");
    ASSERT_TRUE(kept.has_value());
    EXPECT_TRUE(kept->contains("unknown"));

    tk::write_file(home / "broken.json", R"({"workspace": )");
    EXPECT_FALSE(h::read_json_for_update(home / "broken.json").has_value());
    tk::write_file(home / "array.json", "[1,2]");
    EXPECT_FALSE(h::read_json_for_update(home / "array.json").has_value());
    fs::remove_all(home);
}

XTEST(WriterRules, SubosNewDoesNotBlankAnUnparseableManifest,
      .area = "home", .cost = tk::Cost::Medium,
      .covers = {"HOME-KEEP-UNKNOWN-KEYS"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("writer-subos");
    fs::create_directories(home.dir() / "subos" / "default");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto manifest = home.dir() / "subos" / "box" / ".xlings.json";
    const std::string broken = R"({"workspace": {"gcc": "16.1.0"}, )";
    tk::write_file(manifest, broken);

    // Adopting the existing directory again must not rebuild it from {}.
    auto r = home.xlings({"subos", "new", "box", "-y"});
    EXPECT_NE(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(tk::read_file(manifest), broken);
}

// ── Deployment S and M (design §4) ───────────────────────────────────

XTEST(Deployment, AnEntryIsASystemPackagesOnlyWhereRootInstallsThem,
      .area = "home", .covers = {"HOME-SYSTEM-MODE"}) {
    auto home = tk::Home::isolated("entry");
    EXPECT_FALSE(xlings::home::describe_entry(home.dir() / "bin" / "xlings", home.dir()).system)
        << "the home's own entry";
    EXPECT_FALSE(xlings::home::describe_entry(home.root() / "work" / "xlings", home.dir()).system)
        << "a binary of the user's, outside the home";
    if constexpr (tk::is_linux) {
        // Root's, under /usr: what a system package installs. (A container
        // can map /usr to another owner; then the premise does not hold.)
        auto owner = tk::run({.argv = {"/usr/bin/stat", "-c", "%u", "/usr/bin/env"}});
        if (owner.exit_code == 0 && owner.out.starts_with("0\n"))
            EXPECT_TRUE(xlings::home::describe_entry("/usr/bin/env", home.dir()).system);
    }
}

XTEST(Deployment, TheSystemConfigIsADefaultTheHomeOverrides,
      .area = "home", .cost = tk::Cost::Medium, .covers = {"HOME-SYSTEM-MODE"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("sysconfig");
    const auto sys = home.root() / "etc" / "config.json";
    tk::write_file(sys, R"({"lang":"zh","mirror":"CN"})");
    auto layer = home.root() / "layer";
    tk::write_file(layer / ".xlings-home", R"({"layout":2,"mode":"multi"})");
    const std::map<std::string, std::string> env{
        {"XLINGS_SYSTEM_CONFIG", sys.string()}, {"XLINGS_SYSTEM_LAYER", layer.string()},
        {"XLINGS_TEST_MIRROR", ""}, {"XLINGS_RELEASE_MIRROR", ""}};
    // Fix this fixture's home mirror independently of the lane's CN override.
    tk::write_file(home.dir() / ".xlings.json", R"({"mirror":"GLOBAL"})");
    auto r = home.xlings({"config"}, env);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("zh"), std::string::npos) << "the system's language\n" << r.out;
    EXPECT_EQ(r.out.find("CN"), std::string::npos) << "the home's mirror wins\n" << r.out;

    auto doctor = home.xlings({"self", "doctor"}, env);
    EXPECT_NE(doctor.transcript().find("system config: " + sys.string()), std::string::npos)
        << doctor.transcript();
    EXPECT_NE(doctor.transcript().find("system layer: " + fs::canonical(layer).string()), std::string::npos)
        << doctor.transcript();
}

// #640 F14: the AUR package made its shared config world-writable (766).
XTEST(Deployment, ThePackageRecipesInstallNothingWorldWritable,
      .area = "home", .covers = {"F14"}) {
    const auto recipe = fs::current_path() / "config" / "aur" / "PKGBUILD";
    if (!fs::exists(recipe)) GTEST_SKIP() << "not run from the repository root";
    const auto text = tk::read_file(recipe);
    // chmod with an octal mode whose last digit grants others write (2, 3, 6, 7).
    const std::regex world_writable(R"(chmod\s+(-\w+\s+)*0?[0-7]?[0-7][0-7][2367]\b|chmod\s+(-\w+\s+)*[ao]?\+w|-m\s*0?[0-7]?[0-7][0-7][2367]\b)");
    std::smatch m;
    EXPECT_FALSE(std::regex_search(text, m, world_writable)) << m.str();
}
