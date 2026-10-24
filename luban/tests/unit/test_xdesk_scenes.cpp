// The wallpaper engine's scenes (design 2026-10-24 §9.7): each one, stepped,
// draws a picture that is not one colour, changes with time, answers the
// pointer without failing, and draws the same from the same seed.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xdesk.scenes;

namespace scenes = xdesk::scenes;

XTEST(XdeskScenes, EveryScenePaintsMovesAndIsTheSameFromItsSeed, .area = "luban", .covers = {"XDESK-WALLPAPER"}) {
    ASSERT_GE(scenes::names().size(), 10u);
    EXPECT_EQ(scenes::names().front(), "reaction-diffusion") << "the default first: slow and quiet";
    constexpr int W = 96, H = 54;
    for (const auto name : scenes::names()) {
        SCOPED_TRACE(std::string(name));
        auto a = scenes::make(name, 7), b = scenes::make(name, 7);
        a->resize(W, H);
        b->resize(W, H);
        std::vector<std::uint32_t> pa(W * H), pb(W * H), before(W * H);
        for (int k = 0; k < 20; ++k) { a->step(0.05); b->step(0.05); }
        a->draw(before);
        a->pointer(W / 2.0, H / 2.0, true);
        b->pointer(W / 2.0, H / 2.0, true);
        a->camera(0, 0, 1);
        b->camera(0, 0, 1);
        for (int k = 0; k < 40; ++k) { a->step(0.05); b->step(0.05); }
        a->draw(pa);
        b->draw(pb);
        EXPECT_GT(std::set<std::uint32_t>(pa.begin(), pa.end()).size(), 3u) << "more than a colour";
        EXPECT_NE(pa, before) << "it moves";
        EXPECT_EQ(pa, pb) << "the same from the same seed";
        EXPECT_TRUE(std::ranges::all_of(pa, [](std::uint32_t p) { return (p >> 24) == 0xff; })) << "opaque";
    }
    // An unknown name is the default, not a failure.
    auto d = scenes::make("no-such-scene");
    d->resize(16, 16);
    d->step(0.1);
}
