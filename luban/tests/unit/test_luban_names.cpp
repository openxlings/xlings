// A name while it is made (design 2026-10-21 §7.3, J7): two makings never
// take one name -- two processes, or two of the window's tasks in one.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import luban.service;

namespace svc = luban::service;
namespace tk = xlings::testkit;

XTEST(LubanNames, OneProcessDoesNotTakeOverItsOwnMaking, .area = "luban", .covers = {"LUBAN-NAME-RESERVE"}) {
    auto home = tk::Home::isolated("luban-names");
    auto first = svc::reserve_name(home.dir(), "desk");
    ASSERT_TRUE(first.has_value()) << first.error();
    EXPECT_TRUE(svc::name_reserved(home.dir(), "desk"));
    auto second = svc::reserve_name(home.dir(), "desk");
    ASSERT_FALSE(second.has_value()) << "a second task of the same window took over the first's name";
    EXPECT_NE(second.error().find("is being made"), std::string::npos) << second.error();
    { auto done = std::move(*first); }   // the first making ends
    EXPECT_FALSE(svc::name_reserved(home.dir(), "desk"));
    auto again = svc::reserve_name(home.dir(), "desk");
    EXPECT_TRUE(again.has_value()) << "the name is free once its making ended";
}

XTEST(LubanNames, AReservationOfAProcessThatIsGoneIsTakenOver, .area = "luban", .covers = {"LUBAN-NAME-RESERVE"}) {
    auto home = tk::Home::isolated("luban-names-stale");
    const auto dir = home.dir() / "run" / "luban" / "names";
    std::filesystem::create_directories(dir);
    // The old format: a process id alone, of a process that is no longer.
    tk::write_file(dir / "desk", "999999999\n");
    EXPECT_FALSE(svc::name_reserved(home.dir(), "desk"));
    auto taken = svc::reserve_name(home.dir(), "desk");
    EXPECT_TRUE(taken.has_value()) << (taken ? "" : taken.error());
}

XTEST(LubanNames, WhatACancelledMakingSetAsideIsListedAndRemoved, .area = "luban", .covers = {"LUBAN-CANCEL"}) {
    auto home = tk::Home::isolated("luban-unfinished");
    const auto in_home = home.dir() / "subos" / ".unfinished-desk-4242";
    const auto in_domain = home.dir() / "domains" / "xlings" / "private" / "subos" / ".unfinished-box-7";
    std::filesystem::create_directories(in_home / "root" / "usr");
    std::filesystem::create_directories(in_domain / "home");
    std::filesystem::create_directories(home.dir() / "subos" / "kept");
    const auto left = svc::unfinished(home.dir());
    ASSERT_EQ(left.size(), 2u);
    EXPECT_EQ(left[0].name, "box");
    EXPECT_EQ(left[1].name, "desk");
    EXPECT_EQ(svc::remove_unfinished(home.dir()), 2u);
    EXPECT_FALSE(std::filesystem::exists(in_home));
    EXPECT_FALSE(std::filesystem::exists(in_domain));
    EXPECT_TRUE(std::filesystem::exists(home.dir() / "subos" / "kept")) << "only what was set aside";
}
