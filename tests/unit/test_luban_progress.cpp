// One progress for making a system (design 2026-10-20 G22, G23): stages with
// their shares, a stage's progress filling its share, a machine starting a
// stage of its own, and the whole never going back.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import luban.service;

namespace svc = luban::service;

namespace {
svc::Event progress_(std::string phase, double percent) {
    return svc::Event{.kind = svc::Event::Kind::Progress, .phase = std::move(phase), .percent = percent};
}
}  // namespace

XTEST(LubanProgress, TheWholeOperationIsOneProgressThatNeverGoesBack, .area = "luban", .covers = {"LUBAN-PROGRESS"}) {
    svc::Overall o;
    auto e = svc::phase("prepare", "begin", "making box");
    o.take(e);
    EXPECT_EQ(e.stage, "prepare");
    EXPECT_NEAR(e.overall, 0, 1e-9);
    // Halfway through the download: 2 % + half of 55 %.
    nlohmann::json files = nlohmann::json::array({{{"totalBytes", 100.0}, {"downloadedBytes", 50.0}}});
    auto d = svc::Event{.kind = svc::Event::Kind::Data, .data_kind = "download_progress", .data = {{"files", files}}};
    o.take(d);
    EXPECT_EQ(d.stage, "download");
    EXPECT_NEAR(d.overall, 0.295, 1e-9);
    auto x = progress_("extract", 1.0);
    o.take(x);
    EXPECT_NEAR(x.overall, 0.75, 1e-9);
    auto c = progress_("configure", 0.5);
    o.take(c);
    EXPECT_NEAR(c.overall, 0.825, 1e-9);
    // An install after the system is made (its desktop) is in the stage reached:
    // a download of its own does not take the bar back.
    auto again = progress_("download", 0.1);
    o.take(again);
    EXPECT_EQ(again.stage, "configure");
    EXPECT_GE(again.overall, c.overall);
    // The machine starting is a stage of its own, before the check (it used to read "prepare", 0 %).
    auto boot = progress_("boot", 0.5);
    o.take(boot);
    EXPECT_EQ(boot.stage, "start");
    EXPECT_NEAR(boot.overall, 0.935, 1e-9);
    auto check = svc::phase("check", "done", "box is ready");
    o.take(check);
    EXPECT_EQ(check.stage, "check");
    EXPECT_NEAR(check.overall, 1.0, 1e-9);
}
