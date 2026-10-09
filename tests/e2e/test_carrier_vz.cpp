// The vz carrier's lifecycle (SubOS design part 3 §5.4) against a stand-in for
// its helper (tests/fixtures/carrier/fake-vz-helper.sh): the same contract the
// real xlings-vm answers on a Mac, run here on any POSIX CI host.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.carrier;
import xlings.carrier.vz;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {
fs::path fake_helper() { return fs::path(__FILE__).parent_path().parent_path() / "fixtures/carrier/fake-vz-helper.sh"; }
std::string read_or_empty(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
}  // namespace

XTEST(CarrierVz, WithoutItsHelperTheCarrierIsRefusedWithTheRoute,
      .area = "subos", .covers = {"CARRIER-UNAVAILABLE"}) {
    const auto* vz = xlings::carrier::find("vz");
    ASSERT_NE(vz, nullptr);
    auto home = tk::Home::isolated("carrier-vz-none");
    const auto p = vz->probe({home.dir()});
    EXPECT_FALSE(p.supported);
    EXPECT_FALSE(p.route.empty());
}

XTEST(CarrierVz, AVmPerHomeIsCreatedOnceStartedWhenStoppedAndReachedThroughTheHelper,
      .area = "subos", .covers = {"CARRIER-VZ-LIFECYCLE"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("carrier-vz");
    const auto vms = home.root() / "vz";
    fs::create_directories(vms);
    const std::map<std::string, std::string> env{
        {"XLINGS_VZ_HELPER", fake_helper().string()}, {"FAKE_VZ_ROOT", vms.string()},
        {"XLINGS_CARRIER_GUEST_XLINGS", tk::xlings_binary().string()}};
    auto made = home.xlings({"subos", "new", "box", "--carrier", "vz"}, env);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto vm = vms / "vms" / xlings::carrier::vz::vm_name(home.dir());
    EXPECT_TRUE(fs::is_directory(vm / "xlings/subos/box")) << made.transcript();
    EXPECT_FALSE(fs::exists(vm / "etc/wsl.conf")) << "a VM is not a WSL distribution";

    auto info = home.xlings({"subos", "info", "box"}, env);
    EXPECT_EQ(info.exit_code, 0) << info.transcript();
    // Stopped (an idle VM), then used again: started, not created again.
    fs::remove(vm / "running");
    auto again = home.xlings({"subos", "info", "box"}, env);
    EXPECT_EQ(again.exit_code, 0) << again.transcript();
    const auto calls = read_or_empty(vms / "calls.log");
    std::size_t creates = 0, starts = 0;
    for (std::size_t at = 0; (at = calls.find("create ", at)) != std::string::npos; ++at) ++creates;
    for (std::size_t at = 0; (at = calls.find("start ", at)) != std::string::npos; ++at) ++starts;
    EXPECT_EQ(creates, 1u) << calls;
    EXPECT_EQ(starts, 2u) << calls;

    // A grant through the helper: the path inside is what it answers.
    const auto* vz = xlings::carrier::find("vz");
    for (const auto& [k, v] : env) xlings::testkit::set_env(k, v);
    auto at = vz->ensure({home.dir()});
    ASSERT_TRUE(at) << at.error();
    auto granted = vz->grant(*at, home.root() / "project", true, "project");
    ASSERT_TRUE(granted) << granted.error();
    EXPECT_EQ(*granted, "/grant/project");
    EXPECT_NE(read_or_empty(vm / "shares").find("project rw"), std::string::npos);

    auto removed = home.xlings({"subos", "remove", "box", "-y"}, env);
    EXPECT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(vm / "xlings/subos/box"));
}
