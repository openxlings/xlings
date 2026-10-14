// luban (Luban OS architecture design §6): the command line, edition names,
// image formats, machine settings, the archive and the version.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import luban.cli;
import luban.service;
import luban.service.archive;
import xlings.libs.json;
import xlings.core.config;

namespace cli = luban::cli;
namespace svc = luban::service;
namespace tk = xlings::testkit;
using V = std::vector<std::string>;

XTEST(LubanCli, ACommandLineIsItsWordsItsOptionsAndWhatFollowsTheDoubleDash, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    auto p = cli::parse(V{"new", "box", "--edition", "tiny", "--proxy=socks5h://127.0.0.1:1080"});
    ASSERT_TRUE(p) << p.error();
    EXPECT_EQ(p->command, "new");
    EXPECT_EQ(p->words, (V{"box"}));
    EXPECT_EQ(p->option("--edition"), "tiny");
    EXPECT_EQ(p->option("--proxy"), "socks5h://127.0.0.1:1080");
    auto r = cli::parse(V{"run", "box", "--", "ls", "-l"});
    ASSERT_TRUE(r);
    EXPECT_EQ(r->after, (V{"ls", "-l"}));
    auto e = cli::parse(V{"enter", "box", "--mode", "machine", "--temp"});
    ASSERT_TRUE(e);
    EXPECT_EQ(e->option("--mode"), "machine");
    EXPECT_TRUE(e->flag("--temp"));
    // An edition is an option, not a second word; an unknown option is refused.
    auto unknown = cli::parse(V{"new", "box", "--from", "tiny"});
    ASSERT_FALSE(unknown);
    EXPECT_NE(unknown.error().find("--from"), std::string::npos) << unknown.error();
    EXPECT_FALSE(cli::parse(V{"config", "box", "--mode"})) << "a value is missing";
}

XTEST(LubanCli, EditionsAndFormatsAreNamedTheUsersWay, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    EXPECT_EQ(svc::edition_ref("tiny"), "subos:luban-tiny");
    EXPECT_EQ(svc::edition_ref("tiny@2026.10.20.1"), "subos:luban-tiny@2026.10.20.1");
    EXPECT_EQ(svc::edition_ref("desktop"), "subos:luban-desktop");
    EXPECT_EQ(svc::edition_ref("luban-x"), "subos:luban-x");
    EXPECT_EQ(svc::edition_ref("acme:my-os"), "acme:my-os");
    EXPECT_EQ(svc::export_format("box.iso"), "iso");
    EXPECT_EQ(svc::export_format("box.img"), "img");
    EXPECT_EQ(svc::export_format("box.qcow2"), "qcow2");
    EXPECT_EQ(svc::export_format("box.tar.zst"), "tar");
    EXPECT_EQ(svc::export_format("box.luban"), "luban");
    EXPECT_EQ(svc::export_format("out/"), "dir");
    EXPECT_EQ(svc::export_format("box.bin"), "");
}

XTEST(LubanCli, MachineSettingsAreCheckedAndEveryOtherKeySurvives, .area = "luban", .covers = {"LUBAN-SETTINGS"}) {
    auto home = tk::Home::isolated("luban-settings");
    auto defaults = svc::read_settings(home.dir(), "box");
    ASSERT_TRUE(defaults);
    EXPECT_EQ(defaults->mode, "sandbox");
    EXPECT_EQ(defaults->persist, "all");
    EXPECT_EQ(defaults->on_close, "ask");
    ASSERT_TRUE(svc::set_setting(home.dir(), "box", "mode", "machine"));
    ASSERT_TRUE(svc::set_setting(home.dir(), "box", "on-close", "background"));
    EXPECT_FALSE(svc::set_setting(home.dir(), "box", "persist", "some")) << "not a value it takes";
    EXPECT_FALSE(svc::set_setting(home.dir(), "box", "colour", "red"));
    auto doc = nlohmann::json::parse(tk::read_file(svc::settings_file(home.dir(), "box")));
    doc["future"] = "kept";
    tk::write_file(svc::settings_file(home.dir(), "box"), doc.dump());
    ASSERT_TRUE(svc::set_setting(home.dir(), "box", "persist", "home"));
    auto now = svc::read_settings(home.dir(), "box");
    ASSERT_TRUE(now);
    EXPECT_EQ(now->mode, "machine");
    EXPECT_EQ(now->persist, "home");
    EXPECT_EQ(now->on_close, "background");
    EXPECT_EQ(now->document.value("future", ""), "kept");
    tk::write_file(svc::settings_file(home.dir(), "box"), "{not json");
    EXPECT_FALSE(svc::read_settings(home.dir(), "box")) << "could not read is not the defaults";
}

XTEST(LubanCli, AnEditionsDefaultsAreBehindWhatTheUserSet, .area = "luban", .covers = {"LUBAN-SETTINGS"}) {
    auto home = tk::Home::isolated("luban-settings-edition");
    // The root an edition made: its /etc, and its defaults in /usr/share/luban.
    const auto root = home.dir() / "subos" / "desk" / "rootfs";
    tk::write_file(root / "etc/hostname", "desk\n");
    tk::write_file(root / "usr/share/luban/settings.json",
                   R"({"mode": "machine", "desktop": "labwc-desktop", "persist": "sometimes"})");
    auto s = svc::read_settings(home.dir(), "desk");
    ASSERT_TRUE(s);
    EXPECT_EQ(s->mode, "machine");
    EXPECT_EQ(s->desktop, "labwc-desktop");
    EXPECT_EQ(s->persist, "all") << "a value the key does not take is not a default";
    ASSERT_TRUE(svc::set_setting(home.dir(), "desk", "mode", "sandbox"));
    EXPECT_EQ(svc::read_settings(home.dir(), "desk")->mode, "sandbox") << "what the user set wins";
    EXPECT_EQ(svc::read_settings(home.dir(), "desk")->desktop, "labwc-desktop");
    ASSERT_TRUE(svc::set_setting(home.dir(), "desk", "desktop", "none"));
    EXPECT_EQ(svc::read_settings(home.dir(), "desk")->desktop, "none");
    // An unreadable defaults file is no defaults (the user's file is what must be read).
    tk::write_file(root / "usr/share/luban/settings.json", "{oops");
    auto again = svc::read_settings(home.dir(), "desk");
    ASSERT_TRUE(again);
    EXPECT_EQ(again->mode, "sandbox");
}

XTEST(LubanCli, AnArchiveCarriesTheManifestAndTheRootsStateAndNothingThatLeaves, .area = "luban", .covers = {"LUBAN-ARCHIVE"}) {
    namespace archive = luban::service::archive;
    auto home = tk::Home::isolated("luban-archive");
    const auto root = home.root() / "root";
    tk::write_file(root / "etc/hostname", "box\n");
    tk::write_file(root / "home/user/a-very-long-name-that-needs-a-pax-record-because-it-is-longer-than-one-hundred-bytes-in-all.txt", "x");
    std::filesystem::create_directories(root / "var/empty");
    std::filesystem::create_symlink("/usr/share/factory/etc/os-release", root / "etc/os-release");
    tk::write_file(root / "usr/bin/sh", "not state");
    const auto file = home.root() / "box.luban";
    auto written = archive::write(file, {{"format", std::string(archive::kFormat)}, {"version", archive::kVersion}, {"name", "box"}}, root);
    ASSERT_TRUE(written) << written.error();
    EXPECT_FALSE(archive::write(file, {}, root)) << "an existing file is not overwritten";
    const auto into = home.root() / "into";
    auto placed = archive::extract_state(file, into);
    ASSERT_TRUE(placed) << placed.error();
    EXPECT_EQ(tk::read_file(into / "etc/hostname"), "box\n");
    EXPECT_EQ(std::filesystem::read_symlink(into / "etc/os-release"), "/usr/share/factory/etc/os-release");
    EXPECT_TRUE(std::filesystem::exists(into / "home/user/a-very-long-name-that-needs-a-pax-record-because-it-is-longer-than-one-hundred-bytes-in-all.txt"));
    EXPECT_TRUE(std::filesystem::is_directory(into / "var/empty"));
    EXPECT_FALSE(std::filesystem::exists(into / "usr")) << "/usr is not the root's state";
    auto manifest = archive::read(file);
    ASSERT_TRUE(manifest);
    EXPECT_EQ(manifest->value("name", ""), "box");
    tk::write_file(home.root() / "plain.tar", std::string(1024, '\0'));
    EXPECT_FALSE(archive::read(home.root() / "plain.tar")) << "no manifest, not an archive";
}

XTEST(LubanCli, LubanIsReleasedAsTheXlingsItDrives, .area = "luban", .covers = {"LUBAN-TOOL-SHIPPED"}) {
    EXPECT_EQ(cli::kVersion, xlings::Info::VERSION)
        << "bump luban/os/cli/src/cli.cppm kVersion with mcpp.toml and src/core/config.cppm";
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
