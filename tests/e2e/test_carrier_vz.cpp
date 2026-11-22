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
      .area = "subos", .covers = {"CARRIER-VZ-LIFECYCLE", "CARRIER-GUEST-CURRENT"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("carrier-vz");
    const auto vms = home.root() / "vz";
    fs::create_directories(vms);
    const std::map<std::string, std::string> env{
        {"XLINGS_VZ_HELPER", fake_helper().string()}, {"FAKE_VZ_ROOT", vms.string()},
        {"XLINGS_CARRIER_GUEST_XLINGS", tk::xlings_binary().string()},
        {"XLINGS_CARRIER_GUEST_KERNEL", (home.root() / "vmlinuz").string()}};
    tk::write_file(home.root() / "vmlinuz", "a machine kernel");
    auto made = home.xlings({"subos", "new", "box", "--carrier", "vz"}, env);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto vm = vms / "vms" / xlings::carrier::vz::vm_name(home.dir());
    EXPECT_TRUE(fs::is_directory(vm / "xlings/subos/box")) << made.transcript();
    EXPECT_FALSE(fs::exists(vm / "etc/wsl.conf")) << "a VM is not a WSL distribution";
    // What Virtualization.framework boots is in its tree: the machine kernel.
    EXPECT_EQ(read_or_empty(vm / "boot/vmlinuz"), "a machine kernel");

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

    // An upgrade reaches the guest as on WSL 2 (#724 §4.4): an older release
    // there is given the host's Linux build through a share and adopted by
    // rename before the call; once; a newer guest is left as it is.
    {
        const auto guest = vm / "xlings/bin/xlings";
        const auto real = vm / "xlings/bin/xlings.real";
        const auto stand_in = [&](std::string_view version) {
            fs::remove(guest);
            tk::write_file(guest, std::format("#!/bin/sh\n"
                                              "case \"$1\" in __carrier-probe) echo '{{\"kvm\":false,\"kernel\":\"6\",\"version\":\"{}\"}}'; exit 0 ;; esac\n"
                                              "exec \"$(dirname \"$0\")/xlings.real\" \"$@\"\n", version));
            fs::permissions(guest, fs::perms::owner_all);
        };
        fs::copy_file(guest, real);
        const auto probe_file = home.dir() / "carriers/vz/probe.json";
        const auto age = [&] { fs::last_write_time(probe_file, fs::file_time_type::clock::now() - std::chrono::hours(1)); };
        const auto adoptions = [&] {
            const auto log = read_or_empty(vms / "calls.log");
            std::size_t n = 0;
            for (std::size_t at = 0; (at = log.find("__carrier-adopt", at)) != std::string::npos; ++at) ++n;
            return n;
        };
        ASSERT_TRUE(fs::exists(probe_file)) << read_or_empty(vms / "calls.log");

        stand_in("2000.1.1.1");
        age();
        auto brought = home.xlings({"subos", "info", "box"}, env);
        EXPECT_EQ(brought.exit_code, 0) << brought.transcript();
        EXPECT_EQ(adoptions(), 1u) << read_or_empty(vms / "calls.log");
        EXPECT_EQ(read_or_empty(guest), read_or_empty(tk::xlings_binary())) << "the guest's xlings was not replaced";
        auto kept = nlohmann::json::parse(read_or_empty(probe_file), nullptr, false);
        EXPECT_TRUE(kept["update"].value("ok", false)) << kept.dump();
        age();
        EXPECT_EQ(home.xlings({"subos", "info", "box"}, env).exit_code, 0);
        EXPECT_EQ(adoptions(), 1u) << "not again: the guest is current";

        stand_in("2099.1.1.1");
        age();
        EXPECT_EQ(home.xlings({"subos", "info", "box"}, env).exit_code, 0);
        EXPECT_EQ(adoptions(), 1u) << "a newer guest is not replaced";
        fs::remove(guest);
        fs::rename(real, guest);
    }

    auto removed = home.xlings({"subos", "remove", "box", "-y"}, env);
    EXPECT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(vm / "xlings/subos/box"));
}
