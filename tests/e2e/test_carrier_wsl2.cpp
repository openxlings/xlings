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

// The one-time setup a Windows machine needs for a Linux SubOS: WSL from the
// index's MSI and the Virtual Machine Platform, each a Windows program under
// System32 (run through the elevation door); never `wsl --install` from the
// Microsoft Store, never a command the user types.
XTEST(CarrierWsl2, ItsOneTimeSetupIsTheIndexsWslAndTheVirtualMachinePlatform,
      .area = "subos", .covers = {"CARRIER-WSL-SETUP"}) {
    const auto steps = w2::setup_steps("C:/h/data/xpkgs/xim-x-wsl-installer/3.0.1/wsl.msi", "C:/Windows/System32");
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0], (std::vector<std::string>{(fs::path("C:/Windows/System32") / "msiexec.exe").string(), "/i",
                                                  "C:/h/data/xpkgs/xim-x-wsl-installer/3.0.1/wsl.msi", "/quiet", "/norestart"}));
    EXPECT_EQ(steps[1][0], (fs::path("C:/Windows/System32") / "dism.exe").string());
    EXPECT_NE(std::ranges::find(steps[1], "/featurename:VirtualMachinePlatform"), steps[1].end());
    EXPECT_EQ(w2::kRestartRequired, 3010);
    EXPECT_EQ(w2::kSetupCommand, "xlings self doctor --isolation --fix");
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
      .area = "subos", .covers = {"CARRIER-WSL-LIFECYCLE", "PLATFORM-CAPS", "CARRIER-GUEST-CURRENT"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("carrier-wsl2");
    const auto wsl = home.root() / "wsl";
    fs::create_directories(wsl);
    const std::map<std::string, std::string> env{
        {"XLINGS_WSL_EXE", fake_wsl().string()},
        {"FAKE_WSL_ROOT", wsl.string()},
        {"XLINGS_CARRIER_GUEST_XLINGS", tk::xlings_binary().string()},
        // The guest's userland (a static busybox in a real image).
        {"XLINGS_CARRIER_GUEST_BUSYBOX", (home.root() / "busybox").string()},
    };
    tk::write_file(home.root() / "busybox", "#!/bin/sh\n");
    auto made = home.xlings({"subos", "new", "box", "--carrier", "wsl2"}, env);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto name = w2::distro_name(home.dir());
    const auto distro = wsl / "distros" / name;
    ASSERT_TRUE(fs::is_directory(distro / "xlings/subos/box")) << made.transcript();
    EXPECT_NE(read_or_empty(distro / "etc/wsl.conf").find("[interop]\nenabled = false"), std::string::npos);
    // A shell there, for what a package's hooks run (sh -c, find, ln).
    EXPECT_TRUE(fs::is_regular_file(distro / "bin/busybox"));
    EXPECT_EQ(fs::read_symlink(distro / "bin/sh"), "busybox");
    // Here: only the name and where it runs.
    auto instance = nlohmann::json::parse(tk::read_file(home.dir() / "config/subos/box/instance.json"));
    EXPECT_EQ(instance["carrier"], "wsl2");
    EXPECT_TRUE(fs::is_empty(home.dir() / "subos/box"));

    // What its machine gives a Luban system (luban status reads it): asked
    // there once the carrier is up.
    const auto probe = nlohmann::json::parse(read_or_empty(home.dir() / "carriers/wsl2/probe.json"), nullptr, false);
    ASSERT_TRUE(probe.is_object()) << read_or_empty(wsl / "calls.log");
    EXPECT_TRUE(probe["kvm"].is_boolean()) << probe.dump();
    EXPECT_FALSE(probe.value("kernel", std::string{}).empty()) << probe.dump();

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

    // An upgrade reaches the guest (part 4 §9): a machine that reports another
    // release is brought to this one -- `xlings self update` there, once per
    // release, not on every command. (In this test, not a second one: each
    // carrier image carries a whole xlings.)
    {
        const auto guest = distro / "xlings/bin/xlings";
        const auto real = distro / "xlings/bin/xlings.real";
        fs::rename(guest, real);
        tk::write_file(guest, "#!/bin/sh\necho \"$*\" >> \"$(dirname \"$0\")/../calls\"\n"
                              "case \"$1\" in __carrier-probe) echo '{\"kvm\":false,\"kernel\":\"6\",\"version\":\"2000.1.1.1\"}' ;; "
                              "self) exit 0 ;; esac\nexec \"$(dirname \"$0\")/xlings.real\" \"$@\"\n");
        fs::permissions(guest, fs::perms::owner_all);
        const auto probe_file = home.dir() / "carriers/wsl2/probe.json";
        const auto age = [&] { fs::last_write_time(probe_file, fs::file_time_type::clock::now() - std::chrono::hours(1)); };
        const auto updates = [&] {
            const auto calls = read_or_empty(distro / "xlings/calls");
            std::size_t n = 0;
            for (std::size_t at = 0; (at = calls.find("self update", at)) != std::string::npos; ++at) ++n;
            return n;
        };
        age();
        EXPECT_EQ(home.xlings({"subos", "info", "box"}, env).exit_code, 0);
        EXPECT_EQ(updates(), 1u) << read_or_empty(distro / "xlings/calls");
        const auto kept = nlohmann::json::parse(read_or_empty(probe_file), nullptr, false);
        EXPECT_FALSE(kept.value("updated_for", std::string{}).empty()) << kept.dump();
        age();
        (void)home.xlings({"subos", "info", "box"}, env);
        EXPECT_EQ(updates(), 1u) << "not again for the same release";
        fs::remove(guest);
        fs::rename(real, guest);
    }

    // Removed there; then here.
    auto removed = home.xlings({"subos", "remove", "box", "-y"}, env);
    EXPECT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(distro / "xlings/subos/box"));
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box"));
    EXPECT_FALSE(fs::exists(home.dir() / "config/subos/box/instance.json"));
    EXPECT_TRUE(fs::is_directory(distro)) << "the carrier is the home's; removing a SubOS keeps it";
}
