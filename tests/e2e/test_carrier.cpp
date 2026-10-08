// Carriers (SubOS design part 3 §5): where a SubOS runs, and how this xlings
// reaches it -- through one launcher prefix and the NDJSON interface.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.carrier;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace cr = xlings::carrier;

namespace {

cr::Probe yes(std::string_view) { return {.supported = true}; }
cr::Probe no_wsl(std::string_view name) {
    if (name == "local") return {.supported = true};
    return {.supported = false, .reason = "WSL is not installed", .route = "wsl --install --no-distribution"};
}

}  // namespace

XTEST(Carrier, TheChoiceFollowsWhatTheSubosIsNotWhereItIsAskedFrom,
      .area = "subos", .covers = {"CARRIER-LOCAL", "CARRIER-UNAVAILABLE"}) {
    // Linux: one kernel, every SubOS here.
    auto lin = cr::choose("linux", {.abi = "native", .root = true}, yes);
    ASSERT_TRUE(lin);
    EXPECT_EQ(lin->carrier, "local");
    // Windows: its own programs stay here; a Linux one or a root goes to WSL2.
    EXPECT_EQ(cr::choose("windows", {}, yes)->carrier, "local");
    EXPECT_EQ(cr::choose("windows", {.abi = "linux"}, yes)->carrier, "wsl2");
    EXPECT_EQ(cr::choose("windows", {.root = true}, yes)->carrier, "wsl2");
    EXPECT_EQ(cr::choose("macos", {.root = true}, yes)->carrier, "vz");
    EXPECT_EQ(cr::choose("macos", {.strong = true}, yes)->carrier, "vz");
    // Unavailable: refused with the route, and a native SubOS is unaffected.
    auto refused = cr::choose("windows", {.abi = "linux"}, no_wsl);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().route, "wsl --install --no-distribution");
    EXPECT_TRUE(cr::choose("windows", {}, no_wsl));
    // Asked for by name: honoured or refused, never silently replaced.
    EXPECT_FALSE(cr::choose("linux", {.requested = "wsl2"}, yes));
    EXPECT_FALSE(cr::choose("windows", {.requested = "local", .abi = "linux"}, yes));
    EXPECT_EQ(cr::choose("windows", {.requested = "wsl2"}, yes)->carrier, "wsl2");
}

XTEST(Carrier, ALocalSubosRecordsWhereItRunsAndAForeignCarrierIsRefusedWithItsRoute,
      .area = "subos", .covers = {"CARRIER-LOCAL", "CARRIER-UNAVAILABLE"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("carrier-local");
    auto made = home.xlings({"subos", "new", "box", "--carrier", "local"});
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    auto instance = nlohmann::json::parse(tk::read_file(home.dir() / "config/subos/box/instance.json"));
    EXPECT_EQ(instance["carrier"], "local");
    EXPECT_EQ(instance["abi"], "native");
    // A Linux ABI on Linux is the native one: nothing to record.
    ASSERT_EQ(home.xlings({"subos", "new", "plain", "--abi", "linux"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(home.dir() / "config/subos/plain/instance.json"));

    auto listed = home.xlings({"interface", "list_subos"});
    ASSERT_EQ(listed.exit_code, 0) << listed.transcript();
    bool seen = false;
    for (const auto& j : listed.json_lines()) {
        if (j.value("dataKind", "") != "subos_list") continue;
        for (const auto& e : j["payload"]["entries"]) {
            if (e["name"] != "box") continue;
            seen = true;
            EXPECT_EQ(e["carrier"], "local");
            EXPECT_EQ(e["abi"], "native");
            EXPECT_EQ(e["view"], "overlay");
        }
    }
    EXPECT_TRUE(seen) << listed.transcript();

    auto foreign = home.xlings({"subos", "new", "far", "--carrier", "wsl2"});
    EXPECT_EQ(foreign.exit_code, 125) << foreign.transcript();
    EXPECT_NE(foreign.transcript().find("does not exist on linux"), std::string::npos) << foreign.transcript();
    EXPECT_NE(foreign.transcript().find("--carrier local"), std::string::npos) << foreign.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/far"));
}

XTEST(Carrier, AnEndpointIsReachedThroughItsLauncherWithTheNdjsonInterface,
      .area = "subos", .covers = {"CARRIER-LOCAL"}, .requires_ = {"linux", "xlings-bin"}) {
    // What wsl.exe -d <distro> --exec does for the wsl2 carrier, /usr/bin/env
    // does here: a prefix that runs the command "there", with that home.
    auto home = tk::Home::isolated("carrier-launcher");
    ASSERT_EQ(home.xlings({"subos", "new", "guest"}).exit_code, 0);
    const cr::Endpoint at{.carrier = "test", .instance = "guest-machine",
                          .launcher = {"/usr/bin/env", "XLINGS_HOME=" + home.dir().string(),
                                       "HOME=" + home.root().string()},
                          .xlings = tk::xlings_binary().string()};
    auto reply = cr::control(at, "list_subos", nlohmann::json::object());
    ASSERT_TRUE(reply) << reply.error();
    EXPECT_EQ(reply->exit_code, 0) << reply->diagnostics;
    bool guest = false;
    for (const auto& e : reply->events)
        if (e.value("dataKind", "") == "subos_list")
            for (const auto& entry : e["payload"]["entries"]) guest |= entry["name"] == "guest";
    EXPECT_TRUE(guest);
    const std::vector<std::string> version{"--version"};
    EXPECT_EQ(cr::terminal(at, version), 0);
    const std::vector<std::string> unknown{"subos", "info", "no-such-subos"};
    EXPECT_NE(cr::terminal(at, unknown), 0) << "the exit code is the command's, through the launcher";
}
