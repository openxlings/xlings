// Unit tests for the width and output contract in `xlings.ui:layout`.
//
// The bugs these cover all had the same shape: a renderer decided how wide to
// draw from something that was not the terminal width, or measured a string in
// bytes and called the answer columns. Nothing in the output said so — a
// truncated line and a complete one look identical, which is why
// `xlings use gcc` shipped for months silently dropping the `bin` off a
// toolchain path.
#include <gtest/gtest.h>

#include "ftxui/dom/elements.hpp"

#include <cstdlib>
#include <string>
#include <vector>

import std;
import xlings.ui;

using namespace xlings::ui;

namespace {

// XLINGS_TERM_WIDTH is read on every call, so a test can set the width it
// wants without a pty. 0 means "not a terminal" — no clamping, no padding.
struct TermWidth {
    explicit TermWidth(const char* v) {
#if defined(_WIN32)
        _putenv_s("XLINGS_TERM_WIDTH", v);
#else
        ::setenv("XLINGS_TERM_WIDTH", v, 1);
#endif
    }
    ~TermWidth() {
#if defined(_WIN32)
        _putenv_s("XLINGS_TERM_WIDTH", "");
#else
        ::unsetenv("XLINGS_TERM_WIDTH");
#endif
    }
};

const std::string kHan = "\xe4\xb8\xad\xe6\x96\x87";      // 中文 — 6 bytes, 4 columns
const std::string kEllipsis = "\xe2\x80\xa6";              // …

// True when every byte of `s` belongs to a complete UTF-8 sequence.
bool well_formed_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        if ((c & 0x80U) == 0x00U)      n = 1;
        else if ((c & 0xE0U) == 0xC0U) n = 2;
        else if ((c & 0xF0U) == 0xE0U) n = 3;
        else if ((c & 0xF8U) == 0xF0U) n = 4;
        else return false;                       // stray continuation byte
        if (i + n > s.size()) return false;      // truncated sequence
        for (std::size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0U) != 0x80U) return false;
        }
        i += n;
    }
    return true;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto nl = s.find('\n', start);
        out.push_back(s.substr(start, (nl == std::string::npos ? s.size() : nl) - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

}  // namespace

// ─── display width ────────────────────────────────────────────────

TEST(UiLayoutWidth, AsciiWidthIsLength) {
    EXPECT_EQ(layout::display_width("gcc@15.1.0"), 10);
    EXPECT_EQ(layout::display_width(""), 0);
}

TEST(UiLayoutWidth, WideGlyphsCountTwoColumnsNotThreeBytes) {
    EXPECT_EQ(kHan.size(), 6u);
    EXPECT_EQ(layout::display_width(kHan), 4);
}

TEST(UiLayoutWidth, MaxLineWidthTakesTheWidestLine) {
    EXPECT_EQ(layout::max_line_width("ab\nabcd\na"), 4);
    EXPECT_EQ(layout::max_line_width("solo"), 4);
}

// ─── padding ──────────────────────────────────────────────────────

TEST(UiLayoutPad, PadsToColumnsNotBytes) {
    EXPECT_EQ(layout::display_width(layout::pad_to_width("ab", 6)), 6);
    // The byte-based idiom this replaces padded 中文 by 6 and produced a
    // 10-column cell where a 6-column one was asked for.
    EXPECT_EQ(layout::display_width(layout::pad_to_width(kHan, 6)), 6);
}

TEST(UiLayoutPad, NeverShrinks) {
    EXPECT_EQ(layout::pad_to_width("abcdef", 3), "abcdef");
}

// ─── truncation ───────────────────────────────────────────────────

TEST(UiLayoutTruncate, LeavesAFittingStringAlone) {
    EXPECT_EQ(layout::truncate_to_width("abcdef", 6), "abcdef");
    EXPECT_EQ(layout::truncate_to_width("abc", 10), "abc");
}

TEST(UiLayoutTruncate, MarksTheCut) {
    auto out = layout::truncate_to_width("abcdefgh", 5);
    EXPECT_EQ(layout::display_width(out), 5);
    EXPECT_TRUE(out.ends_with(kEllipsis));
}

TEST(UiLayoutTruncate, NeverExceedsTheBudgetOnWideGlyphs) {
    // 中文中文 is 8 columns; asking for 5 must not emit 6 by keeping a
    // double-width glyph that only half fits.
    auto out = layout::truncate_to_width(kHan + kHan, 5);
    EXPECT_LE(layout::display_width(out), 5);
    EXPECT_TRUE(out.ends_with(kEllipsis));
}

TEST(UiLayoutTruncate, NeverSplitsAUtf8Sequence) {
    // Every cut, at every budget, has to land on a sequence boundary — a
    // half-written glyph is a mojibake byte in the middle of the output.
    for (int budget = 0; budget <= 10; ++budget) {
        auto out = layout::truncate_to_width(kHan + kHan, budget);
        EXPECT_TRUE(well_formed_utf8(out)) << "budget " << budget;
        EXPECT_LE(layout::display_width(out), std::max(budget, 0));
    }
}

TEST(UiLayoutTruncate, DegradesToABareMarker) {
    EXPECT_EQ(layout::truncate_to_width("abcdef", 1), kEllipsis);
    EXPECT_EQ(layout::truncate_to_width("abcdef", 0), "");
}

// ─── glyph splitting (progress bar) ───────────────────────────────

TEST(UiLayoutSplit, SplitsOnGlyphBoundaries) {
    EXPECT_EQ(layout::split_at_width("abcdef", 3), 3u);
    // Halfway through 中文 in columns is a whole glyph in bytes, never 3.
    EXPECT_EQ(layout::split_at_width(kHan, 2), 3u);
    EXPECT_EQ(layout::split_at_width(kHan, 1), 0u);
    EXPECT_EQ(layout::split_at_width(kHan, 0), 0u);
}

// ─── wrapping ─────────────────────────────────────────────────────

TEST(UiLayoutWrap, BreaksOnSpaces) {
    auto lines = layout::wrap_to_width("one two three four", 9);
    ASSERT_GE(lines.size(), 2u);
    for (auto& l : lines) EXPECT_LE(layout::display_width(l), 9);
    // Nothing is lost: joining the pieces back gives the original words.
    std::string joined;
    for (auto& l : lines) { if (!joined.empty()) joined += ' '; joined += l; }
    EXPECT_EQ(joined, "one two three four");
}

TEST(UiLayoutWrap, HardSplitsATokenWiderThanTheLine) {
    auto lines = layout::wrap_to_width("aaaaaaaaaaaaaaa", 5);
    ASSERT_EQ(lines.size(), 3u);
    for (auto& l : lines) EXPECT_LE(layout::display_width(l), 5);
    EXPECT_EQ(lines[0] + lines[1] + lines[2], "aaaaaaaaaaaaaaa");
}

TEST(UiLayoutWrap, PrefersToBreakAPathAfterASeparator) {
    auto lines = layout::wrap_to_width("@xlings/data/xpkgs/xim-x-gcc/16.1.0/bin", 20);
    ASSERT_GE(lines.size(), 2u);
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
        EXPECT_TRUE(lines[i].ends_with("/")) << "line " << i << ": " << lines[i];
    }
    std::string joined;
    for (auto& l : lines) joined += l;
    EXPECT_EQ(joined, "@xlings/data/xpkgs/xim-x-gcc/16.1.0/bin");
}

TEST(UiLayoutWrap, HonorsEmbeddedNewlines) {
    auto lines = layout::wrap_to_width("first\nsecond", 40);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "first");
    EXPECT_EQ(lines[1], "second");
}

TEST(UiLayoutWrap, LosesNothingOnWideGlyphs) {
    auto lines = layout::wrap_to_width(kHan + kHan + kHan, 5);
    std::string joined;
    for (auto& l : lines) joined += l;
    EXPECT_EQ(joined, kHan + kHan + kHan);
    for (auto& l : lines) EXPECT_LE(layout::display_width(l), 5);
}

// ─── terminal width ───────────────────────────────────────────────

TEST(UiLayoutTermWidth, EnvOverrideWins) {
    TermWidth w("57");
    ASSERT_TRUE(layout::term_width().has_value());
    EXPECT_EQ(*layout::term_width(), 57);
}

TEST(UiLayoutTermWidth, ZeroMeansUnlimited) {
    TermWidth w("0");
    EXPECT_FALSE(layout::term_width().has_value());
}

TEST(UiLayoutFit, ClampsToTheTerminalAndNeverPast) {
    TermWidth w("60");
    EXPECT_EQ(layout::fit_width(200), 60);
    EXPECT_EQ(layout::fit_width(30), 30);
    // The old formula was max(term, natural), which is what produced
    // 92-column lines in a 60-column terminal.
    EXPECT_LE(layout::fit_width(1000), 60);
}

TEST(UiLayoutFit, KeepsAFloorSoOutputStaysReadable) {
    TermWidth w("5");
    EXPECT_GE(layout::fit_width(80), layout::kMinCanvas);
}

TEST(UiLayoutFit, OffATerminalTheNaturalWidthStands) {
    TermWidth w("0");
    EXPECT_EQ(layout::fit_width(200), 200);
}

// ─── column planning ──────────────────────────────────────────────

TEST(UiLayoutColumns, IdentifierColumnKeepsItsFullWidth) {
    TermWidth w("60");
    auto P = layout::plan_two_column(/*markerW=*/4, /*gap=*/2,
                                     /*nameMax=*/33, /*descMax=*/80);
    EXPECT_EQ(P.nameW, 33);          // never shrunk to make room for prose
    EXPECT_FALSE(P.stacked);
    EXPECT_EQ(P.width, 60);
    EXPECT_EQ(P.lead + P.descW, 60);
}

TEST(UiLayoutColumns, DescriptionIsDroppedRatherThanTheName) {
    TermWidth w("40");
    auto P = layout::plan_two_column(4, 2, /*nameMax=*/33, /*descMax=*/80);
    EXPECT_EQ(P.nameW, 33);
    EXPECT_EQ(P.descW, 0);
}

TEST(UiLayoutColumns, StacksWhenTheIdentifiersAloneDoNotFit) {
    TermWidth w("30");
    auto P = layout::plan_two_column(4, 2, /*nameMax=*/40, /*descMax=*/20);
    EXPECT_TRUE(P.stacked);
    EXPECT_LE(P.width, 30);
}

TEST(UiLayoutColumns, NarrowContentDoesNotStretchToTheTerminal) {
    TermWidth w("200");
    auto P = layout::plan_two_column(4, 2, /*nameMax=*/10, /*descMax=*/20);
    EXPECT_EQ(P.width, 4 + 10 + 2 + 20);
}

// ─── rendered output ──────────────────────────────────────────────

TEST(UiLayoutRender, EmitsNoNulNoCrAndNoTrailingSpaces) {
    TermWidth w("40");
    auto doc = ftxui::vbox({
        ftxui::text("short"),
        ftxui::text("a rather longer line here"),
    });
    auto out = layout::render_to_string(doc, 40);

    EXPECT_EQ(out.find('\0'), std::string::npos);   // Screen::Print() wrote one
    EXPECT_EQ(out.find('\r'), std::string::npos);   // ToString() joins with \r\n
    for (auto& s : split_lines(out)) {
        if (s.empty()) continue;
        EXPECT_FALSE(s.ends_with(" ")) << "padded line: [" << s << "]";
    }
}

TEST(UiLayoutRender, NoLineExceedsTheCanvas) {
    TermWidth w("32");
    auto doc = ftxui::vbox({
        ftxui::text("0123456789012345678901234567890123456789"),
        ftxui::text("short"),
    });
    auto out = layout::render_to_string(doc, layout::fit_width(120));
    for (auto& s : split_lines(out)) {
        EXPECT_LE(layout::display_width(s), 32) << "[" << s << "]";
    }
}
