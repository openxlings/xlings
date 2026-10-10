// luban (Luban design §B3): each command is an xlings command; edition names,
// image formats and the version are luban's own.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import luban.cli;
import xlings.core.config;

namespace cli = luban::cli;
namespace tk = xlings::testkit;
using V = std::vector<std::string>;

namespace {
V x(std::initializer_list<std::string> args) {
    const V a(args);
    auto r = cli::to_xlings(a);
    return r ? *r : V{"ERROR", r.error()};
}
}  // namespace

XTEST(LubanCli, EachCommandIsTheXlingsCommandItNames, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    // A Luban edition is a Linux root: on Windows and macOS it runs on that
    // platform's carrier, with the same command.
    auto expect_new = [](V v) {
        if (const auto c = cli::default_carrier(); !c.empty()) v.insert(v.end(), {"--carrier", c});
        return v;
    };
    EXPECT_EQ(x({"new", "box"}), expect_new(V{"subos", "new", "box", "--from", "subos:luban-core"}));
    EXPECT_EQ(x({"new", "box", "--carrier", "local"}), (V{"subos", "new", "box", "--from", "subos:luban-core", "--carrier", "local"}));
    EXPECT_EQ(x({"new", "agent", "agent-workspace", "--proxy", "socks5h://127.0.0.1:7897"}),
              expect_new(V{"subos", "new", "agent", "--from", "subos:luban-agent-workspace", "--proxy", "socks5h://127.0.0.1:7897"}));
    EXPECT_EQ(x({"new", "box", "--from=tiny@2026.10.20.1"}),
              expect_new(V{"subos", "new", "box", "--from", "subos:luban-tiny@2026.10.20.1"}));
    EXPECT_EQ(x({"new", "box", "acme:my-os"}), expect_new(V{"subos", "new", "box", "--from", "acme:my-os"}));
    // A preview edition has no short name; its full one still works.
    EXPECT_EQ(cli::edition_ref("desktop"), "desktop");
    EXPECT_EQ(cli::edition_ref("luban-desktop"), "subos:luban-desktop");
    EXPECT_EQ(x({"enter", "box"}), (V{"subos", "use", "box"}));
    EXPECT_EQ(x({"run", "box", "--", "ls", "-l"}), (V{"subos", "exec", "box", "--", "ls", "-l"}));
    EXPECT_EQ(x({"ls"}), (V{"subos", "list"}));
    EXPECT_EQ(x({"status", "box"}), (V{"subos", "status", "box"}));
    EXPECT_EQ(x({"config", "box", "proxy", "socks5h://p:1"}),
              (V{"subos", "config", "box", "--net", "proxy", "--proxy", "socks5h://p:1"}));
    EXPECT_EQ(x({"config", "box", "proxy", "off"}), (V{"subos", "config", "box", "--net", "host"}));
    EXPECT_EQ(x({"config", "box", "tz", "utc"}), (V{"subos", "config", "box", "--tz", "utc"}));
    EXPECT_EQ(x({"config", "box", "policy", "xim:agent-private"}),
              (V{"subos", "config", "box", "--sandbox", "xim:agent-private"}));
    EXPECT_EQ(x({"history", "box"}), (V{"subos", "rollback", "box", "--list"}));
    EXPECT_EQ(x({"rollback", "box", "--to", "3"}), (V{"subos", "rollback", "box", "--to", "3"}));
    EXPECT_EQ(x({"rm", "box"}), (V{"subos", "remove", "box"}));
    EXPECT_EQ(x({"setup"}), (V{"self", "doctor", "--isolation", "--fix"}));
}

XTEST(LubanCli, AnImageFormatFollowsItsNameAndAnUnknownNameIsRefused, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    EXPECT_EQ(x({"export", "box", "box.iso"}), (V{"subos", "export", "box", "--iso", "box.iso"}));
    EXPECT_EQ(x({"export", "box", "box.img", "--size", "8G"}),
              (V{"subos", "export", "box", "--drive", "box.img", "--size", "8G"}));
    EXPECT_EQ(x({"export", "box", "box.qcow2"}), (V{"subos", "export", "box", "--qcow2", "box.qcow2"}));
    EXPECT_EQ(x({"export", "box", "box.tar.zst"}), (V{"subos", "export", "box", "--tar", "box.tar.zst"}));
    EXPECT_EQ(x({"export", "box", "out/"}), (V{"subos", "export", "box", "--rootfs", "out/"}));
    EXPECT_EQ(x({"export", "box", "box.bin", "--format", "img"}), (V{"subos", "export", "box", "--drive", "box.bin"}));
    const auto refused = x({"export", "box", "box.bin"});
    ASSERT_EQ(refused.front(), "ERROR");
    EXPECT_NE(refused[1].find("--format"), std::string::npos) << refused[1];
}

XTEST(LubanCli, AMissingWordIsAUsageErrorThatSaysWhatIsMissing, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    EXPECT_EQ(x({"enter"}).front(), "ERROR");
    EXPECT_EQ(x({"run", "box"}).front(), "ERROR");
    const auto noValue = x({"config", "box", "proxy"});
    ASSERT_EQ(noValue.front(), "ERROR");
    EXPECT_NE(noValue[1].find("luban config box"), std::string::npos) << noValue[1];
    EXPECT_EQ(x({"config", "box", "colour", "red"}).front(), "ERROR");
}

XTEST(LubanCli, LubanIsReleasedAsTheXlingsItDrives, .area = "luban", .covers = {"LUBAN-TOOL-SHIPPED"}) {
    EXPECT_EQ(cli::kVersion, xlings::Info::VERSION)
        << "bump luban/src/cli/cli.cppm kVersion with mcpp.toml and src/core/config.cppm";
}

XTEST(LubanCli, ADriveIsWrittenOnlyWhenItIsAWholeDriveNobodyUses, .area = "luban", .covers = {"LUBAN-WRITE-SAFE"}) {
    auto home = tk::Home::isolated("drives");
    const auto sys = home.root() / "sys";
    const auto block = sys / "class/block";
    auto drive = [&](std::string name, std::string sectors, std::string model, std::string serial) {
        tk::write_file(block / name / "size", sectors + "\n");
        tk::write_file(block / name / "device/model", model + "   \n");
        if (!serial.empty()) tk::write_file(block / name / "device/serial", serial + "\n");
        std::filesystem::create_directories(block / name / "holders");
    };
    drive("sdb", "62914560", "SanDisk Ultra", "4C530001");   // 32 GB stick
    drive("sda", "1953525168", "Samsung SSD", "S4EV");
    tk::write_file(block / "sda/sda2/partition", "2\n");     // its partition
    tk::write_file(block / "sda2/partition", "2\n");
    drive("loop3", "204800", "", "");
    drive("sdc", "100", "held", "X");
    tk::write_file(block / "sdc/holders/dm-0", "");
    const auto mounts = home.root() / "mounts";
    tk::write_file(mounts, "/dev/sda2 / ext4 rw 0 0\nproc /proc proc rw 0 0\n");

    auto stick = cli::inspect_drive("/dev/sdb", sys, mounts);
    EXPECT_TRUE(stick.refused.empty()) << stick.refused;
    EXPECT_EQ(stick.bytes, 62914560ull * 512);
    EXPECT_EQ(stick.model, "SanDisk Ultra");
    EXPECT_EQ(stick.serial, "4C530001");
    EXPECT_NE(cli::inspect_drive("/dev/sda", sys, mounts).refused.find("mounted"), std::string::npos)
        << "the drive this system runs from";
    EXPECT_NE(cli::inspect_drive("/dev/sda2", sys, mounts).refused.find("partition"), std::string::npos);
    EXPECT_NE(cli::inspect_drive("/dev/sdc", sys, mounts).refused.find("held"), std::string::npos);
    EXPECT_NE(cli::inspect_drive("/dev/nvme9n9", sys, mounts).refused.find("not a drive"), std::string::npos);
    EXPECT_NE(cli::inspect_drive("/tmp/sdb", sys, mounts).refused.find("not a drive"), std::string::npos);
    auto loop = cli::inspect_drive("/dev/loop3", sys, mounts);
    EXPECT_TRUE(loop.refused.empty()) << loop.refused;
    EXPECT_EQ(loop.serial, "loop3") << "no serial: its name is what --serial must say";
}
