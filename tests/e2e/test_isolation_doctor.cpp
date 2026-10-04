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

XTEST(IsolationDoctor, ProbesAreCachedPerBootAndTraced,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"ISO-CAPS-CACHE", "OBS-TRACE"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    auto home = tk::Home::isolated("caps-cache");
    home.seed_sandbox_backend();
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto first = home.xlings({"subos", "exec", "box", "--sandbox", "--", "true"}, {{"XLINGS_TRACE", "caps"}});
    auto second = home.xlings({"subos", "exec", "box", "--sandbox", "--", "true"}, {{"XLINGS_TRACE", "caps,spec"}});
    EXPECT_NE(first.err.find("[trace:caps]"), std::string::npos) << first.transcript();
    EXPECT_NE(first.err.find("probed"), std::string::npos) << first.transcript();
    EXPECT_NE(second.err.find("cached"), std::string::npos) << second.transcript();
    EXPECT_EQ(second.err.find("probed"), std::string::npos) << second.transcript();
    EXPECT_NE(second.err.find("[trace:spec]"), std::string::npos);
    EXPECT_TRUE(fs::exists(home.dir() / "state" / "isolation-caps.json"));
}

XTEST(IsolationDoctor, APlatformWithoutTheIsolationSaysSoInOneFormat,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PLAT-HINT"},
      .requires_ = {"xlings-bin"}) {
    if (tk::probe("linux")) {   // a reason it is not Linux: macOS or Windows
        auto home = tk::Home::isolated("plat-hint");
        ASSERT_EQ(home.xlings({"self", "init"}, {}, std::chrono::minutes(3)).exit_code, 0);
        ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
        auto r = home.xlings({"subos", "exec", "box", "--sandbox=private", "--", "true"});
        EXPECT_EQ(r.exit_code, 125) << r.transcript();
        EXPECT_NE(r.transcript().find("not implemented on this platform yet"), std::string::npos)
            << r.transcript();
        return;
    }
    // Linux: the same shared format, for a requirement this host cannot meet.
    auto home = tk::Home::isolated("plat-hint");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    tk::write_file(home.dir() / "data" / "xpkgs" / "xim-x-proot" / "0" / "bin" / "proot", "#!/bin/sh\nexit 0\n");
    fs::permissions(home.dir() / "data" / "xpkgs" / "xim-x-proot" / "0" / "bin" / "proot", fs::perms::owner_all);
    auto r = home.xlings({"subos", "exec", "box", "--sandbox", "proot", "--sandbox=locked", "--", "true"});
    EXPECT_EQ(r.exit_code, 125) << r.transcript();
    EXPECT_NE(r.transcript().find("cannot enter"), std::string::npos) << r.transcript();
}
