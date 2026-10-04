// `self doctor --isolation` (C21) and the backend lookup order (#640 F10).
// The root repair itself runs on a CI runner of its own
// (tests/e2e/isolation_doctor_fix_test.sh): it installs files as root.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(IsolationDoctor, ReportsEveryBackendItFoundAndWhatTheProbeSaid,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"DOC-ISOLATION", "F10"},
      .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("doctor-iso");
    home.seed_sandbox_backend();
    auto r = home.xlings({"self", "doctor", "--isolation", "--json"});
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << r.transcript();
    EXPECT_EQ(j["gates"].size(), 8u);
    EXPECT_TRUE(j["sysctl"].contains("kernel.apparmor_restrict_unprivileged_userns"));
    // Root-owned first, then the system's, then the payload -- each probed.
    std::vector<std::string> order;
    for (auto& b : j["bwrap"]) order.push_back(b["source"].get<std::string>());
    auto rank = [](const std::string& s) { return s == "root-owned" ? 0 : s == "system" ? 1 : 2; };
    EXPECT_TRUE(std::ranges::is_sorted(order, {}, rank)) << j["bwrap"].dump();
    if (j["ok"].get<bool>()) {
        // The backend is the first usable candidate, not "the payload because
        // it is ours".
        std::string first_usable;
        for (auto& b : j["bwrap"]) if (b["usable"].get<bool>()) { first_usable = b["path"]; break; }
        EXPECT_EQ(j["backend"]["path"], first_usable);
        EXPECT_EQ(r.exit_code, 0);
    } else {
        EXPECT_NE(r.exit_code, 0);
    }
    // No setuid anywhere in what it chose or offered.
    EXPECT_EQ(r.out.find("chmod 4755"), std::string::npos);
}
