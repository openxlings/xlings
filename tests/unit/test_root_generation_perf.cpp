#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.subos.rootfs;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace rf = xlings::subos::rootfs;

XTEST(RootGenerationPerf, ThreeHundredPayloadsMeetBuildAndSwitchBudgets,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"PERF-GEN-BUILD", "PERF-GEN-SWITCH"}, .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("generation-perf");
    const auto scope = home.dir() / "subos/box";
    rf::Inputs inputs;
    for (int i = 0; i != 300; ++i) {
        const auto payload = home.dir() / "data/xpkgs" / std::format("fixture-x-tool-{}/1", i);
        const auto name = std::format("tool-{}", i);
        tk::write_file(payload / "bin" / name, "#!/bin/sh\nexit 0\n");
        fs::permissions(payload / "bin" / name, fs::perms::owner_all);
        tk::write_file(payload / "lib" / std::format("libtool-{}.so", i), "fixture library\n");
        inputs.payloads.push_back(payload);
        inputs.programs.push_back({name, payload / "bin" / name});
    }
    std::vector<long long> builds, switches;
    std::vector<int> generations;
    for (int sample = 0; sample != 3; ++sample) {
        inputs.programs.push_back({std::format("sample-{}", sample), inputs.programs.front().target});
        const auto started = std::chrono::steady_clock::now();
        const auto plan = rf::plan(inputs);
        ASSERT_GE(plan.links.size(), 600u) << "the full payload projection must be measured";
        const auto generation = rf::commit(scope, plan, "performance sample");
        ASSERT_TRUE(generation) << generation.error();
        builds.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
        generations.push_back(*generation);
        EXPECT_EQ(fs::read_symlink(rf::usr_of(scope) / "bin/tool-299"), inputs.programs[299].target);
        EXPECT_EQ(fs::read_symlink(rf::usr_of(scope) / "lib/libtool-299.so"),
                  inputs.payloads[299] / "lib/libtool-299.so");
    }
    for (const int generation : generations) {
        const auto started = std::chrono::steady_clock::now();
        const auto switched = rf::switch_to(scope, generation);
        ASSERT_TRUE(switched) << switched.error();
        switches.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
        ASSERT_TRUE(rf::current(scope));
        EXPECT_EQ(*rf::current(scope), generation);
    }
    std::ranges::sort(builds);
    std::ranges::sort(switches);
    std::cout << "generation_300_payloads build_median_us=" << builds[1]
              << " switch_median_us=" << switches[1] << '\n';
    EXPECT_LE(builds[1], 1'000'000);
    EXPECT_LE(switches[1], 10'000);
}
