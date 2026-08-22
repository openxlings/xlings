// One problem, one severity marker.
//
// The defect these guard against is structural, not cosmetic: `log::` has four
// levels and all four answer "how bad is this", so a call site with a detail to
// add could only emit another line at the same level. Measured before this
// module existed: 74 of 313 error/warn literals (24%) were indented
// continuations, each paying for its own `[error]`/`[warn]` marker.
//
// `render()` therefore produces ONE string with embedded newlines, and the
// tests below assert that shape directly rather than capturing a log stream --
// the logger's own continuation indent is already covered by test_log_line.cpp.
#include <gtest/gtest.h>

import std;
import xlings.core.diag;

using xlings::diag::Action;
using xlings::diag::Diagnostic;
using xlings::diag::Fact;
using xlings::diag::Level;

namespace {

Diagnostic sample_() {
    return Diagnostic{
        .level   = Level::Error,
        .code    = "xvm.not_in_subos",
        .summary = "llvm is not installed in this subos (default)",
        .facts   = { Fact{ "installed elsewhere", "22.1.8, 20.1.7" } },
        .actions = { Action{ "install it here", "xlings install llvm@22.1.8" } },
    };
}

std::size_t line_count_(std::string_view s) {
    return static_cast<std::size_t>(std::ranges::count(s, '\n')) + 1;
}

}  // namespace

// ─── Shape ─────────────────────────────────────────────────

TEST(Diag, RendersOneBlockNotSeveralMessages) {
    const auto text = xlings::diag::render(sample_());
    // Summary first, on its own line: it has to stand alone, because a
    // frontend that shows only one line shows this one.
    EXPECT_TRUE(text.starts_with("llvm is not installed in this subos (default)"));
    // Facts and actions are continuations of the SAME message.
    EXPECT_EQ(line_count_(text), 3u);
    EXPECT_NE(text.find("installed elsewhere"), std::string::npos);
    EXPECT_NE(text.find("xlings install llvm@22.1.8"), std::string::npos);
}

TEST(Diag, DetailRowsAlignOnTheWidestLabel) {
    Diagnostic d = sample_();
    d.facts.push_back(Fact{ "x", "short" });

    const auto text = xlings::diag::render(d);
    // "installed elsewhere" is the widest label; the narrow one is padded to
    // match, so the value column is straight. A hardcoded indent is what
    // `xvm::render()` used to do, and it landed detail at column 18 because
    // the logger was already indenting by the tag width.
    const auto wide = text.find("installed elsewhere");
    const auto narrow = text.find("\n  x ");
    ASSERT_NE(wide, std::string::npos);
    ASSERT_NE(narrow, std::string::npos);

    const auto valueColumnOf = [&](std::size_t labelStart, std::size_t labelLen) {
        const auto lineStart = text.rfind('\n', labelStart);
        const auto base = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        auto i = labelStart + labelLen;
        while (i < text.size() && text[i] == ' ') ++i;
        return i - base;
    };
    EXPECT_EQ(valueColumnOf(wide, std::string_view{"installed elsewhere"}.size()),
              valueColumnOf(narrow + 3, 1));
}

TEST(Diag, SourceIsRenderedFirstBecauseItAnswersWhereThisCameFrom) {
    Diagnostic d = sample_();
    d.source = "./.xlings.json  ->  workspace.mcpp";

    const auto text = xlings::diag::render(d);
    const auto from = text.find("from");
    const auto fact = text.find("installed elsewhere");
    ASSERT_NE(from, std::string::npos);
    ASSERT_NE(fact, std::string::npos);
    // A version the user never typed is unactionable until they know who asked
    // for it, so the origin outranks the evidence.
    EXPECT_LT(from, fact);
    EXPECT_NE(text.find("./.xlings.json"), std::string::npos);
}

TEST(Diag, NothingChangedIsRenderedOnlyWhenPromised) {
    EXPECT_EQ(xlings::diag::render(sample_()).find("nothing was changed"),
              std::string::npos);

    Diagnostic d = sample_();
    d.nothingChanged = true;
    EXPECT_NE(xlings::diag::render(d).find("nothing was changed"),
              std::string::npos);
}

// ─── Invariants ────────────────────────────────────────────

TEST(Diag, ValidDiagnosticPasses) {
    EXPECT_FALSE(xlings::diag::validate(sample_()).has_value());
}

TEST(Diag, ProblemWithoutAWayOutIsRejected) {
    Diagnostic d = sample_();
    d.actions.clear();
    // The rule `xvm::errors.cppm` already stated for its own hints, made
    // general: an error nobody can act on is not finished.
    EXPECT_TRUE(xlings::diag::validate(d).has_value());

    d.level = Level::Warn;
    EXPECT_TRUE(xlings::diag::validate(d).has_value());

    // A Note is a courtesy; there may genuinely be nothing to do.
    d.level = Level::Note;
    EXPECT_FALSE(xlings::diag::validate(d).has_value());
}

TEST(Diag, SummaryMayNotCarryItsOwnMarkerOrSpanLines) {
    Diagnostic d = sample_();
    d.summary = "[xlings:use] llvm is not installed";
    EXPECT_TRUE(xlings::diag::validate(d).has_value());

    d = sample_();
    d.summary = "llvm is not installed.";
    EXPECT_TRUE(xlings::diag::validate(d).has_value());

    d = sample_();
    d.summary = "llvm is not installed\n  here";
    EXPECT_TRUE(xlings::diag::validate(d).has_value());
}

TEST(Diag, CodeIsRequiredSoAFindingCanBeSearchedFor) {
    Diagnostic d = sample_();
    d.code.clear();
    EXPECT_TRUE(xlings::diag::validate(d).has_value());
}

// ─── Candidate lists ───────────────────────────────────────

TEST(Diag, CandidatesAreNewestFirstNotMapOrder) {
    // The real bug: `get_all_versions` walks a std::map, so callers printed
    // `0.0.100` before `0.0.24`. Lexicographic order on version strings is
    // not just ugly, it points the user at the wrong version.
    const auto f = xlings::diag::candidates(
        "available", { "0.0.24", "0.0.100", "0.0.9" }, 6);
    EXPECT_EQ(f.value, "0.0.100, 0.0.24, 0.0.9");
}

TEST(Diag, CandidatesAreCappedAndSayHowManyWereDropped) {
    std::vector<std::string> many;
    for (int i = 1; i <= 20; ++i) many.push_back(std::format("1.0.{}", i));

    const auto f = xlings::diag::candidates("available", many, 3,
                                            "xlings list foo");
    // Silent truncation reads as "that's all of them" -- the failure mode this
    // project keeps rediscovering. Name the remainder and where to see it.
    EXPECT_EQ(f.value, "1.0.20, 1.0.19, 1.0.18, +17 more (xlings list foo)");
}

TEST(Diag, EmptyCandidateListSaysSoRatherThanRenderingBlank) {
    const auto f = xlings::diag::candidates("available", {}, 6);
    EXPECT_EQ(f.value, "(none)");
}
