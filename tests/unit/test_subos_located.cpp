// Where an instance's files are: a Luban system in a prefix domain has them
// in the domain's private home, seen inside as /xlings (part 3 §3.4).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.home_view;
import xlings.subos.located;

namespace tk = xlings::testkit;

XTEST(SubosLocated, ASystemInAPrefixDomainIsFoundInItsPrivateHome, .area = "subos", .covers = {"DOMAIN-LUBAN-SYSTEM"}) {
    auto home = tk::Home::isolated("located");
    const xlings::subos::HomeView owner{home.dir()};
    const auto physical = home.dir() / "domains/xlings/private";
    tk::write_file(owner.instance_file("box"), std::format(R"({{"kind":"rootfs","prefix_domain":{{"logical_home":"/xlings",
        "owner_home":"{0}","physical_home":"{1}","private_home":true,"schema":1}}}})",
        home.dir().generic_string(), physical.generic_string()));
    const auto at = xlings::subos::locate(owner, "box");
    EXPECT_TRUE(at.domain);
    EXPECT_EQ(at.home.home, physical);
    EXPECT_EQ(at.logical, std::filesystem::path("/xlings"));
    EXPECT_EQ(at.home.instance("box"), physical / "subos/box");

    // Outside a domain, and with a record that cannot be read: where it always was.
    tk::write_file(owner.instance_file("view"), R"({"kind":"view"})");
    tk::write_file(owner.instance_file("broken"), "{not json");
    for (const auto* name : {"view", "broken", "absent"}) {
        const auto here = xlings::subos::locate(owner, name);
        EXPECT_FALSE(here.domain) << name;
        EXPECT_EQ(here.home.home, home.dir()) << name;
        EXPECT_EQ(here.logical, home.dir()) << name;
    }
}

// What a root's links name (/xlings/...) is read from outside through them:
// the desktop a system declares is found from the host (part 4, the Luban OS
// app's desktop of a system in a prefix domain or a Mac's host machine).
XTEST(SubosLocated, ALinkThatNamesTheSystemsHomeIsFollowedIntoWhereThisHostHasIt, .area = "subos",
      .covers = {"DOMAIN-LUBAN-SYSTEM"}, .requires_ = {"linux"}) {
    namespace fs = std::filesystem;
    auto home = tk::Home::isolated("located-physical");
    const auto physical = home.dir() / "domains/xlings/private";
    const xlings::subos::Located at{xlings::subos::HomeView{physical}, "/xlings-located-test", true};
    tk::write_file(physical / "data/xpkgs/x-labwc/1/share/wayland-sessions/labwc.desktop", "[Desktop Entry]\n");
    const auto root = physical / "subos/box/rootfs";
    fs::create_directories(root / "usr/share");
    fs::create_symlink("/xlings-located-test/data/xpkgs/x-labwc/1/share/wayland-sessions", root / "usr/share/wayland-sessions");
    const auto here = xlings::subos::physical(at, root / "usr/share/wayland-sessions");
    EXPECT_EQ(here, (physical / "data/xpkgs/x-labwc/1/share/wayland-sessions").lexically_normal());
    EXPECT_TRUE(fs::exists(here / "labwc.desktop"));
    // A relative link on the way, and a path with no links, are what they are.
    fs::create_symlink("share", root / "usr/share2");
    EXPECT_EQ(xlings::subos::physical(at, root / "usr/share2/wayland-sessions"),
              (physical / "data/xpkgs/x-labwc/1/share/wayland-sessions").lexically_normal());
    const xlings::subos::Located here_only{xlings::subos::HomeView{home.dir()}, home.dir(), false};
    EXPECT_EQ(xlings::subos::physical(here_only, root / "usr/share/wayland-sessions"), root / "usr/share/wayland-sessions");
}
