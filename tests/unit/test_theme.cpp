// Colours addressed by role, and a rename that moves no pixels.
//
// The change these guard: 119 call sites named a COLOUR (`theme::cyan()`),
// 7 named a ROLE. A theme file remapping roles therefore changed almost
// nothing -- the semantic layer existed and the call sites went around it.
//
// Renaming 119 sites is only safe if the default theme is byte-identical to
// the table those sites used to read, so `DefaultThemeMatchesThePaletteItReplaced`
// is the load-bearing test here: it is what turns "a big rename" into "a
// refactor with no user-visible effect".
#include <gtest/gtest.h>

import std;
import xlings.theme;
import xlings.core.palette;

using xlings::theme::Background;
using xlings::theme::LoadIssue;
using xlings::theme::Slot;

namespace {

bool same_(xlings::theme::Rgb a, xlings::palette::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

xlings::theme::Rgb slot_(const xlings::theme::Theme& t, Slot s, Background bg) {
    return bg == Background::Light ? t.light[static_cast<int>(s)]
                                   : t.dark[static_cast<int>(s)];
}

}  // namespace

// ─── The rename moves no pixels ────────────────────────────

TEST(Theme, DefaultThemeMatchesThePaletteItReplaced) {
    const auto& d = xlings::theme::builtin_default();
    namespace pd = xlings::palette::dark_;
    namespace pl = xlings::palette::light_;

    // Role <- the colour that role used to be spelled as. If these ever
    // disagree, somebody changed the look while claiming to rename.
    EXPECT_TRUE(same_(slot_(d, Slot::Accent,  Background::Dark), pd::cyan));
    EXPECT_TRUE(same_(slot_(d, Slot::Alt,     Background::Dark), pd::magenta));
    EXPECT_TRUE(same_(slot_(d, Slot::Success, Background::Dark), pd::green));
    EXPECT_TRUE(same_(slot_(d, Slot::Warn,    Background::Dark), pd::amber));
    EXPECT_TRUE(same_(slot_(d, Slot::Error,   Background::Dark), pd::red));
    EXPECT_TRUE(same_(slot_(d, Slot::Text,    Background::Dark), pd::text));
    EXPECT_TRUE(same_(slot_(d, Slot::Muted,   Background::Dark), pd::dim));
    EXPECT_TRUE(same_(slot_(d, Slot::Border,  Background::Dark), pd::border));
    EXPECT_TRUE(same_(slot_(d, Slot::Surface, Background::Dark), pd::surface));

    EXPECT_TRUE(same_(slot_(d, Slot::Accent,  Background::Light), pl::cyan));
    EXPECT_TRUE(same_(slot_(d, Slot::Alt,     Background::Light), pl::magenta));
    EXPECT_TRUE(same_(slot_(d, Slot::Success, Background::Light), pl::green));
    EXPECT_TRUE(same_(slot_(d, Slot::Warn,    Background::Light), pl::amber));
    EXPECT_TRUE(same_(slot_(d, Slot::Error,   Background::Light), pl::red));
    EXPECT_TRUE(same_(slot_(d, Slot::Text,    Background::Light), pl::text));
    EXPECT_TRUE(same_(slot_(d, Slot::Muted,   Background::Light), pl::dim));
    EXPECT_TRUE(same_(slot_(d, Slot::Border,  Background::Light), pl::border));
    EXPECT_TRUE(same_(slot_(d, Slot::Surface, Background::Light), pl::surface));
}

TEST(Theme, EverySlotHasAName) {
    // A slot with no name cannot be written in a theme file, so it would be
    // themeable in the type system and not in practice.
    for (int i = 0; i < xlings::theme::kSlotCount; ++i) {
        const auto s = static_cast<Slot>(i);
        const auto name = xlings::theme::slot_name(s);
        EXPECT_FALSE(name.empty()) << i;
        EXPECT_EQ(xlings::theme::slot_from_name(name), s) << name;
    }
}

// ─── Partial overlay ───────────────────────────────────────

TEST(Theme, UnmentionedSlotsKeepTheBaseColour) {
    const auto& base = xlings::theme::builtin_default();
    auto r = xlings::theme::load_from_json(
        R"({"name":"mono","dark":{"accent":"#C9D1D9"}})", base);

    ASSERT_TRUE(r.issues.empty());
    EXPECT_EQ(r.theme.name, "mono");
    // The one that was stated moved...
    EXPECT_TRUE(same_(slot_(r.theme, Slot::Accent, Background::Dark),
                      xlings::palette::Rgb{0xC9, 0xD1, 0xD9}));
    // ...and everything else is untouched. This is why there is no `extends`:
    // "not mentioned" already means "inherit".
    EXPECT_TRUE(same_(slot_(r.theme, Slot::Error, Background::Dark),
                      xlings::palette::dark_::red));
}

TEST(Theme, DarkAndLightFallBackIndependently) {
    const auto& base = xlings::theme::builtin_default();
    auto r = xlings::theme::load_from_json(
        R"({"dark":{"accent":"#000000"}})", base);

    ASSERT_TRUE(r.issues.empty());
    // A theme that only states `dark` has said nothing about light terminals.
    // Mirroring the value into `light` would hand the user a scheme its author
    // never looked at.
    EXPECT_TRUE(same_(slot_(r.theme, Slot::Accent, Background::Light),
                      xlings::palette::light_::cyan));
}

TEST(Theme, ShortHexFormExpands) {
    auto r = xlings::theme::load_from_json(
        R"({"dark":{"accent":"#abc"}})", xlings::theme::builtin_default());
    ASSERT_TRUE(r.issues.empty());
    EXPECT_TRUE(same_(slot_(r.theme, Slot::Accent, Background::Dark),
                      xlings::palette::Rgb{0xAA, 0xBB, 0xCC}));
}

// ─── Nothing fails silently ────────────────────────────────

TEST(Theme, UnknownSlotIsReportedWithASuggestion) {
    auto r = xlings::theme::load_from_json(
        R"({"dark":{"acent":"#000000"}})", xlings::theme::builtin_default());

    // Silently ignoring it is indistinguishable from a theme that does not do
    // much -- and the user would never find the typo.
    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_EQ(r.issues[0].kind, LoadIssue::Kind::UnknownSlot);
    EXPECT_EQ(r.issues[0].suggestion, "accent");
}

TEST(Theme, WildlyWrongNameGetsNoSuggestionRatherThanANonsenseOne) {
    auto r = xlings::theme::load_from_json(
        R"({"dark":{"zzzzzzzzzz":"#000000"}})", xlings::theme::builtin_default());
    ASSERT_EQ(r.issues.size(), 1u);
    // A suggestion six edits away teaches the reader to stop reading
    // suggestions. The subos "did you mean" has no threshold and does exactly
    // that.
    EXPECT_TRUE(r.issues[0].suggestion.empty());
}

TEST(Theme, BadColourIsReportedAndLeavesTheSlotAlone) {
    const auto& base = xlings::theme::builtin_default();
    for (auto* bad : { R"({"dark":{"accent":"C9D1D9"}})",     // no '#'
                       R"({"dark":{"accent":"#12345"}})",     // wrong length
                       R"({"dark":{"accent":"#gggggg"}})",    // not hex
                       R"({"dark":{"accent":42}})" }) {       // not a string
        auto r = xlings::theme::load_from_json(bad, base);
        ASSERT_EQ(r.issues.size(), 1u) << bad;
        EXPECT_EQ(r.issues[0].kind, LoadIssue::Kind::BadColor) << bad;
        EXPECT_TRUE(same_(slot_(r.theme, Slot::Accent, Background::Dark),
                          xlings::palette::dark_::cyan)) << bad;
    }
}

TEST(Theme, BadJsonIsReportedAndYieldsAUsableTheme) {
    auto r = xlings::theme::load_from_json("{not json", xlings::theme::builtin_default());
    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_EQ(r.issues[0].kind, LoadIssue::Kind::BadJson);
    // Still complete: the caller renders SOMETHING while reporting the problem.
    EXPECT_TRUE(same_(slot_(r.theme, Slot::Accent, Background::Dark),
                      xlings::palette::dark_::cyan));
}

// ─── The active theme actually reaches the colour lookup ───

TEST(Theme, SettingAThemeChangesWhatColorReturns) {
    const auto saved = xlings::theme::current();
    auto r = xlings::theme::load_from_json(
        R"({"dark":{"accent":"#010203"},"light":{"accent":"#040506"}})",
        xlings::theme::builtin_default());
    xlings::theme::set_current(r.theme);

    // If this failed, `xlings config --theme x` would be another setting that
    // is stored, echoed, and changes nothing.
    auto d = xlings::theme::color(Slot::Accent, Background::Dark);
    auto l = xlings::theme::color(Slot::Accent, Background::Light);
    EXPECT_TRUE(same_(d, xlings::palette::Rgb{1, 2, 3}));
    EXPECT_TRUE(same_(l, xlings::palette::Rgb{4, 5, 6}));

    xlings::theme::set_current(saved);
}
