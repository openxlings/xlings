#include <gtest/gtest.h>
import std;
import xlings.core.semver;

using namespace xlings::semver;

// ── Parse ──

TEST(SemverParse, ThreeComponents) {
    auto v = parse("15.1.0");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 15);
    EXPECT_EQ(v->minor, 1);
    EXPECT_EQ(v->patch, 0);
    EXPECT_EQ(v->components, 3);
    EXPECT_TRUE(v->prerelease.empty());
}

TEST(SemverParse, TwoComponents) {
    auto v = parse("15.1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 15);
    EXPECT_EQ(v->minor, 1);
    EXPECT_EQ(v->patch, 0);
    EXPECT_EQ(v->components, 2);
}

TEST(SemverParse, OneComponent) {
    auto v = parse("15");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 15);
    EXPECT_EQ(v->components, 1);
}

TEST(SemverParse, Prerelease) {
    auto v = parse("1.3.3-beta.1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 1);
    EXPECT_EQ(v->minor, 3);
    EXPECT_EQ(v->patch, 3);
    EXPECT_EQ(v->prerelease, "beta.1");
}

TEST(SemverParse, LargeNumbers) {
    auto v = parse("2026.5.7");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 2026);
    EXPECT_EQ(v->minor, 5);
    EXPECT_EQ(v->patch, 7);
}

TEST(SemverParse, Empty) {
    EXPECT_FALSE(parse("").has_value());
}

TEST(SemverParse, Letters) {
    EXPECT_FALSE(parse("abc").has_value());
    EXPECT_FALSE(parse("gcc-15").has_value());
}

// ── Compare ──

TEST(SemverCompare, NumericCorrect) {
    // The bug: lexicographic "9.0" > "15.0", but numeric 9 < 15
    EXPECT_LT(compare("9.0.0", "15.0.0"), 0);
    EXPECT_GT(compare("15.0.0", "9.0.0"), 0);
}

TEST(SemverCompare, BasicOrdering) {
    EXPECT_LT(compare("1.0.0", "2.0.0"), 0);
    EXPECT_LT(compare("1.0.0", "1.1.0"), 0);
    EXPECT_LT(compare("1.0.0", "1.0.1"), 0);
    EXPECT_EQ(compare("1.0.0", "1.0.0"), 0);
}

TEST(SemverCompare, PrereleaseIsLower) {
    // semver: 1.0.0-beta < 1.0.0
    EXPECT_LT(compare("1.0.0-beta", "1.0.0"), 0);
    EXPECT_GT(compare("1.0.0", "1.0.0-beta"), 0);
}

TEST(SemverCompare, PrereleaseOrdering) {
    EXPECT_LT(compare("1.0.0-alpha", "1.0.0-beta"), 0);
    EXPECT_LT(compare("1.0.0-beta.1", "1.0.0-beta.2"), 0);
}

// ── SortDesc ──

TEST(SemverSort, DescendingOrder) {
    std::vector<std::string> versions = {"1.0.0", "15.1.0", "9.4.0", "2.39", "15.0.0"};
    sort_desc(versions);
    ASSERT_EQ(versions.size(), 5);
    EXPECT_EQ(versions[0], "15.1.0");
    EXPECT_EQ(versions[1], "15.0.0");
    EXPECT_EQ(versions[2], "9.4.0");
    EXPECT_EQ(versions[3], "2.39");
    EXPECT_EQ(versions[4], "1.0.0");
}

// ── Range parsing ──

TEST(SemverRange, ExactThreeComponent) {
    auto r = parse_range("15.1.0");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("15.1.0"), *r));
    EXPECT_FALSE(satisfies(*parse("15.1.1"), *r));
    EXPECT_FALSE(satisfies(*parse("15.0.0"), *r));
}

TEST(SemverRange, PrefixOneComponent) {
    // "15" → [15.0.0, 16.0.0)
    auto r = parse_range("15");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("15.0.0"), *r));
    EXPECT_TRUE(satisfies(*parse("15.1.0"), *r));
    EXPECT_TRUE(satisfies(*parse("15.99.99"), *r));
    EXPECT_FALSE(satisfies(*parse("14.9.9"), *r));
    EXPECT_FALSE(satisfies(*parse("16.0.0"), *r));
}

TEST(SemverRange, PrefixTwoComponent) {
    // "15.1" → [15.1.0, 15.2.0)
    auto r = parse_range("15.1");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("15.1.0"), *r));
    EXPECT_TRUE(satisfies(*parse("15.1.5"), *r));
    EXPECT_FALSE(satisfies(*parse("15.0.0"), *r));
    EXPECT_FALSE(satisfies(*parse("15.2.0"), *r));
}

TEST(SemverRange, Caret) {
    // ^1.2.3 → [1.2.3, 2.0.0)
    auto r = parse_range("^1.2.3");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("1.2.3"), *r));
    EXPECT_TRUE(satisfies(*parse("1.9.0"), *r));
    EXPECT_FALSE(satisfies(*parse("1.2.2"), *r));
    EXPECT_FALSE(satisfies(*parse("2.0.0"), *r));
}

TEST(SemverRange, CaretZeroMajor) {
    // ^0.2.3 → [0.2.3, 0.3.0)
    auto r = parse_range("^0.2.3");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("0.2.3"), *r));
    EXPECT_TRUE(satisfies(*parse("0.2.9"), *r));
    EXPECT_FALSE(satisfies(*parse("0.3.0"), *r));
}

TEST(SemverRange, Tilde) {
    // ~1.2.3 → [1.2.3, 1.3.0)
    auto r = parse_range("~1.2.3");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("1.2.3"), *r));
    EXPECT_TRUE(satisfies(*parse("1.2.9"), *r));
    EXPECT_FALSE(satisfies(*parse("1.3.0"), *r));
    EXPECT_FALSE(satisfies(*parse("1.2.2"), *r));
}

TEST(SemverRange, Gte) {
    auto r = parse_range(">=1.0.0");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("1.0.0"), *r));
    EXPECT_TRUE(satisfies(*parse("2.0.0"), *r));
    EXPECT_FALSE(satisfies(*parse("0.9.9"), *r));
}

TEST(SemverRange, Lt) {
    auto r = parse_range("<2.0.0");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("1.9.9"), *r));
    EXPECT_FALSE(satisfies(*parse("2.0.0"), *r));
}

TEST(SemverRange, CombinedGteLt) {
    // ">=1.0.0 <2.0.0"
    auto r = parse_range(">=1.0.0 <2.0.0");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("1.0.0"), *r));
    EXPECT_TRUE(satisfies(*parse("1.5.0"), *r));
    EXPECT_FALSE(satisfies(*parse("0.9.0"), *r));
    EXPECT_FALSE(satisfies(*parse("2.0.0"), *r));
}

TEST(SemverRange, Wildcard) {
    // "1.2.*" → [1.2.0, 1.3.0)
    auto r = parse_range("1.2.*");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("1.2.0"), *r));
    EXPECT_TRUE(satisfies(*parse("1.2.5"), *r));
    EXPECT_FALSE(satisfies(*parse("1.3.0"), *r));
}

TEST(SemverRange, WildcardMajor) {
    // "1.*" → [1.0.0, 2.0.0)
    auto r = parse_range("1.*");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(satisfies(*parse("1.0.0"), *r));
    EXPECT_TRUE(satisfies(*parse("1.99.0"), *r));
    EXPECT_FALSE(satisfies(*parse("2.0.0"), *r));
}

// ── SelectBest ──

TEST(SemverSelectBest, BasicSelection) {
    std::vector<std::string> available = {"13.1.0", "14.2.0", "15.1.0", "16.1.0", "latest"};
    EXPECT_EQ(select_best(available, "15"), "15.1.0");
    EXPECT_EQ(select_best(available, "^14.0.0"), "14.2.0");
    EXPECT_EQ(select_best(available, ">=15.0.0"), "16.1.0");
    EXPECT_EQ(select_best(available, ">=15.0.0 <16.0.0"), "15.1.0");
}

TEST(SemverSelectBest, NoMatch) {
    std::vector<std::string> available = {"1.0.0", "2.0.0"};
    EXPECT_EQ(select_best(available, ">=3.0.0"), "");
}

TEST(SemverSelectBest, SkipsLatest) {
    std::vector<std::string> available = {"latest", "1.0.0"};
    EXPECT_EQ(select_best(available, ">=1.0.0"), "1.0.0");
}

TEST(SemverSelectBest, NumericNotLexicographic) {
    // Regression: lexicographic "9.4.0" > "15.1.0"
    std::vector<std::string> available = {"9.4.0", "15.1.0", "11.5.0"};
    EXPECT_EQ(select_best(available, ">=1.0.0"), "15.1.0");
}

// ── ToString ──

TEST(SemverToString, RoundTrip) {
    EXPECT_EQ(to_string(*parse("15.1.0")), "15.1.0");
    EXPECT_EQ(to_string(*parse("15.1")), "15.1");
    EXPECT_EQ(to_string(*parse("15")), "15");
    EXPECT_EQ(to_string(*parse("1.3.3-beta.1")), "1.3.3-beta.1");
}

// ── SatisfiesExpr ──
//
// The predicate that decides whether an install happens at all: "is the
// version already active good enough for this dependency?"

TEST(SemverSatisfiesExpr, EmptyConstraintAcceptsAnything) {
    // A dependency written `xim:mcpp` asks for *some* mcpp, not the newest.
    EXPECT_TRUE(satisfies_expr("1.0.0", ""));
    EXPECT_TRUE(satisfies_expr("2026.7.31.2", ""));
    EXPECT_TRUE(satisfies_expr("15.1.0-musl", ""));
    EXPECT_TRUE(satisfies_expr("anything", "   "));
}

TEST(SemverSatisfiesExpr, EmptyVersionSatisfiesNothingConcrete) {
    EXPECT_FALSE(satisfies_expr("", "1.0.0"));
    EXPECT_TRUE(satisfies_expr("", ""));
}

TEST(SemverSatisfiesExpr, ExactPin) {
    EXPECT_TRUE(satisfies_expr("2.39", "2.39"));
    EXPECT_FALSE(satisfies_expr("2.38", "2.39"));
    // A flavor tag is a different version, not the same one.
    EXPECT_FALSE(satisfies_expr("15.1.0-musl", "15.1.0"));
}

TEST(SemverSatisfiesExpr, BarePrefixIsARange) {
    EXPECT_TRUE(satisfies_expr("3.11.4", "3"));
    EXPECT_TRUE(satisfies_expr("3.0.0", "3"));
    EXPECT_FALSE(satisfies_expr("4.0.0", "3"));
    EXPECT_FALSE(satisfies_expr("2.9.9", "3"));
}

TEST(SemverSatisfiesExpr, PrefixDoesNotMatchAcrossComponentBoundary) {
    // The bug a plain starts_with test has: "1.1" must not accept "1.10".
    EXPECT_FALSE(satisfies_expr("1.10.0", "1.1"));
    EXPECT_TRUE(satisfies_expr("1.1.9", "1.1"));
}

TEST(SemverSatisfiesExpr, Operators) {
    EXPECT_TRUE(satisfies_expr("1.5.0", "^1.2.3"));
    EXPECT_FALSE(satisfies_expr("2.0.0", "^1.2.3"));
    EXPECT_TRUE(satisfies_expr("1.2.9", "~1.2.3"));
    EXPECT_FALSE(satisfies_expr("1.3.0", "~1.2.3"));
    EXPECT_TRUE(satisfies_expr("1.5.0", ">=1.0.0 <2.0.0"));
    EXPECT_FALSE(satisfies_expr("2.5.0", ">=1.0.0 <2.0.0"));
    EXPECT_TRUE(satisfies_expr("1.2.7", "1.2.*"));
}

TEST(SemverSatisfiesExpr, FourComponentReleaseStampsFallBackToPrefix) {
    // xlings' own releases carry four components, which `parse` rejects.
    // Rejecting them here would turn "already satisfied" into "install the
    // latest" for the packages that ship most often.
    EXPECT_TRUE(satisfies_expr("2026.7.31.2", "2026.7.31.2"));
    EXPECT_TRUE(satisfies_expr("2026.7.31.2", "2026.7.31"));
    EXPECT_TRUE(satisfies_expr("2026.7.31.2", "2026.7"));
    EXPECT_FALSE(satisfies_expr("2026.7.31.2", "2026.7.30"));
    EXPECT_FALSE(satisfies_expr("2026.7.3.1", "2026.7.31"));
}

TEST(SemverSatisfiesExpr, FlavorTagSatisfiesTheNumericRange) {
    // 15.1.0-musl parses as 15.1.0 with a prerelease tag, and is inside "15".
    EXPECT_TRUE(satisfies_expr("15.1.0-musl", "15"));
    EXPECT_FALSE(satisfies_expr("15.1.0-musl", "16"));
}

// ═══════════════════════════════════════════════════════════════════════
//  Generalized grammar (2026.8.9.2): N-segment versions and alpha segments
//  are IN the grammar — ranges, selection and ordering all see them.
//
//  The motivating production failure: fontconfig 2.15.0.1 was a resolvable
//  KEY but `>=2.15.0.1` matched nothing and select_best skipped the version
//  entirely (xim-pkgindex#582) — writable, installable, unselectable.
// ═══════════════════════════════════════════════════════════════════════

TEST(SemverGeneralized, FourComponentsParse) {
    auto v = parse("2.15.0.1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 2);
    EXPECT_EQ(v->minor, 15);
    EXPECT_EQ(v->patch, 0);
    EXPECT_EQ(v->components, 4);
    EXPECT_EQ(to_string(*v), "2.15.0.1");
}

TEST(SemverGeneralized, FourComponentRangesResolve) {
    EXPECT_TRUE(satisfies_expr("2.15.0.1", ">=2.15.0.1"));
    EXPECT_FALSE(satisfies_expr("2.15.0", ">=2.15.0.1"));   // the old gap
    EXPECT_TRUE(satisfies_expr("2.16.0", ">=2.15.0.1"));
    EXPECT_TRUE(satisfies_expr("2.15.0.1", ">=2.15.0"));
    EXPECT_TRUE(satisfies_expr("2026.8.9.2", ">2026.8.9.1"));
    EXPECT_FALSE(satisfies_expr("2026.8.9.1", ">2026.8.9.1"));
}

TEST(SemverGeneralized, SelectBestSeesFourComponents) {
    std::vector<std::string> available{"2.15.0", "2.15.0.1", "latest"};
    // Every one of these returned "" or the unsealed 2.15.0 under the old
    // grammar, because 2.15.0.1 was skipped as unparseable.
    EXPECT_EQ(select_best(available, ">=2.15.0"), "2.15.0.1");
    EXPECT_EQ(select_best(available, ">=2.15.0.1"), "2.15.0.1");
    EXPECT_EQ(select_best(available, "2.15.0.1"), "2.15.0.1");
    EXPECT_EQ(select_best(available, ">=2.15.0.2"), "");
}

TEST(SemverGeneralized, CaretAndTildeOnFourComponents) {
    EXPECT_TRUE(satisfies_expr("2026.9.0", "^2026.8.9.1"));
    EXPECT_FALSE(satisfies_expr("2027.0.0", "^2026.8.9.1"));
    EXPECT_TRUE(satisfies_expr("2.15.9", "~2.15.0.1"));
    EXPECT_FALSE(satisfies_expr("2.16.0", "~2.15.0.1"));
}

TEST(SemverGeneralized, AlphaSegmentsAreANaturalSort) {
    // Boundary-splitting is what makes this numeric where it counts.
    EXPECT_GT(compare("6.5rc10", "6.5rc9"), 0);
    EXPECT_LT(compare("6.5rc2", "6.5rc10"), 0);
    EXPECT_LT(compare("1.0.alpha", "1.0.beta"), 0);
}

TEST(SemverGeneralized, NumericBeatsAlpha) {
    EXPECT_GT(compare("1.0", "1.rc"), 0);
    // A missing segment reads as numeric 0, which also beats alpha:
    // 6.5 is the release, 6.5rc1 came before it.
    EXPECT_GT(compare("6.5", "6.5rc1"), 0);
    EXPECT_GT(compare("1.0.0", "1.0.beta"), 0);
}

TEST(SemverGeneralized, AlphaRangesWork) {
    EXPECT_TRUE(satisfies_expr("6.5rc9", ">=6.5rc2"));
    EXPECT_TRUE(satisfies_expr("6.5rc10", ">=6.5rc9"));
    EXPECT_TRUE(satisfies_expr("6.5", ">=6.5rc1"));
    EXPECT_FALSE(satisfies_expr("6.5rc1", ">=6.5"));
}

TEST(SemverGeneralized, PureAlphaStaysAName) {
    // "latest" and friends are sentinels; a version must contain at least
    // one numeric segment, or every sentinel becomes an (absurdly high or
    // low) version and sorts among the real ones.
    EXPECT_FALSE(parse("latest").has_value());
    EXPECT_FALSE(parse("abc").has_value());
    EXPECT_FALSE(parse("gcc-15").has_value());   // name, not version
    EXPECT_GT(compare("0.0.1", "latest"), 0);    // parseable always wins
}

TEST(SemverGeneralized, MixedFieldsKeepFieldCountSemantics) {
    // "6.5rc1" is two FIELDS (bare → prefix range), four segments.
    auto v = parse("6.5rc1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->components, 2);
    // NOT inside [6.5, 6.6): rc1 sorts BELOW the 6.5 release (alpha loses
    // to the missing-segment zero), the same way semver keeps 1.0.0-rc1
    // out of ">=1.0.0". One ordering, one answer — a version cannot be
    // both below 6.5 in a sort and inside a range that starts at 6.5.
    EXPECT_FALSE(satisfies_expr("6.5rc1", "6.5"));
    EXPECT_TRUE(satisfies_expr("6.5.1", "6.5"));
    EXPECT_FALSE(satisfies_expr("6.6", "6.5"));
}
