// openwx (luban/specs/openwx/SPEC.md): WuXian's model -- the camera's
// transforms, the order of hits, numbers, where a new window goes -- and its
// state, wuxian.json version 1, read, written and checked.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import openwx;

namespace wx = openwx;

XTEST(Openwx, AWorkspaceIsItsContentTimesItsScaleAndTheCameraMapsBothWays, .area = "luban", .covers = {"WUXIAN-MODEL"}) {
    const wx::Workspace w{.id = 2, .origin = {100, -50}, .scale = 0.5};
    EXPECT_EQ(wx::world_rect(w, {1200, 800}), (wx::Rect{100, -50, 600, 400}));
    EXPECT_EQ(wx::content_to_world(w, {200, 100}), (wx::Point{200, 0}));
    EXPECT_EQ(wx::world_to_content(w, {200, 0}), (wx::Point{200, 100}));
    const wx::Camera c{.at = {1000, 500}, .zoom = 0.25};
    const auto s = wx::to_screen(c, wx::Point{1400, 900}, {0, 30});
    EXPECT_EQ(s, (wx::Point{100, 130}));
    EXPECT_EQ(wx::to_world(c, s, {0, 30}), (wx::Point{1400, 900}));
    EXPECT_EQ(wx::to_screen(c, wx::Rect{1000, 500, 400, 400}), (wx::Rect{0, 0, 100, 100}));
    // Framed: the rect whole and centred; the zoom within the limits.
    const auto f = wx::framing({0, 0, 2400, 1600}, {1200, 800});
    EXPECT_DOUBLE_EQ(f.zoom, 0.5);
    EXPECT_EQ(wx::to_screen(f, wx::Point{1200, 800}), (wx::Point{600, 400}));
    EXPECT_DOUBLE_EQ(wx::framing({0, 0, 1e9, 1e9}, {1200, 800}).zoom, wx::kMinZoom);
    EXPECT_DOUBLE_EQ(wx::framing({0, 0, 1, 1}, {1200, 800}).zoom, wx::kMaxZoom);
    // Zoomed about a point: the world point under it stays under it.
    const auto z = wx::zoomed(c, 2, {300, 200});
    EXPECT_DOUBLE_EQ(z.zoom, 0.5);
    EXPECT_EQ(wx::to_world(z, {300, 200}), wx::to_world(c, {300, 200}));
}

XTEST(Openwx, WhatIsHitIsPinnedThenTheHighestLayerThenThePlane, .area = "luban", .covers = {"WUXIAN-MODEL"}) {
    using K = wx::Layer::Kind;
    const std::vector<wx::Layer> layers{
        {.kind = K::workspace, .workspace = 1, .world = {0, 0, 1000, 1000}, .z = 1},
        {.kind = K::workspace, .workspace = 2, .world = {500, 500, 1000, 1000}, .z = 3},
        {.kind = K::window, .window = 9, .world = {600, 600, 100, 100}, .z = 2},
        {.kind = K::window, .window = 7, .world = {900, 900, 50, 50}, .z = 0, .pinned = true},
    };
    EXPECT_EQ(wx::hit(layers, {100, 100})->workspace, 1);
    EXPECT_EQ(wx::hit(layers, {650, 650})->workspace, 2) << "workspace 2 is above the free window";
    EXPECT_EQ(wx::hit(layers, {920, 920})->window, 7u) << "a pinned window is above everything";
    EXPECT_FALSE(wx::hit(layers, {-10, -10})) << "the plane";
    EXPECT_EQ(wx::top_z(layers), 4);
    // Of two at one z, the later.
    const std::vector<wx::Layer> tie{{.workspace = 1, .world = {0, 0, 10, 10}}, {.workspace = 2, .world = {0, 0, 10, 10}}};
    EXPECT_EQ(wx::hit(tie, {5, 5})->workspace, 2);
}

XTEST(Openwx, ANewWorkspaceTakesTheSmallestFreeNumberAndANewWindowGoesWhereTheViewIs, .area = "luban", .covers = {"WUXIAN-MODEL"}) {
    const std::vector<wx::Workspace> ws{{.id = 1}, {.id = 3}};
    EXPECT_EQ(wx::next_id(ws), 2);
    const std::vector<wx::Workspace> full{{.id = 1}, {.id = 2}};
    EXPECT_FALSE(wx::next_id(full, 2)) << "every number within the limit taken";
    EXPECT_EQ(wx::next_id(full), 3);
    EXPECT_NE(wx::color_for(1), wx::color_for(2));
    EXPECT_EQ(wx::color_for(1), wx::color_for(10)) << "the colours go round";
    const std::vector<wx::Layer> layers{{.workspace = 4, .world = {0, 0, 100, 100}}};
    EXPECT_EQ(wx::new_window(wx::View::workspace, 2, layers, {50, 50}), (wx::Destination{.workspace = 2}));
    EXPECT_EQ(wx::new_window(wx::View::wuxian, 2, layers, {50, 50}), (wx::Destination{.workspace = 4}));
    EXPECT_EQ(wx::new_window(wx::View::wuxian, 2, layers, {500, 500}), (wx::Destination{.plane = true}));
    for (const auto r : {wx::Rule::tile, wx::Rule::stack}) EXPECT_EQ(wx::rule_named(wx::name(r)), r);
    for (const auto v : {wx::View::workspace, wx::View::wuxian}) EXPECT_EQ(wx::view_named(wx::name(v)), v);
    EXPECT_FALSE(wx::rule_named("canvas"));
}

XTEST(Openwx, TheStateReadsBackAsItWasWrittenItsLayoutUntouched, .area = "luban", .covers = {"WUXIAN-STATE"}) {
    wx::State s;
    s.view = wx::View::wuxian;
    s.current = 2;
    s.camera = {.at = {-120.5, 40}, .zoom = 0.5};
    s.workspaces = {{.id = 2, .name = "编码 \"x\"", .color = 0x5e9cff, .origin = {0, 0}, .scale = 1, .z = 3},
                    {.id = 4, .name = "", .color = 0x3fbfb0, .rule = wx::Rule::stack, .flow = wx::Flow::vertical,
                     .origin = {1800, -20}, .scale = 0.75, .z = 1}};
    s.layouts[2] = R"({"columns": [[0, 1], [2]], "widths": [0.5, 0]})";
    s.windows = {{.app_id = "foot", .title = "build\tlog", .in = 2, .flags = {"floating", "made-up"}},
                 {.app_id = "org.kde.falkon", .free = wx::Rect{1800, 200, 1200, 800}, .z = 4}};
    EXPECT_TRUE(wx::validate(s).empty()) << wx::validate(s).front();
    const auto text = wx::format(s);
    auto back = wx::parse(text);
    ASSERT_TRUE(back) << back.error() << "\n" << text;
    EXPECT_EQ(*back, s) << text;
    EXPECT_EQ(back->layouts.at(2), s.layouts.at(2)) << "the implementation's layout, as it was written";
    // What another writer adds is read past; what is left out takes its default.
    auto other = wx::parse(R"({"wuxian": 1, "later": {"x": [1, 2]}, "workspaces": [{"id": 1, "extra": true}],
                               "windows": [{"match": {"app_id": "aé😀"}, "in": 1}]})");
    ASSERT_TRUE(other) << other.error();
    EXPECT_EQ(other->workspaces.front().rule, wx::Rule::tile);
    EXPECT_EQ(other->workspaces.front().color, wx::color_for(1));
    EXPECT_EQ(other->windows.front().app_id, "aé\U0001F600");
    EXPECT_TRUE(wx::validate(*other).empty());
}

XTEST(Openwx, AStateThatIsNotThisVersionOrNotValidIsSaidSoNeverEmpty, .area = "luban", .covers = {"WUXIAN-STATE"}) {
    const auto newer = wx::parse(R"({"wuxian": 2, "workspaces": []})");
    ASSERT_FALSE(newer);
    EXPECT_NE(newer.error().find("version 2"), std::string::npos) << newer.error();
    for (const auto* bad : {"", "[]", "{\"view\": \"workspace\"}", "{\"wuxian\": 1, \"workspaces\": [{\"name\": \"x\"}]}",
                            "{\"wuxian\": 1, \"workspaces\": [{\"id\": 1, \"rule\": \"canvas\"}]}",
                            "{\"wuxian\": 1, \"workspaces\": [{\"id\": 1, \"color\": \"blue\"}]}",
                            "{\"wuxian\": 1} trailing", "{\"wuxian\": 1, \"view\": \"wuxian\""})
        EXPECT_FALSE(wx::parse(bad)) << bad;
    wx::State s;
    s.workspaces = {{.id = 1}, {.id = 1, .scale = 3}, {.id = 0}};
    s.current = 7;
    s.camera.zoom = 10;
    s.windows = {{.app_id = "both", .in = 1, .free = wx::Rect{0, 0, 1, 1}}, {.app_id = "nowhere-in", .in = 5}};
    const auto problems = wx::validate(s);
    auto said = [&](std::string_view part) {
        return std::ranges::any_of(problems, [&](const std::string& p) { return p.find(part) != std::string::npos; });
    };
    EXPECT_TRUE(said("used twice"));
    EXPECT_TRUE(said("scale 3"));
    EXPECT_TRUE(said("positive"));
    EXPECT_TRUE(said("current workspace 7"));
    EXPECT_TRUE(said("camera zoom"));
    EXPECT_TRUE(said("one of them"));
    EXPECT_TRUE(said("workspace 5, which is not there"));
    EXPECT_FALSE(wx::validate(wx::State{.workspaces = {}}).empty()) << "a plane has a workspace";
}
