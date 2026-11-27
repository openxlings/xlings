// `self doctor` and `self doctor --deep` name the same remedy for the same
// finding (E2E-103 of 2026.9.12, once doctor_remedy_mode_parity_test.sh;
// #592). Before, a broken payload the index still provides read as "no
// package in any index provides this entry" outside --deep and --fix.
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

// The remedy lines ("→ run ...") of a doctor report, trimmed.
std::vector<std::string> remedies(const tk::RunResult& r) {
    std::vector<std::string> out;
    std::istringstream in(hc::plain(r.transcript()));
    for (std::string line; std::getline(in, line);) {
        if (line.find("→ run") == std::string::npos) continue;
        const auto b = line.find_first_not_of(" \t");
        out.push_back(line.substr(b == std::string::npos ? 0 : b));
    }
    return out;
}

}  // namespace

XTEST(DoctorRemedyParity, QuickAndDeepNameTheSameRemedy, .area = "self",
      .covers = {"DOCTOR-REMEDY-PARITY"}, .requires_ = {"xlings-bin"}) {
    hc::Program fixture{ .name = "remedy-fixture" };
    auto home = hc::seeded("doctor-remedy-parity", {{"remedy-fixture", hc::recipe(fixture)}});
    ASSERT_EQ(home.xlings({"install", "remedy-fixture@1.0.0", "-y"}).exit_code, 0);
    const auto bin = home.dir() / "data" / "xpkgs" / "xim-x-remedy-fixture" / "1.0.0" / "bin";
    ASSERT_TRUE(fs::is_directory(bin));
    fs::remove_all(bin);

    const auto quick = home.xlings({"self", "doctor"});
    EXPECT_NE(quick.exit_code, 0) << quick.transcript();
    const auto deep = home.xlings({"self", "doctor", "--deep"});
    EXPECT_NE(deep.exit_code, 0) << deep.transcript();

    const auto q = remedies(quick), d = remedies(deep);
    ASSERT_FALSE(q.empty()) << "quick doctor printed no remedy\n" << quick.transcript();
    EXPECT_EQ(q, d) << "---quick---\n" << quick.transcript() << "\n---deep---\n" << deep.transcript();
    const bool named = std::ranges::any_of(q, [](const std::string& l) {
        return l.find("xlings install remedy-fixture@1.0.0") != std::string::npos
            || l.find("xlings install xim:remedy-fixture@1.0.0") != std::string::npos;
    });
    EXPECT_TRUE(named) << quick.transcript();
    EXPECT_FALSE(hc::has_nocase(quick.transcript(), "no package in any index provides")) << quick.transcript();
}
