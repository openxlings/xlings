// A private environment, probed from inside (Luban design §C5, §C8): what the
// host plants as a sentinel must not be there; the persona is the same each
// time and another instance's is not; the zone is the proxy's exit.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::string host_name() {
    std::ifstream in("/proc/sys/kernel/hostname");
    std::string h;
    std::getline(in, h);
    return h;
}

const char* kProbe =
    "echo HOST=$(hostname); echo MID=$(cat /etc/machine-id 2>/dev/null); env; "
    "echo DMI=$(ls /sys/class/dmi/id 2>/dev/null | head -3 | tr '\\n' ,); "
    "echo DISKS=$(ls /dev/disk 2>/dev/null | tr '\\n' ,); "
    "echo IFACES=$(tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d ' ' | tr '\\n' ,)";

}  // namespace

XTEST(AgentPrivacy, NothingTheHostPlantsIsVisibleAndThePersonaHolds,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"PRIVACY-LEAK-PROBE", "PRIVACY-PERSONA-STABLE"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}, .proves = "isolation") {
    auto home = tk::Home::isolated("privacy");
    home.seed_sandbox_backend();
    ASSERT_EQ(home.xlings({"subos", "new", "agent"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "config", "agent", "--sandbox=locked"}).exit_code, 0);
    const std::map<std::string, std::string> sentinels{
        {"LC_TIME", "xx_SENTINEL_LC.UTF-8"}, {"SECRET_TOKEN", "sentinel-secret-7731"},
        {"SSH_AUTH_SOCK", "/tmp/sentinel-agent.sock"}, {"TZ", "Sentinel/Zone"}};
    auto first = home.xlings({"subos", "use", "agent", "--cmd", kProbe}, sentinels);
    ASSERT_EQ(first.exit_code, 0) << first.transcript();
    for (const auto& [k, v] : sentinels)
        EXPECT_EQ(first.out.find(v), std::string::npos) << k << " crossed in\n" << first.out;
    if (const auto h = host_name(); h.size() > 2)
        EXPECT_EQ(first.out.find("HOST=" + h), std::string::npos) << "the host's name\n" << first.out;
    EXPECT_NE(first.out.find("TZ=UTC"), std::string::npos) << first.out;
    EXPECT_NE(first.out.find("DMI=\n"), std::string::npos) << "firmware identity visible\n" << first.out;
    EXPECT_NE(first.out.find("DISKS=\n"), std::string::npos) << "disk serials visible\n" << first.out;
    EXPECT_NE(first.out.find("IFACES=lo,"), std::string::npos) << first.out;

    const auto persona = Json::parse(tk::read_file(home.dir() / "config/subos/agent/persona.json"));
    const auto name = persona["hostname"].get<std::string>();
    EXPECT_NE(first.out.find("HOST=" + name), std::string::npos) << first.out;
    auto second = home.xlings({"subos", "use", "agent", "--cmd", "echo HOST=$(hostname)"});
    EXPECT_NE(second.out.find("HOST=" + name), std::string::npos) << "the same persona on every entry\n" << second.out;

    // A fork is another instance, with a persona of its own.
    ASSERT_EQ(home.xlings({"subos", "new", "agent2", "--from", "agent"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "config", "agent2", "--sandbox=locked"}).exit_code, 0);
    auto fork = home.xlings({"subos", "use", "agent2", "--cmd", "echo HOST=$(hostname)"});
    ASSERT_EQ(fork.exit_code, 0) << fork.transcript();
    EXPECT_NE(fork.out.find("HOST="), std::string::npos) << fork.out;
    EXPECT_EQ(fork.out.find("HOST=" + name), std::string::npos) << "a fork carried its source's identity\n" << fork.out;

    auto status = home.xlings({"subos", "status", "agent", "--json"});
    const auto s = Json::parse(status.out);
    EXPECT_EQ(s["identity"]["hostname"], name) << status.out;
    EXPECT_FALSE(s["identity"]["exposed"].empty()) << "what a shared kernel cannot hide is said\n" << status.out;
}

XTEST(AgentPrivacy, TheZoneIsTheProxysExitAskedThroughTheProxy,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"PRIVACY-TZ-PROXY"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    auto home = tk::Home::isolated("privacy-tz");
    home.seed_sandbox_backend();
    // The home's curl, which the tool table prefers: it records how it was
    // asked and answers as a lookup service would.
    const auto asked = home.root() / "curl-asked";
    const auto curl = home.dir() / "data/xpkgs/xim-x-curl/8.0.0/bin/curl";
    tk::write_file(curl, std::format("#!/bin/sh\necho \"$@\" > {}\necho Asia/Tokyo\n", asked.string()));
    fs::permissions(curl, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    ASSERT_EQ(home.xlings({"subos", "new", "agent"}).exit_code, 0);
    auto set = home.xlings({"subos", "config", "agent", "--sandbox=private", "--proxy", "socks5h://127.0.0.1:9"});
    ASSERT_EQ(set.exit_code, 0) << set.transcript();
    auto r = home.xlings({"subos", "use", "agent", "--cmd", "echo TZ=$TZ"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(r.out.find("TZ=Asia/Tokyo"), std::string::npos) << r.transcript();
    const auto how = tk::read_file(asked);
    EXPECT_NE(how.find("--proxy socks5h://127.0.0.1:9"), std::string::npos) << "asked around the proxy: " << how;
    // Answered once, then cached for this proxy.
    fs::remove(asked);
    auto again = home.xlings({"subos", "use", "agent", "--cmd", "echo TZ=$TZ"});
    EXPECT_NE(again.out.find("TZ=Asia/Tokyo"), std::string::npos) << again.transcript();
    EXPECT_FALSE(fs::exists(asked)) << "asked again within the day";
    // A chosen zone is not asked for.
    ASSERT_EQ(home.xlings({"subos", "config", "agent", "--tz", "utc"}).exit_code, 0);
    auto utc = home.xlings({"subos", "use", "agent", "--cmd", "echo TZ=$TZ"});
    EXPECT_NE(utc.out.find("TZ=UTC"), std::string::npos) << utc.transcript();
}
