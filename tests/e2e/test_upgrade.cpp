// The upgrade a user does not notice, and the failure that stays small.
//
// docs/specs/upgrade-and-compatibility.md: the first command of a new client
// that changes the home (and succeeds) runs its upgrade steps by itself and
// records them; what it cannot do
// is said once, with its command; an older client says how to go back. And a
// broken piece of state is one failure domain: every command still runs, the
// broken piece is named with its fix, and nothing else is lost.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

tk::Home initialised(std::string_view name) {
    auto home = tk::Home::isolated(name);
    auto made = home.xlings({"self", "init"});
    EXPECT_EQ(made.exit_code, 0) << made.transcript();
    return home;
}

Json config_of(const tk::Home& home) {
    return Json::parse(tk::read_file(home.dir() / ".xlings.json"));
}

void set_upgraded_to(const tk::Home& home, std::string_view version) {
    auto doc = config_of(home);
    doc["upgrade"] = {{"to", version}};
    tk::write_file(home.dir() / ".xlings.json", doc.dump());
}

}  // namespace

XTEST(Upgrade, TheFirstCommandOfANewClientUpgradesTheHomeByItselfAndSaysNothing, .area = "self",
      .covers = {"UPGRADE-LADDER"}, .requires_ = {"xlings-bin"}) {
    auto home = initialised("upgrade-silent");
    set_upgraded_to(home, "2026.1.1.1");
    // A read-only command leaves the home as it is, the upgrade record too.
    auto looked = home.xlings({"list"});
    EXPECT_EQ(looked.exit_code, 0) << looked.transcript();
    EXPECT_EQ(config_of(home)["upgrade"].value("to", ""), "2026.1.1.1");
    // The first one that changes it upgrades it, after it succeeded.
    auto first = home.xlings({"subos", "new", "probe"});
    EXPECT_EQ(first.exit_code, 0) << first.transcript();
    EXPECT_EQ(first.transcript().find("could not take by itself"), std::string::npos) << first.transcript();
    EXPECT_EQ(first.transcript().find("self.upgrade"), std::string::npos) << first.transcript();
    const auto record = config_of(home)["upgrade"];
    EXPECT_EQ(record.value("from", ""), "2026.1.1.1") << record.dump();
    EXPECT_NE(record.value("to", ""), "2026.1.1.1") << record.dump();
    EXPECT_TRUE(record["pending"].empty()) << record.dump();
    EXPECT_NE(tk::read_file(home.dir() / "logs" / "upgrade.ndjson").find("2026.1.1.1"), std::string::npos);
}

XTEST(Upgrade, WhatItCannotDoByItselfIsSaidOnceWithItsCommand, .area = "self",
      .covers = {"UPGRADE-PENDING-ONCE"}, .requires_ = {"xlings-bin"}) {
    auto home = initialised("upgrade-pending");
    ASSERT_EQ(home.xlings({"subos", "new", "other"}).exit_code, 0);
    tk::write_file(home.dir() / "subos" / "other" / ".xlings.json", "{not json");
    set_upgraded_to(home, "2026.1.1.1");
    auto first = home.xlings({"subos", "new", "probe"});
    const auto out = first.transcript();
    EXPECT_EQ(first.exit_code, 0) << "the command asked for still ran\n" << out;
    EXPECT_NE(out.find("need a step it could not take by itself"), std::string::npos) << out;
    EXPECT_NE(out.find("subos 'other'"), std::string::npos) << out;
    EXPECT_NE(out.find("xlings self doctor --fix"), std::string::npos) << out;
    auto second = home.xlings({"subos", "new", "probe2"});
    EXPECT_EQ(second.transcript().find("could not take by itself"), std::string::npos)
        << "said once\n" << second.transcript();
    EXPECT_EQ(config_of(home)["upgrade"]["pending"].size(), 1u);
}

XTEST(Upgrade, AnOlderClientChangesNothingAndSaysHowToGoBack, .area = "self",
      .covers = {"UPGRADE-DOWNGRADE"}, .requires_ = {"xlings-bin"}) {
    auto home = initialised("upgrade-downgrade");
    set_upgraded_to(home, "2099.1.1.1");
    auto first = home.xlings({"subos", "new", "probe"});
    EXPECT_EQ(first.exit_code, 0) << first.transcript();
    EXPECT_NE(first.transcript().find("last upgraded by xlings 2099.1.1.1"), std::string::npos)
        << first.transcript();
    EXPECT_NE(first.transcript().find("xlings self update"), std::string::npos) << first.transcript();
    EXPECT_EQ(config_of(home)["upgrade"].value("to", ""), "2099.1.1.1") << "the record is the newer one's";
}

// One broken file, one failure domain: every command still runs and names
// it with the fix; the fix keeps the file and rebuilds what can be derived.
XTEST(FaultIsolation, ABrokenHomeConfigIsNamedByEveryCommandAndRebuiltByTheFix, .area = "self",
      .covers = {"FAULT-HOME-CONFIG"}, .requires_ = {"xlings-bin"}) {
    auto home = initialised("fault-home-config");
    ASSERT_EQ(home.xlings({"subos", "new", "other"}).exit_code, 0);
    tk::write_file(home.dir() / ".xlings.json", "{\"mirror\": \"GLOBAL\", oops");
    for (auto args : {std::vector<std::string>{"--version"}, {"list"}, {"config"}, {"subos", "list"}}) {
        auto r = home.xlings(args);
        EXPECT_EQ(r.exit_code, 0) << args[0] << "\n" << r.transcript();
        if (args[0] != "--version") {
            EXPECT_NE(r.transcript().find(".xlings.json is not readable JSON"), std::string::npos)
                << args[0] << "\n" << r.transcript();
            EXPECT_NE(r.transcript().find("xlings self doctor --fix"), std::string::npos) << r.transcript();
        }
    }
    auto doctor = home.xlings({"self", "doctor"});
    EXPECT_NE(doctor.exit_code, 0) << doctor.transcript();
    EXPECT_NE(doctor.transcript().find("home config"), std::string::npos) << doctor.transcript();

    auto fixed = home.xlings({"self", "doctor", "--fix"});
    EXPECT_NE(fixed.transcript().find("rebuilt"), std::string::npos) << fixed.transcript();
    bool kept = false;
    for (const auto& e : fs::directory_iterator(home.dir()))
        kept = kept || e.path().filename().string().starts_with(".xlings.json.corrupt-");
    EXPECT_TRUE(kept) << "the broken file is kept beside the new one";
    const auto doc = config_of(home);
    EXPECT_TRUE(doc["subos"].contains("other")) << doc.dump();
    auto after = home.xlings({"list"});
    EXPECT_EQ(after.transcript().find("not readable JSON"), std::string::npos) << after.transcript();
}

XTEST(FaultIsolation, AFieldOfTheWrongShapeIsOneFindingNotAnEmptyHome, .area = "self",
      .covers = {"FAULT-HOME-CONFIG"}, .requires_ = {"xlings-bin"}) {
    auto home = initialised("fault-home-field");
    auto doc = config_of(home);
    doc["subos"] = Json::array({"default"});
    tk::write_file(home.dir() / ".xlings.json", doc.dump());
    auto r = home.xlings({"list"});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.transcript().find("`subos` is an array, not an object"), std::string::npos) << r.transcript();
    (void)home.xlings({"self", "doctor", "--fix"});
    EXPECT_TRUE(config_of(home)["subos"].is_object()) << config_of(home).dump();
    EXPECT_EQ(config_of(home).value("mirror", ""), doc.value("mirror", "")) << "the rest of the file stays";
}

// A SubOS whose state cannot be read is that SubOS's problem: the others,
// and the commands that do not need it, carry on.
XTEST(FaultIsolation, AnUnreadableSubosStopsNothingElse, .area = "subos",
      .covers = {"FAULT-ISOLATION"}, .requires_ = {"xlings-bin"}) {
    auto home = initialised("fault-subos");
    ASSERT_EQ(home.xlings({"subos", "new", "other"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "third"}).exit_code, 0);
    tk::write_file(home.dir() / "subos" / "other" / ".xlings.json", "{not json");
    for (auto args : {std::vector<std::string>{"list"}, {"list", "--all"}, {"subos", "list"},
                      {"subos", "info", "third"}, {"config"}}) {
        auto r = home.xlings(args);
        EXPECT_EQ(r.exit_code, 0) << args[0] << "\n" << r.transcript();
    }
    auto doctor = home.xlings({"self", "doctor"});
    EXPECT_NE(doctor.transcript().find("subos 'other'"), std::string::npos) << doctor.transcript();
}
