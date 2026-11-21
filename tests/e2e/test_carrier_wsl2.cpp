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
import xlings.platform;
import xlings.platform.target;

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
        {"XLINGS_CARRIER_GUEST_BWRAP", (home.root() / "bwrap").string()},
    };
    tk::write_file(home.root() / "busybox", "#!/bin/sh\n");
    tk::write_file(home.root() / "bwrap", "#!/bin/sh\n");
    auto made = home.xlings({"subos", "new", "box", "--carrier", "wsl2"}, env);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto name = w2::distro_name(home.dir());
    const auto distro = wsl / "distros" / name;
    ASSERT_TRUE(fs::is_directory(distro / "xlings/subos/box")) << made.transcript();
    EXPECT_NE(read_or_empty(distro / "etc/wsl.conf").find("[interop]\nenabled = false"), std::string::npos);
    // A shell there, for what a package's hooks run (sh -c, find, ln).
    EXPECT_TRUE(fs::is_regular_file(distro / "bin/busybox"));
    EXPECT_EQ(fs::read_symlink(distro / "bin/sh"), "busybox");
    // And bwrap, for what a root's making runs inside one.
    EXPECT_TRUE(fs::is_regular_file(distro / "usr/bin/bwrap"));
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

    // Its userland is at this release's revision (#724 §4.4): the standard
    // loader directory is the home's runtime, linked when the image has none.
    {
        const bool arm = std::string_view(xlings::platform::build_arch()) != "x86_64";
        const auto std_dir = distro / (arm ? "lib" : "lib64");
        ASSERT_TRUE(fs::is_symlink(std_dir)) << read_or_empty(wsl / "calls.log");
        EXPECT_EQ(fs::read_symlink(std_dir),
                  fs::path(arm ? "/xlings/abi/linux-aarch64-glibc" : "/xlings/abi/linux-x86_64-glibc"));
        const auto userland = nlohmann::json::parse(read_or_empty(distro / "xlings/state/carrier-userland.json"), nullptr, false);
        EXPECT_EQ(userland.value("revision", 0), xlings::carrier::kUserlandRevision) << userland.dump();
        EXPECT_EQ(probe.value("userland", 0), xlings::carrier::kUserlandRevision) << probe.dump();
    }

    // An upgrade reaches the guest (part 4 §9, #724 §4.4): a machine that
    // reports an older release is given the host's Linux build -- granted,
    // adopted there by rename -- before the call; no `self update`, no
    // network there. Once: a current guest is not given it again.
    {
        const auto guest = distro / "xlings/bin/xlings";
        const auto real = distro / "xlings/bin/xlings.real";
        const auto stand_in = [&](std::string_view version) {
            fs::remove(guest);
            tk::write_file(guest, std::format("#!/bin/sh\necho \"$*\" >> \"$(dirname \"$0\")/../calls\"\n"
                                              "case \"$1\" in __carrier-probe) echo '{{\"kvm\":false,\"kernel\":\"6\",\"version\":\"{}\"}}'; exit 0 ;; "
                                              "self) exit 0 ;; esac\nexec \"$(dirname \"$0\")/xlings.real\" \"$@\"\n", version));
            fs::permissions(guest, fs::perms::owner_all);
        };
        fs::copy_file(guest, real);
        const auto probe_file = home.dir() / "carriers/wsl2/probe.json";
        const auto age = [&] { fs::last_write_time(probe_file, fs::file_time_type::clock::now() - std::chrono::hours(1)); };
        const auto adoptions = [&] {
            const auto calls = read_or_empty(wsl / "calls.log");
            std::size_t n = 0;
            for (std::size_t at = 0; (at = calls.find("__carrier-adopt", at)) != std::string::npos; ++at) ++n;
            return n;
        };

        // A failure refuses the call with its reason and is not "tried".
        stand_in("2000.1.1.1");
        age();
        auto broken = env;
        broken["XLINGS_CARRIER_GUEST_XLINGS"] = (home.root() / "no-such-build").string();
        auto refused = home.xlings({"subos", "info", "box"}, broken);
        EXPECT_NE(refused.exit_code, 0) << refused.transcript();
        EXPECT_NE(refused.transcript().find("2000.1.1.1"), std::string::npos) << refused.transcript();
        EXPECT_NE(refused.transcript().find("tried again"), std::string::npos) << refused.transcript();
        auto kept = nlohmann::json::parse(read_or_empty(probe_file), nullptr, false);
        EXPECT_FALSE(kept["update"].value("ok", true)) << kept.dump();

        // The next call brings it: the guest's xlings is the host's build.
        auto brought = home.xlings({"subos", "info", "box"}, env);
        EXPECT_EQ(brought.exit_code, 0) << brought.transcript();
        EXPECT_EQ(adoptions(), 1u) << read_or_empty(wsl / "calls.log");
        EXPECT_EQ(read_or_empty(guest), read_or_empty(tk::xlings_binary())) << "the guest's xlings was not replaced";
        EXPECT_EQ(read_or_empty(distro / "xlings/calls").find("self update"), std::string::npos);
        kept = nlohmann::json::parse(read_or_empty(probe_file), nullptr, false);
        EXPECT_TRUE(kept["update"].value("ok", false)) << kept.dump();
        age();
        EXPECT_EQ(home.xlings({"subos", "info", "box"}, env).exit_code, 0);
        EXPECT_EQ(adoptions(), 1u) << "not again: the guest is current";

        // A guest newer than this client (the user went back) is left as it is.
        stand_in("2099.1.1.1");
        age();
        EXPECT_EQ(home.xlings({"subos", "info", "box"}, env).exit_code, 0);
        EXPECT_EQ(adoptions(), 1u) << "a newer guest is not replaced";
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

// Asked through the interface (luban, an agent), a command for a SubOS on the
// carrier runs there as `interface subos_command` and its events come back
// (#724 §4.2): the guest's own error -- not "no error detail" -- and a hook
// log it names, fetched into this home.
XTEST(CarrierWsl2, AnInterfaceCallThereComesBackAsItsEvents,
      .area = "subos", .covers = {"CARRIER-RELAY-EVENTS"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("carrier-wsl2-relay");
    const auto wsl = home.root() / "wsl";
    fs::create_directories(wsl);
    const std::map<std::string, std::string> env{
        {"XLINGS_WSL_EXE", fake_wsl().string()},
        {"FAKE_WSL_ROOT", wsl.string()},
        {"XLINGS_CARRIER_GUEST_XLINGS", tk::xlings_binary().string()},
        {"XLINGS_CARRIER_GUEST_BUSYBOX", (home.root() / "busybox").string()},
        {"XLINGS_CARRIER_GUEST_BWRAP", (home.root() / "bwrap").string()},
    };
    // A busybox that can cat, what fetching a guest's log uses.
    tk::write_file(home.root() / "busybox", "#!/bin/sh\n[ \"$1\" = cat ] && { shift; exec cat \"$@\"; }\nexit 0\n");
    fs::permissions(home.root() / "busybox", fs::perms::owner_all);
    tk::write_file(home.root() / "bwrap", "#!/bin/sh\n");
    auto made = home.xlings({"subos", "new", "box", "--carrier", "wsl2"}, env);
    ASSERT_EQ(made.exit_code, 0) << made.transcript();
    const auto distro = wsl / "distros" / w2::distro_name(home.dir());

    // A terminal is still a terminal there.
    EXPECT_NE(read_or_empty(wsl / "calls.log").find("-- subos new box"), std::string::npos);

    // The guest's error, as an event.
    auto info = home.xlings({"interface", "subos_command", "--args",
                             R"({"argv":["info","box","--no-such-flag-there"]})"}, env);
    auto calls = read_or_empty(wsl / "calls.log");
    EXPECT_NE(calls.find("interface subos_command --args"), std::string::npos) << calls;
    EXPECT_EQ(info.transcript().find("no error detail"), std::string::npos) << info.transcript();
    EXPECT_EQ(info.transcript().find("[stray stdout]"), std::string::npos) << info.transcript();

    // A guest that fails with a hook log: the log is here, and the error names it.
    const auto guest = distro / "xlings/bin/xlings";
    const auto real = distro / "xlings/bin/xlings.real";
    fs::rename(guest, real);
    tk::write_file(distro / "xlings/logs/hooks/demo-install.log", "the hook's whole record\n");
    tk::write_file(guest, "#!/bin/sh\n"
        "case \"$*\" in *interface*) "
        "echo '{\"kind\":\"progress\",\"phase\":\"install\",\"percent\":0.5,\"message\":\"demo\"}'; "
        "echo '{\"kind\":\"error\",\"code\":\"E_INTERNAL\",\"message\":\"[demo] failed: install hook failed;  full log: /xlings/logs/hooks/demo-install.log\",\"recoverable\":true,\"hint\":\"\"}'; "
        "echo '{\"kind\":\"result\",\"exitCode\":1}'; exit 1 ;; esac\n"
        "exec \"$(dirname \"$0\")/xlings.real\" \"$@\"\n");
    fs::permissions(guest, fs::perms::owner_all);
    auto failed = home.xlings({"interface", "subos_command", "--args", R"({"argv":["info","box"]})"}, env);
    EXPECT_NE(failed.exit_code, 0) << failed.transcript();
    const auto local = home.dir() / "logs/carriers/wsl2/hooks/demo-install.log";
    EXPECT_EQ(read_or_empty(local), "the hook's whole record\n") << failed.transcript();
    bool named = false, progressed = false;
    for (const auto& j : failed.json_lines()) {
        if (j.value("kind", "") == "error") {
            const auto message = j.value("message", "");
            EXPECT_EQ(message.find("no error detail"), std::string::npos) << message;
            if (message.find(local.string()) != std::string::npos) named = true;
        }
        if (j.value("kind", "") == "progress" && j.value("message", "") == "demo") progressed = true;
    }
    EXPECT_TRUE(named) << failed.transcript();
    EXPECT_TRUE(progressed) << failed.transcript();
    fs::remove(guest);
    fs::rename(real, guest);
}
