// What a computer can do with Luban (part 4 §6.3), from stated facts: the
// report for Linux, a Mac and a PC is computed here on any platform, so each
// platform's answers -- ready, why not, the next step -- are checked where
// the CI has none of them.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import openhal;
import luban.service;

namespace svc = luban::service;
using Json = nlohmann::json;

namespace {

svc::HostFacts facts_(std::string platform, Json isolation) {
    svc::HostFacts f;
    f.platform = std::move(platform);
    f.os = f.platform;
    f.arch = "x86_64";
    f.isolation = std::move(isolation);
    f.app = "/opt/luban-os";
    return f;
}

Json carrier_(std::string name, bool supported, std::string reason = {}, std::string route = {}) {
    return {{"carriers", Json::array({{{"name", name}, {"supported", supported}, {"reason", reason}, {"route", route}}})}};
}

}  // namespace

XTEST(LubanCapabilities, LinuxRunsBothWaysAndSaysWhenAMachineIsEmulated, .area = "luban", .covers = {"PLATFORM-CAPS"}) {
    auto f = facts_("linux", {{"ok", true}, {"backend", {{"name", "bwrap"}}}});
    f.kvm = true;
    auto c = svc::capabilities(f);
    ASSERT_NE(c.mode("sandbox"), nullptr);
    EXPECT_TRUE(c.mode("sandbox")->ready);
    EXPECT_EQ(c.mode("sandbox")->how, "isolated here (bwrap)");
    EXPECT_TRUE(c.mode("machine")->ready);
    EXPECT_EQ(c.mode("machine")->traits.at("accel"), "kvm");
    EXPECT_TRUE(c.mode("desktop")->ready);
    f.kvm = false;
    c = svc::capabilities(f);
    EXPECT_TRUE(c.mode("machine")->ready);
    EXPECT_NE(c.mode("machine")->how.find("slow"), std::string::npos);
    // Not isolated yet: the one-time setup is the next step.
    c = svc::capabilities(facts_("linux", {{"ok", false}}));
    EXPECT_FALSE(c.mode("sandbox")->ready);
    EXPECT_EQ(c.mode("sandbox")->route, "luban setup");
}

XTEST(LubanCapabilities, AMacRunsSystemsInItsHostMachine, .area = "luban", .covers = {"PLATFORM-CAPS"}) {
    auto c = svc::capabilities(facts_("macos", carrier_("vz", true)));
    EXPECT_TRUE(c.mode("sandbox")->ready);
    EXPECT_EQ(c.mode("sandbox")->traits.at("carrier"), "vz");
    EXPECT_NE(c.mode("sandbox")->how.find("Luban host machine"), std::string::npos);
    EXPECT_TRUE(c.mode("machine")->ready);
    c = svc::capabilities(facts_("macos", carrier_("vz", false, "this Mac has no virtualization here", "luban setup")));
    EXPECT_FALSE(c.mode("sandbox")->ready);
    EXPECT_EQ(c.mode("sandbox")->reason, "this Mac has no virtualization here");
    EXPECT_FALSE(c.mode("machine")->ready);
    EXPECT_EQ(c.mode("machine")->reason, c.mode("sandbox")->reason);
    EXPECT_FALSE(c.mode("desktop")->ready);
}

XTEST(LubanCapabilities, APcRunsMachinesOnItsHostMachinesKvmWhenWindowsGivesIt, .area = "luban", .covers = {"PLATFORM-CAPS"}) {
    auto f = facts_("windows", carrier_("wsl2", true));
    // Not started yet: not known, and said so -- never guessed.
    auto c = svc::capabilities(f);
    EXPECT_TRUE(c.mode("sandbox")->ready);
    EXPECT_FALSE(c.mode("machine")->ready);
    EXPECT_NE(c.mode("machine")->reason.find("not known"), std::string::npos);
    f.host_machine_kvm = false;
    c = svc::capabilities(f);
    EXPECT_FALSE(c.mode("machine")->ready);
    EXPECT_NE(c.mode("machine")->route.find("nestedVirtualization"), std::string::npos);
    f.host_machine_kvm = true;
    c = svc::capabilities(f);
    EXPECT_TRUE(c.mode("machine")->ready);
    EXPECT_EQ(c.mode("machine")->traits.at("carrier"), "wsl2");
    // No WSL 2: the carrier's own reason and route.
    c = svc::capabilities(facts_("windows", carrier_("wsl2", false, "WSL 2 is not installed", "luban setup")));
    EXPECT_FALSE(c.mode("sandbox")->ready);
    EXPECT_EQ(c.mode("sandbox")->reason, "WSL 2 is not installed");
    EXPECT_EQ(c.mode("sandbox")->route, "luban setup");
}

XTEST(LubanCapabilities, WhatCouldNotBeAskedIsSaidAndTheJsonCarriesItAll, .area = "luban", .covers = {"PLATFORM-CAPS"}) {
    auto f = facts_("linux", Json());
    f.isolation.reset();
    f.app.clear();
    f.devices.push_back(svc::Devices{.kind = openhal::Kind::Gpu, .unreadable = "no /sys here"});
    f.devices.push_back(svc::Devices{.kind = openhal::Kind::Display,
                                     .devices = {openhal::Device{openhal::Kind::Display, "HDMI-A-1@card0", "U2723QE", "DEL",
                                                                 {{"resolution", "3840x2160"}}}}});
    const auto c = svc::capabilities(f);
    EXPECT_FALSE(c.mode("sandbox")->ready);
    EXPECT_FALSE(c.mode("sandbox")->reason.empty());
    EXPECT_EQ(c.mode("desktop")->route, "xlings install luban-os");
    const auto j = svc::to_json(c);
    ASSERT_EQ(j["modes"].size(), 3u);
    EXPECT_EQ(j["modes"][0]["name"], "sandbox");
    EXPECT_FALSE(j["modes"][0]["reason"].get<std::string>().empty());
    EXPECT_EQ(j["devices"]["gpu"]["unreadable"], "no /sys here");
    EXPECT_EQ(j["devices"]["display"][0]["traits"]["resolution"], "3840x2160");
}

// `xlings install luban-os` puts the app among the home's programs (its active
// SubOS), not beside luban: the report and `--display window` find it there.
XTEST(LubanCapabilities, TheInstalledAppIsFoundWhereXlingsPutsIt, .area = "luban", .covers = {"PLATFORM-CAPS"}) {
    auto home = xlings::testkit::Home::isolated("caps-app");
    const auto name = xlings::testkit::is_windows ? "luban-os.exe" : "luban-os";
    EXPECT_TRUE(svc::luban_os_app(home.dir()).empty());
    const auto bin = home.dir() / "subos" / "current" / "bin";
    std::filesystem::create_directories(bin);
    std::ofstream(bin / name) << "app";
    EXPECT_EQ(svc::luban_os_app(home.dir()), bin / name);
}

// What going in shows first (design 2026-10-18 §8): the desktop set, else the
// first one there, else the console; a desktop set and not there is the
// console with why and the next step -- never silent (E20).
XTEST(LubanEnterPlan, ADesktopSetAndNotThereIsTheConsoleWithWhy, .area = "luban", .covers = {"LUBAN-SETTINGS"}) {
    luban::service::Environment e;
    e.name = "box";
    auto plan = luban::service::enter_plan(e);
    EXPECT_EQ(plan.target, "console");
    EXPECT_TRUE(plan.why.empty()) << "a system without a desktop has nothing to explain";
    e.desktops = {"labwc", "xdesk-classic"};
    plan = luban::service::enter_plan(e);
    EXPECT_EQ(plan.target, "desktop");
    EXPECT_EQ(plan.session, "labwc");
    e.settings.desktop = "xdesk-classic";
    EXPECT_EQ(luban::service::enter_plan(e).session, "xdesk-classic");
    e.settings.desktop = "none";
    plan = luban::service::enter_plan(e);
    EXPECT_EQ(plan.target, "console");
    EXPECT_TRUE(plan.why.empty());
    e.settings.desktop = "kde";
    plan = luban::service::enter_plan(e);
    EXPECT_EQ(plan.target, "console");
    EXPECT_NE(plan.why.find("kde"), std::string::npos) << plan.why;
    EXPECT_NE(plan.why.find("labwc, xdesk-classic"), std::string::npos) << plan.why;
    EXPECT_NE(plan.why.find("luban config box --desktop"), std::string::npos) << plan.why;
}
