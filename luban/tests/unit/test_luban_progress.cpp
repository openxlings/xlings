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
    // A sandbox: fetch, unpack, configure and verify -- no start (55 + 15 + 15 + 5 = 90).
    svc::Overall o;
    auto e = svc::phase("prepare", "begin", "making box");
    o.take(e);
    EXPECT_EQ(e.stage, "fetch");
    EXPECT_NEAR(e.overall, 0, 1e-9);
    EXPECT_EQ(e.stages, (std::vector<std::string>{"fetch", "unpack", "configure", "verify"}));
    EXPECT_EQ(e.stage_percent, -1) << "nothing measured yet: a moving bar";
    // Halfway through 400 MB: half of fetch's 55 of 90.
    nlohmann::json files = nlohmann::json::array({{{"totalBytes", 400e6}, {"downloadedBytes", 200e6}}});
    auto d = svc::Event{.kind = svc::Event::Kind::Data, .data_kind = "download_progress", .data = {{"files", files}}};
    o.take(d);
    EXPECT_EQ(d.stage, "fetch");
    EXPECT_NEAR(d.overall, 27.5 / 90, 1e-9);
    EXPECT_NEAR(d.stage_percent, 0.5, 1e-9);
    EXPECT_EQ(d.stage_detail, "200.0 / 400.0 MB");
    auto x = progress_("extract", 1.0);
    o.take(x);
    EXPECT_EQ(x.stage, "unpack");
    EXPECT_NEAR(x.overall, 70.0 / 90, 1e-9);
    auto c = progress_("configure", 0.5);
    c.text = "installed fontconfig";
    o.take(c);
    EXPECT_EQ(c.stage, "configure");
    EXPECT_NEAR(c.overall, 77.5 / 90, 1e-9);
    EXPECT_EQ(c.stage_detail, "installed fontconfig");
    // An install after the system is made (its desktop) is in the stage reached:
    // a download of its own does not take the bar back.
    auto again = progress_("download", 0.1);
    o.take(again);
    EXPECT_EQ(again.stage, "configure");
    EXPECT_GE(again.overall, c.overall);
    auto verify = svc::phase("verify", "done", "box is ready");
    o.take(verify);
    EXPECT_EQ(verify.stage, "verify");
    EXPECT_NEAR(verify.overall, 1.0, 1e-9);
}

XTEST(LubanProgress, AMachineStartsInAStageOfItsOwnAndTheOldCheckIsReadAsBefore, .area = "luban", .covers = {"LUBAN-PROGRESS"}) {
    svc::Overall o(true, true, false);
    auto first = svc::phase("prepare", "begin", "starting box");
    o.take(first);
    EXPECT_EQ(first.stages, (std::vector<std::string>{"fetch", "unpack", "configure", "start"}));
    // `check` of before, while it has not started: its preflight.
    auto check = svc::phase("check", "begin", "checking this host");
    o.take(check);
    EXPECT_EQ(check.stage, "start");
    auto boot = progress_("boot", 0.5);
    o.take(boot);
    EXPECT_EQ(boot.stage, "start");
    EXPECT_NEAR(boot.overall, (85 + 5) / 95.0, 1e-9);
    // A stage it was not said to have, met: it has it now.
    auto verify = svc::phase("verify", "done", "box is ready");
    o.take(verify);
    EXPECT_EQ(verify.stages.back(), "verify");
    EXPECT_NEAR(verify.overall, 1.0, 1e-9);
    // Few bytes to fetch (the rest already here): fetching is a small share.
    svc::Overall cached;
    nlohmann::json few = nlohmann::json::array({{{"totalBytes", 3e6}, {"downloadedBytes", 3e6}}});
    auto d = svc::Event{.kind = svc::Event::Kind::Data, .data_kind = "download_progress", .data = {{"files", few}}};
    cached.take(d);
    EXPECT_NEAR(d.overall, 55 * 0.15 / (55 * 0.15 + 35), 1e-9);
    EXPECT_EQ(svc::Overall::stage_of("check"), 4);
    EXPECT_EQ(svc::Overall::stage_of("preflight"), 3);
}
