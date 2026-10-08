#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.subos.rootfs;

#if defined(__SANITIZE_ADDRESS__)
#define XLINGS_GENERATION_INSTRUMENTED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XLINGS_GENERATION_INSTRUMENTED 1
#endif
#endif

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace rf = xlings::subos::rootfs;

XTEST(RootGenerationPerf, ThreeHundredPayloadsMeetBuildAndSwitchBudgets, .area = "subos",
      .cost = tk::Cost::Medium, .covers = {"PERF-GEN-BUILD", "PERF-GEN-SWITCH"},
      .requires_ = {"linux"}) {
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
        inputs.programs.push_back(
            {std::format("sample-{}", sample), inputs.programs.front().target});
        const auto started = std::chrono::steady_clock::now();
        const auto plan = rf::plan(inputs);
        ASSERT_GE(plan.links.size(), 600u) << "the full payload projection must be measured";
        const auto generation = rf::commit(scope, plan, "performance sample");
        ASSERT_TRUE(generation) << generation.error();
        builds.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count());
        generations.push_back(*generation);
        EXPECT_EQ(fs::read_symlink(rf::usr_of(scope) / "bin/tool-299"),
                  inputs.programs[299].target);
        EXPECT_EQ(fs::read_symlink(rf::usr_of(scope) / "lib/libtool-299.so"),
                  inputs.payloads[299] / "lib/libtool-299.so");
    }
    for (const int generation : generations) {
        const auto started = std::chrono::steady_clock::now();
        const auto switched = rf::switch_to(scope, generation);
        ASSERT_TRUE(switched) << switched.error();
        switches.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count());
        ASSERT_TRUE(rf::current(scope));
        EXPECT_EQ(*rf::current(scope), generation);
    }
    std::ranges::sort(builds);
    std::ranges::sort(switches);
    std::cout << "generation_300_payloads build_median_us=" << builds[1]
              << " checked_switch_median_us=" << switches[1] << '\n';
#if defined(XLINGS_GENERATION_INSTRUMENTED)
    GTEST_SKIP() << "instrumented workload validated; runtime timing budgets require the mandatory "
                   "static performance lane";
#endif
    EXPECT_LE(builds[1], 1'000'000);
    EXPECT_LE(switches[1], 10'000);
}

XTEST(RootGenerationPerf, OptimizedInventoryStillRefusesUnknownAndReplacedEntries, .area = "subos",
      .covers = {"ROOT-GEN-ATOMIC", "ROOT-ROLLBACK"}, .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("generation-inventory");
    const auto scope = home.dir() / "subos/box";
    const auto source = home.dir() / "data/xpkgs/fixture-x-tool/1/bin/tool";
    tk::write_file(source, "fixture executable");
    const rf::Plan plan{{{"usr/bin/tool", source, "fixture"}}, {}};
    const auto first = rf::commit(scope, plan, "first");
    ASSERT_TRUE(first) << first.error();
    auto changed = plan;
    changed.links.push_back({"usr/bin/other", source, "fixture"});
    const auto second = rf::commit(scope, changed, "second");
    ASSERT_TRUE(second) << second.error();
    const auto firstDir = scope / "root.gen" / std::to_string(*first);
    const auto unexpected = firstDir / "usr/user-directory/personal.txt";
    tk::write_file(unexpected, "user data");
    EXPECT_FALSE(rf::switch_to(scope, *first));
    EXPECT_FALSE(rf::validate_generation(scope, *first));
    EXPECT_EQ(rf::current(scope), std::optional<int>(*second));
    EXPECT_EQ(tk::read_file(unexpected), "user data");
    fs::remove(unexpected);
    fs::remove(unexpected.parent_path());
    const auto recorded = firstDir / "usr/bin/tool";
    fs::remove(recorded);
    tk::write_file(recorded, "user replacement");
    EXPECT_FALSE(rf::switch_to(scope, *first));
    EXPECT_FALSE(rf::validate_generation(scope, *first));
    EXPECT_EQ(rf::current(scope), std::optional<int>(*second));
    EXPECT_EQ(tk::read_file(recorded), "user replacement");
}
