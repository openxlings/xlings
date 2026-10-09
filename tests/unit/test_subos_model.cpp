// xlings.subos.model (C4): how a typed name resolves to an instance. The
// rules moved out of src/core/subos.cpp unchanged; these pin them on names
// alone, without a home.
#include <gtest/gtest.h>

import std;
import xlings.subos.model;

namespace m = xlings::subos::model;

namespace {
const std::vector<std::string> kNames{"Dev", "default", "dev-gpu", "py311", "web"};
}

TEST(SubosModel, ExactWinsOverEverythingElse) {
    auto r = m::resolve_name("default", kNames);
    EXPECT_EQ(r.selected, "default");
    EXPECT_EQ(r.reason, "exact");
    EXPECT_FALSE(r.autoSelected);
}

TEST(SubosModel, UniqueCaseInsensitiveMatchIsAutoSelected) {
    auto r = m::resolve_name("dev", kNames);
    EXPECT_EQ(r.selected, "Dev");
    EXPECT_EQ(r.reason, "case_insensitive_exact");
    EXPECT_TRUE(r.autoSelected);
}

TEST(SubosModel, UniquePrefixIsAutoSelectedAndSharedPrefixIsAmbiguous) {
    auto r = m::resolve_name("py", kNames);
    EXPECT_EQ(r.selected, "py311");
    EXPECT_EQ(r.reason, "unique_prefix");

    auto amb = m::resolve_name("de", kNames);
    EXPECT_TRUE(amb.selected.empty());
    EXPECT_EQ(amb.reason, "ambiguous");
    EXPECT_EQ(amb.matches, (std::vector<std::string>{"Dev", "default", "dev-gpu"}));
}

TEST(SubosModel, NotFoundSuggestsRelatedNamesFirst) {
    auto r = m::resolve_name("gpu", kNames);
    EXPECT_EQ(r.reason, "not_found");
    ASSERT_FALSE(r.matches.empty());
    EXPECT_EQ(r.matches.front(), "dev-gpu");
    EXPECT_LE(r.matches.size(), 3u);
}

TEST(SubosModel, MissingNameListsEverything) {
    auto r = m::resolve_name("", kNames);
    EXPECT_EQ(r.reason, "missing_name");
    EXPECT_EQ(r.matches, kNames);
}

TEST(SubosModel, EditDistance) {
    EXPECT_EQ(m::edit_distance("kitten", "sitting"), 3u);
    EXPECT_EQ(m::edit_distance("", "abc"), 3u);
    EXPECT_EQ(m::edit_distance("same", "same"), 0u);
}
