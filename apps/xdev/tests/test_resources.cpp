#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.platform;
import xlings.xdev.resources;
import xlings.xdev.selection;

namespace tk = xlings::testkit;
namespace resource = xlings::xdev::resources;
namespace sel = xlings::xdev::selection;

XTEST(XdevResources, RejectsUnsafeNamesAndMakesCpuRequestsExclusive,
      .area = "testkit", .covers = {"CI-RESOURCE-LOCKS"}) {
    for (const auto* bad : {"", "../escape", "port:0", "port:65536", "port:no", "cpu:0", "cpu:no", "bad key"})
        EXPECT_FALSE(resource::normalize(bad)) << bad;
    EXPECT_EQ(*resource::normalize("cpu:8"), "cpu");
    EXPECT_EQ(*resource::normalize("cpu:all"), "cpu");
    EXPECT_EQ(*resource::normalize("port:08080"), "port:8080");
    EXPECT_EQ(*resource::normalize("opaque-resource:v1"), "opaque-resource:v1");
    const std::vector<sel::Test> tests{{.id = "unit/mixed", .source = "tests/unit/mixed.cpp",
        .cases = {{.name = "Cases.Fast", .resources = {"cpu:2", "port:8080"}},
                  {.name = "Cases.Network", .requires_ = {"network"}, .resources = {"port:9000"}}},
        .metadata_known = true}};
    const auto selected = sel::select(tests, {.lane = "pr"});
    ASSERT_TRUE(selected);
    ASSERT_EQ(selected->size(), 1);
    EXPECT_EQ(selected->front().resources, (std::vector<std::string>{"cpu", "port:8080"}));
}

XTEST(XdevResources, SharedKeysSerializeWhileIndependentKeysRunInParallel,
      .area = "testkit", .covers = {"CI-RESOURCE-LOCKS"}) {
    auto home = tk::Home::isolated("resource-scheduler");
    std::atomic<int> active { 0 }, maximum { 0 }, completed { 0 };
    std::vector<resource::Task> tasks;
    for (int index = 0; index < 4; ++index) {
        tasks.push_back({.id = std::to_string(index), .resources = {index % 2 ? "cpu:4" : "cpu:1", "port:8080"},
            .run = [&] {
                const int count = ++active;
                int prior = maximum.load();
                while (prior < count && !maximum.compare_exchange_weak(prior, count)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                --active;
                ++completed;
                return 0;
            }});
    }
    const auto serial = resource::run(tasks, 4, home.root() / "locks");
    ASSERT_TRUE(serial);
    EXPECT_EQ(maximum.load(), 1);
    EXPECT_EQ(completed.load(), 4);
    for (const auto& result : *serial) EXPECT_EQ(result.exit_code, 0) << result.error;
    std::atomic<int> entered { 0 };
    auto both = [&] {
        ++entered;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (entered.load() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return entered.load() == 2 ? 0 : 42;
    };
    const std::vector<resource::Task> independent{
        {.id = "a", .resources = {"port:8081"}, .run = both},
        {.id = "b", .resources = {"port:8082"}, .run = both}};
    const auto parallel = resource::run(independent, 2, home.root() / "locks");
    ASSERT_TRUE(parallel);
    for (const auto& result : *parallel) EXPECT_EQ(result.exit_code, 0) << result.error;
}

XTEST(XdevResources, LeaseExcludesAnotherProcessAndReleasesForItsNextAttempt,
      .area = "testkit", .covers = {"CI-RESOURCE-LOCKS"}) {
    const std::vector<std::string> names{"port:8080", "cpu:4", "cpu:1"};
    if (const auto* directory = std::getenv("XDEV_LOCK_CHILD"); directory && *directory) {
        const auto lease = resource::Lease::acquire(directory, names, std::chrono::milliseconds(80));
        const bool expected = std::string_view(std::getenv("XDEV_LOCK_EXPECT")) == "available";
        EXPECT_EQ(lease.has_value(), expected);
        return;
    }
    auto home = tk::Home::isolated("cross-process-resource-lock");
    const auto directory = home.root() / "locks";
    auto child = [&](std::string expectation) {
        auto env = tk::inherited_env();
        env["XDEV_LOCK_CHILD"] = directory.string();
        env["XDEV_LOCK_EXPECT"] = expectation;
        env.erase("XTEST_META_OUT");
        env.erase("XTEST_RESULTS_OUT");
        return tk::run({.argv = {xlings::platform::get_executable_path().string(),
            "--gtest_filter=XdevResources.LeaseExcludesAnotherProcessAndReleasesForItsNextAttempt"},
            .env = std::move(env), .timeout = std::chrono::seconds(10)});
    };
    {
        auto lease = resource::Lease::acquire(directory, names, std::chrono::seconds(1));
        ASSERT_TRUE(lease) << lease.error();
        const auto blocked = child("blocked");
        EXPECT_EQ(blocked.exit_code, 0) << blocked.transcript();
    }
    const auto released = child("available");
    EXPECT_EQ(released.exit_code, 0) << released.transcript();
    EXPECT_TRUE(std::filesystem::is_directory(directory)) << "persistent lock files are not removed";
}
