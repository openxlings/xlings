// What Luban keeps for released older parts (design 2026-10-19 §15.3): each
// entry's old behaviour, and the entries the compatibility spec lists.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import luban.compat;

namespace tk = xlings::testkit;
namespace compat = luban::compat;

XTEST(LubanCompat, AFrozenEditionNameMovesToItsSuccessor, .area = "luban", .covers = {"LUBAN-UPGRADE-REVERSIBLE"}) {
    EXPECT_TRUE(compat::frozen_edition("subos:luban-desktop@2026.10.15.1"));
    EXPECT_FALSE(compat::frozen_edition("luban:desktop@2026.10.18.1"));
    EXPECT_EQ(compat::without_frozen_prefix("subos:luban-core@1"), "core@1");
    EXPECT_EQ(compat::without_frozen_prefix("luban:core"), "luban:core") << "anything else unchanged";
    EXPECT_EQ(compat::successor("subos:luban-desktop@2026.10.15.1"), "luban:desktop");
    EXPECT_EQ(compat::successor("luban:desktop"), std::nullopt);
    EXPECT_EQ(compat::frozen_ref("core", "@2026.10.16.2"), "subos:luban-core@2026.10.16.2");
    EXPECT_TRUE(compat::frozen_package("subos", "luban-core", "core"));
    EXPECT_FALSE(compat::frozen_package("subos", "luban-core", "tiny"));
}

XTEST(LubanCompat, AnOldXdeskSessionGetsTheHostsArrow, .area = "luban", .covers = {"LUBAN-OS-POINTER"}) {
    EXPECT_TRUE(compat::legacy_xdesk_pointer("xdesk-classic", false));
    EXPECT_TRUE(compat::legacy_xdesk_pointer("xdesk-canvas", false));
    EXPECT_FALSE(compat::legacy_xdesk_pointer("xdesk-classic", true)) << "a new xdesk asks: the host draws its shapes";
    EXPECT_FALSE(compat::legacy_xdesk_pointer("labwc", false)) << "an old labwc draws its own: no second pointer";
}

XTEST(LubanCompat, OldLubanOsWindowsAreFoundNotEnded, .area = "luban", .covers = {"LUBAN-OS-TRAY-PROCESS"}) {
    const auto none = compat::unregistered_luban_os({});
    EXPECT_EQ(none.looked, xlings::platform::can_list_processes());
    if (!none.looked) GTEST_SKIP() << "this system's processes cannot be listed";
    // This test process is not luban-os: whatever is found, it is not us.
    for (const auto& w : none.found) EXPECT_NE(w.pid, xlings::platform::current_pid());
    std::vector<int> all;
    for (const auto& w : none.found) all.push_back(w.pid);
    EXPECT_TRUE(compat::unregistered_luban_os(all).found.empty()) << "a registered window is not an old one";
}

XTEST(LubanCompat, EveryEntryIsListedWhereItIsDocumented, .area = "luban", .covers = {"LUBAN-UPGRADE-REVERSIBLE"}) {
    const auto spec = tk::read_file(std::filesystem::current_path() / "docs/specs/upgrade-and-compatibility.md");
    const auto readme = tk::read_file(std::filesystem::current_path() / "luban/os/compat/README.md");
    for (const auto& e : compat::kEntries) {
        EXPECT_NE(spec.find(std::format("`{}`", e.name)), std::string::npos) << e.name;
        EXPECT_NE(readme.find(std::format("`{}`", e.name)), std::string::npos) << e.name;
        EXPECT_FALSE(e.remove_when.empty()) << e.name << ": an entry says when it goes";
    }
}
