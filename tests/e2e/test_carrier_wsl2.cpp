// The wsl2 carrier's lifecycle (SubOS design part 3 §5.3), on any POSIX CI
// host: a stand-in wsl.exe (tests/fixtures/carrier/fake-wsl.sh) keeps each
// "distribution" as a directory. The image is the real one -- written by this
// xlings, extracted by tar -- and the guest is a real xlings, self-contained
// at <distro>/xlings. What the stand-in cannot be is a WSL2 kernel: interop,
// drvfs grants and namespaces inside need a Windows host with WSL2.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.carrier;
import xlings.carrier.wsl2;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace w2 = xlings::carrier::wsl2;

namespace {

fs::path fake_wsl() { return fs::path(__FILE__).parent_path().parent_path() / "fixtures/carrier/fake-wsl.sh"; }

std::string read_or_empty(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

XTEST(CarrierWsl2, ItsListingIsReadInEitherEncodingAndItsNameIsTheHomes,
      .area = "subos", .covers = {"CARRIER-WSL-LIFECYCLE"}) {
    using namespace std::string_literals;
    EXPECT_EQ(w2::parse_list("Ubuntu\r\nxlings-abc\r\n"), (std::vector<std::string>{"Ubuntu", "xlings-abc"}));
    // UTF-16LE with a BOM, as wsl.exe answers without WSL_UTF8.
    const auto utf16 = "\xFF\xFE" "U\0b\0u\0n\0t\0u\0\r\0\n\0x\0l\0\r\0\n\0"s;
    EXPECT_EQ(w2::parse_list(utf16), (std::vector<std::string>{"Ubuntu", "xl"}));
    EXPECT_EQ(w2::distro_name("/h/.xlings"), w2::distro_name("/h/./.xlings"));
    EXPECT_NE(w2::distro_name("/h/.xlings"), w2::distro_name("/g/.xlings"));
    EXPECT_TRUE(w2::distro_name("/h").starts_with("xlings-"));
    EXPECT_EQ(w2::launcher("C:\\Windows\\System32\\wsl.exe", "xlings-1"),
              (std::vector<std::string>{"C:\\Windows\\System32\\wsl.exe", "-d", "xlings-1", "-u", "root", "--exec"}));
}

XTEST(CarrierWsl2, TheImageTurnsInteropAndAutomountOffAndAGrantIsOnlyUnderGrant,
      .area = "subos", .covers = {"CARRIER-WSL-INTEROP-OFF", "CARRIER-WSL-GRANT"}) {
    std::string conf;
    bool init_link = false;
    for (const auto& f : w2::image_files()) {
        if (f.path == "etc/wsl.conf") conf = f.content;
        if (f.path == "sbin/mount.drvfs") init_link = f.link == "/init";
    }
    EXPECT_NE(conf.find("[interop]\nenabled = false"), std::string::npos) << conf;
    EXPECT_NE(conf.find("appendWindowsPath = false"), std::string::npos) << conf;
    EXPECT_NE(conf.find("[automount]\nenabled = false"), std::string::npos) << conf;
    EXPECT_TRUE(init_link);
    // The guest side refuses a mount point outside /grant/.
    const std::vector<std::string> outside{"C:\\work", "/etc", "rw"};
    EXPECT_EQ(w2::guest_grant(outside), 2);
    const std::vector<std::string> mode{"C:\\work", "/grant/w", "rwx"};
    EXPECT_EQ(w2::guest_grant(mode), 2);
}

XTEST(CarrierWsl2, ASubosOnTheCarrierIsCreatedThereOnceUsedThereAndRemovedThere,
      .area = "subos", .covers = {"CARRIER-WSL-LIFECYCLE"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("carrier-wsl2");
    const auto wsl = home.root() / "wsl";
    fs::create_directories(wsl);
    const std::map<std::string, std::string> env{
        {"XLINGS_WSL_EXE", fake_wsl().string()},
        {"FAKE_WSL_ROOT", wsl.string()},
        {"XLINGS_CARRIER_GUEST_XLINGS", tk::xlings_binary().string()},
    };
    auto made = home.xlings({"subos", "new", "box", "--carrier", "wsl2"}, env);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto name = w2::distro_name(home.dir());
    const auto distro = wsl / "distros" / name;
    ASSERT_TRUE(fs::is_directory(distro / "xlings/subos/box")) << made.transcript();
    EXPECT_NE(read_or_empty(distro / "etc/wsl.conf").find("[interop]\nenabled = false"), std::string::npos);
    // Here: only the name and where it runs.
    auto instance = nlohmann::json::parse(tk::read_file(home.dir() / "config/subos/box/instance.json"));
    EXPECT_EQ(instance["carrier"], "wsl2");
    EXPECT_TRUE(fs::is_empty(home.dir() / "subos/box"));

    // Used there, through the same distribution: imported once.
    auto info = home.xlings({"subos", "info", "box"}, env);
    EXPECT_EQ(info.exit_code, 0) << info.transcript();
    auto calls = read_or_empty(wsl / "calls.log");
    std::size_t imports = 0;
    for (std::size_t at = 0; (at = calls.find("--import", at)) != std::string::npos; ++at) ++imports;
    EXPECT_EQ(imports, 1u) << calls;
    const auto run_there = calls.find("-d " + name + " -u root --exec /xlings/bin/xlings __carrier-env");
    ASSERT_NE(run_there, std::string::npos) << calls;
    EXPECT_NE(calls.find("-- subos info box", run_there), std::string::npos) << calls;

    // Listed here with where it runs.
    auto listed = home.xlings({"interface", "list_subos"}, env);
    bool seen = false;
    for (const auto& j : listed.json_lines())
        if (j.value("dataKind", "") == "subos_list")
            for (const auto& e : j["payload"]["entries"])
                if (e["name"] == "box") { seen = true; EXPECT_EQ(e["carrier"], "wsl2"); }
    EXPECT_TRUE(seen) << listed.transcript();

    // Removed there; then here.
    auto removed = home.xlings({"subos", "remove", "box", "-y"}, env);
    EXPECT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(distro / "xlings/subos/box"));
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box"));
    EXPECT_FALSE(fs::exists(home.dir() / "config/subos/box/instance.json"));
    EXPECT_TRUE(fs::is_directory(distro)) << "the carrier is the home's; removing a SubOS keeps it";
}
