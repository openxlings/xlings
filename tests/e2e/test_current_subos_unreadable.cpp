// The current subos's own .xlings.json unreadable: commands run against it
// survive, doctor names it, nothing overwrites it, and another subos's file
// is not touched (E2E-109 of 2026.9.12, finding F10, once
// current_subos_unreadable_test.sh; #592). Config's writer of the active
// subos used to blank such a file on the first ordinary write.
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

XTEST(CurrentSubosUnreadable, CommandsSurviveItAndNothingOverwritesIt, .area = "subos",
      .covers = {"CURRENT-SUBOS-UNREADABLE"}, .requires_ = {"xlings-bin"}) {
    auto home = hc::seeded("current-subos-unreadable",
                           {{"csugood", hc::recipe({ .name = "csugood" })}, {"csusafe", hc::recipe({ .name = "csusafe" })}});
    ASSERT_EQ(hc::in(home, "default", {"subos", "new", "safe"}).exit_code, 0);
    ASSERT_EQ(hc::in(home, "default", {"install", "csugood@1.0.0", "-y"}).exit_code, 0);
    ASSERT_EQ(hc::in(home, "safe", {"install", "csusafe@1.0.0", "-y"}).exit_code, 0);
    const auto safe_ws = home.dir() / "subos" / "safe" / ".xlings.json";
    const auto default_ws = home.dir() / "subos" / "default" / ".xlings.json";
    ASSERT_TRUE(fs::exists(safe_ws));
    ASSERT_TRUE(fs::exists(default_ws));
    tk::write_file(default_ws, "{garbage");
    const auto safe_before = tk::read_file(safe_ws);

    const auto run = [&](std::string_view label, std::vector<std::string> args) {
        const auto r = hc::in(home, "default", std::move(args));
        hc::expect_survives(label, r);
        EXPECT_EQ(tk::read_file(safe_ws), safe_before) << label << ": the safe subos's file changed\n" << r.transcript();
        return r;
    };
    run("list", {"list"});
    run("install csugood", {"install", "csugood@1.0.0", "-y"});
    run("remove csugood", {"remove", "csugood", "-y"});
    const auto doctor = run("self doctor", {"self", "doctor"});
    EXPECT_TRUE(hc::has_nocase(doctor.transcript(), "unreadable")) << doctor.transcript();
    EXPECT_TRUE(hc::has(doctor.transcript(), "default")) << doctor.transcript();
    run("self doctor --fix --dry-run", {"self", "doctor", "--fix", "--dry-run", "-y"});
    EXPECT_EQ(tk::read_file(default_ws), "{garbage") << "the unreadable file was written over";
}
