// The home's format (design §4): which client understands which layout, what
// a switch to an older one is told, and steps that each stay their own
// failure domain.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.home.format;

namespace fmt = xlings::home::format;
namespace fs = std::filesystem;

XTEST(HomeFormat, VersionsCompareAsNumbersPartByPart, .area = "self", .covers = {"HOME-FORMAT-SWITCH-NOTE"}) {
    EXPECT_LT(fmt::compare_versions("0.4.40", "2026.10.9.1"), 0);
    EXPECT_LT(fmt::compare_versions("2026.10.9.2", "2026.10.10.1"), 0) << "10 > 9, not as text";
    EXPECT_EQ(fmt::compare_versions("v2026.10.12.3", "2026.10.12.3"), 0);
    EXPECT_EQ(fmt::compare_versions("2026.10.12", "2026.10.12.0"), 0);
    EXPECT_GT(fmt::compare_versions("2026.10.12.4", "2026.10.12.3"), 0);
}

XTEST(HomeFormat, AnOlderClientIsToldWhatItWillNotSee, .area = "self", .covers = {"HOME-FORMAT-SWITCH-NOTE"}) {
    EXPECT_FALSE(fmt::client_format("0.4.40").gates);
    EXPECT_EQ(fmt::client_format("2026.10.9.1").knows, 2);
    EXPECT_FALSE(fmt::switch_note(fmt::kLayout, "2026.10.12.3")) << "the same layout says nothing";
    auto note = fmt::switch_note(2, "0.4.40");
    ASSERT_TRUE(note);
    EXPECT_FALSE(note->read_only) << "a client that does not read the layout still writes";
    EXPECT_NE(note->text.find("Luban environments"), std::string::npos) << note->text;
    // A client that reads the layout and knows a lower one only reads the home.
    auto gated = fmt::switch_note(fmt::kLayout + 1, "2026.10.12.3");
    ASSERT_TRUE(gated);
    EXPECT_TRUE(gated->read_only);
    EXPECT_NE(gated->text.find("only reads it"), std::string::npos) << gated->text;
}

XTEST(HomeFormat, AFailingStepStopsNoOther, .area = "self", .covers = {"HOME-FORMAT-STEPS"}) {
    auto home = xlings::testkit::Home::isolated("home-format-steps");
    int ran = 0;
    const std::array<fmt::Step, 4> steps{{
        {"throws", nullptr, [](const fs::path&) -> std::expected<std::string, std::string> {
             throw std::runtime_error("boom");
         }},
        {"fails", nullptr, [](const fs::path&) -> std::expected<std::string, std::string> {
             return std::unexpected("no");
         }},
        {"not-needed", [](const fs::path&) { return false; },
         [&](const fs::path&) -> std::expected<std::string, std::string> { ++ran; return "ran"; }},
        {"works", [](const fs::path&) { return true; },
         [&](const fs::path&) -> std::expected<std::string, std::string> { ++ran; return "done"; }},
    }};
    const auto report = fmt::run(home.dir(), steps);
    EXPECT_EQ(ran, 1);
    ASSERT_EQ(report.failed.size(), 2u);
    EXPECT_EQ(report.failed[0].second, "boom");
    ASSERT_EQ(report.done.size(), 1u);
    EXPECT_EQ(report.done[0].first, "works");
    EXPECT_TRUE(fmt::told(fmt::Problem::Decision));
    EXPECT_FALSE(fmt::told(fmt::Problem::Recipe));
}
