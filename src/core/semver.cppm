module;

export module xlings.core.semver;
import std;

// Generalized version grammar (2026.8.9.2).
//
// The old grammar was semver-strict: at most three numeric components.
// Everything else fell to ad-hoc fallbacks, and the fallbacks disagreed with
// each other: `satisfies_expr` prefix-matched what it could not parse, while
// `select_best` silently SKIPPED it — so a four-component key like
// `2.15.0.1` could be written in a recipe, resolved exactly, and still be
// unselectable by every range expression (`>=2.15.0.1` matched nothing,
// xim-pkgindex#582 hit this in production form).
//
// The grammar is now total over practical version strings:
//
//   version    = field ('.' field)*  ('-' prerelease)?      (dash needs a
//                                                            digit before it)
//   field      = [0-9A-Za-z]+ , split at digit/alpha boundaries into segments
//   segment    = numeric (compared as a number)  |  alpha (compared
//                lexicographically)
//   validity   = at least one numeric segment somewhere. A purely alphabetic
//                string ("latest", "abc", "gcc") is a NAME, not a version,
//                and stays unparseable on purpose — sentinels must never win
//                a version comparison.
//
// Ordering rules, in decreasing load-bearing order:
//   * numeric vs numeric   → numeric ("0.0.100" > "0.0.11" > "0.0.9")
//   * alpha  vs alpha      → lexicographic; boundary-splitting makes it a
//                            natural sort ("rc9" < "rc10" because 9 < 10)
//   * numeric vs alpha     → numeric wins ("1.0" > "1.rc")
//   * missing segment      → numeric 0  ("1.2" == "1.2.0"; "1.0" > "1.0.rc")
//   * prerelease           → lower than the same version without one
//                            (semver §11); compared as a PLAIN string, not
//                            segment-wise — natural-sorting it would change
//                            verdicts on the pre-extension domain
//   * parseable vs not     → parseable wins (sentinels sort after versions;
//                            adopted from version_order, which had it right)
//
// COMPATIBILITY CONTRACT: on the old domain — versions of at most three
// numeric components, with or without prerelease — every (version, expr)
// verdict and every ordering is unchanged. The extension only defines what
// used to be undefined. tests/unit/test_semver.cpp carries the pinned corpus.
export namespace xlings::semver {

// ── Version ──────────────────────────────────────────────────────────

struct Segment {
    bool isNum = true;
    unsigned long long num = 0;
    std::string text;         // alpha segments only

    static Segment number(unsigned long long v) { return {true, v, {}}; }
    static Segment alpha(std::string t) { return {false, 0, std::move(t)}; }
};

struct Version {
    // The first three numeric values, kept for the callers (and tests) that
    // predate the generalized form. An alpha segment reads as 0 here.
    int major = 0;
    int minor = 0;
    int patch = 0;
    int components = 0;       // how many dot-FIELDS the user wrote
    std::string prerelease;   // "" = release, "beta.1" = prerelease
    std::vector<Segment> segs;
    std::string raw;          // the trimmed input, for faithful round-trips
};

// ── Compare ──────────────────────────────────────────────────────────

// A missing right-hand segment behaves as numeric 0.
inline std::strong_ordering compare_segment(const Segment& a, const Segment& b) {
    if (a.isNum && b.isNum) return a.num <=> b.num;
    if (a.isNum != b.isNum) {
        // numeric beats alpha: 1.0 > 1.rc
        return a.isNum ? std::strong_ordering::greater
                       : std::strong_ordering::less;
    }
    return a.text <=> b.text;
}

// Three-way compare: segments left-to-right (missing = 0) → prerelease.
inline std::strong_ordering compare_versions(const Version& a, const Version& b) {
    const auto count = std::max(a.segs.size(), b.segs.size());
    static const Segment zero = Segment::number(0);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& l = i < a.segs.size() ? a.segs[i] : zero;
        const auto& r = i < b.segs.size() ? b.segs[i] : zero;
        if (auto c = compare_segment(l, r); c != 0) return c;
    }
    // Both have prerelease → lexicographic (alpha < beta < rc)
    if (!a.prerelease.empty() && !b.prerelease.empty())
        return a.prerelease <=> b.prerelease;
    // One has, other doesn't → prerelease is lower (1.0.0-alpha < 1.0.0)
    if (a.prerelease.empty() && !b.prerelease.empty())
        return std::strong_ordering::greater;
    if (!a.prerelease.empty() && b.prerelease.empty())
        return std::strong_ordering::less;
    return std::strong_ordering::equal;
}

// ── Parse ────────────────────────────────────────────────────────────

// Parse "15.1.0", "15.1", "2026.8.9.1", "6.5rc10", "1.3.3-beta.1".
// Rejects the empty string, fields with characters outside [0-9A-Za-z],
// and anything with no numeric segment at all (names are not versions).
inline std::optional<Version> parse(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    Version v;
    v.raw = std::string(s);

    // Split off prerelease: everything after the first '-' that follows at
    // least one digit. "gcc-15" has no digit before the dash and stays whole
    // (and then fails the field charset, as a name should).
    std::string_view numpart = s;
    std::string_view prepart;
    if (auto dash = s.find('-'); dash != std::string_view::npos) {
        bool has_digit = false;
        for (std::size_t i = 0; i < dash; ++i) {
            if (s[i] >= '0' && s[i] <= '9') { has_digit = true; break; }
        }
        if (has_digit) {
            numpart = s.substr(0, dash);
            prepart = s.substr(dash + 1);
        }
    }

    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto is_alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };

    bool sawNumeric = false;
    std::size_t start = 0;
    while (start <= numpart.size()) {
        auto dot = numpart.find('.', start);
        auto field = numpart.substr(
            start, dot == std::string_view::npos ? numpart.size() - start
                                                 : dot - start);
        if (field.empty()) return std::nullopt;     // "1..2", "1.", ".1"
        v.components++;

        // Split the field at digit/alpha boundaries: "6a" → 6, "a";
        // "rc10" → "rc", 10. That is what makes alpha ordering a natural
        // sort instead of a byte sort.
        std::size_t i = 0;
        while (i < field.size()) {
            if (is_digit(field[i])) {
                std::size_t j = i;
                while (j < field.size() && is_digit(field[j])) ++j;
                auto digits = field.substr(i, j - i);
                if (digits.size() > 19) return std::nullopt;  // absurd input
                unsigned long long val = 0;
                for (char c : digits) val = val * 10 + (c - '0');
                v.segs.push_back(Segment::number(val));
                sawNumeric = true;
                i = j;
            } else if (is_alpha(field[i])) {
                std::size_t j = i;
                while (j < field.size() && is_alpha(field[j])) ++j;
                v.segs.push_back(Segment::alpha(std::string(field.substr(i, j - i))));
                i = j;
            } else {
                return std::nullopt;                // '_', '+', anything else
            }
        }

        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (v.segs.empty() || !sawNumeric) return std::nullopt;

    const auto numeric_at = [&](std::size_t i) -> int {
        if (i >= v.segs.size() || !v.segs[i].isNum) return 0;
        return static_cast<int>(v.segs[i].num);
    };
    v.major = numeric_at(0);
    v.minor = numeric_at(1);
    v.patch = numeric_at(2);
    if (!prepart.empty()) v.prerelease = std::string(prepart);
    return v;
}

// Format version back to string. The raw input round-trips faithfully;
// synthetic versions (range bounds) reconstruct from what they carry.
inline std::string to_string(const Version& v) {
    if (!v.raw.empty()) return v.raw;
    std::string s;
    s += std::to_string(v.major);
    if (v.components >= 2) { s += '.'; s += std::to_string(v.minor); }
    if (v.components >= 3) { s += '.'; s += std::to_string(v.patch); }
    if (!v.prerelease.empty()) { s += '-'; s += v.prerelease; }
    return s;
}

// ── Range ────────────────────────────────────────────────────────────

enum class Op { Eq, Gt, Gte, Lt, Lte };

struct Constraint {
    Op op;
    Version ver;
};

struct Range {
    std::vector<Constraint> constraints;
};

namespace detail_ {

// A synthetic bound: numeric segments only, no raw, no field count.
inline Version num_version(std::initializer_list<unsigned long long> nums) {
    Version v;
    for (auto n : nums) v.segs.push_back(Segment::number(n));
    v.components = static_cast<int>(v.segs.size());
    const auto at = [&](std::size_t i) {
        return i < v.segs.size() && v.segs[i].isNum
             ? static_cast<int>(v.segs[i].num) : 0;
    };
    v.major = at(0); v.minor = at(1); v.patch = at(2);
    return v;
}

// The numeric value a bump rule sees at seg i: alpha reads as 0.
inline unsigned long long num_at(const Version& v, std::size_t i) {
    return i < v.segs.size() && v.segs[i].isNum ? v.segs[i].num : 0;
}

}  // namespace detail_

// Check if a single constraint is satisfied.
//
// Eq is written-prefix equality, floored at three segments: "2.15.0.1"
// matches 2.15.0.1 (and a hypothetical 2.15.0.1.x), "15.1.0" matches
// 15.1.0 and 15.1.0.5 but never 15.1.1 — which is exactly what the old
// grammar's component-boundary fallback already promised for versions it
// could not parse ("2026.7.31" matches "2026.7.31.2"). The three-segment
// floor keeps the old padded-exact meaning for one- and two-segment
// constraint tokens ("1.2" as Eq matches 1.2 and 1.2.0, never 1.2.5).
// Ordering constraints always compare the whole thing, missing = 0.
inline bool check_constraint(const Version& ver, const Constraint& c) {
    if (c.op == Op::Eq) {
        const auto width = std::max<std::size_t>(c.ver.segs.size(), 3);
        static const Segment zero = Segment::number(0);
        for (std::size_t i = 0; i < width; ++i) {
            const auto& l = i < ver.segs.size() ? ver.segs[i] : zero;
            const auto& r = i < c.ver.segs.size() ? c.ver.segs[i] : zero;
            if (compare_segment(l, r) != 0) return false;
        }
        return ver.prerelease == c.ver.prerelease;
    }
    auto cmp = compare_versions(ver, c.ver);
    switch (c.op) {
        case Op::Eq:  return cmp == 0;   // unreachable; kept for -Wswitch
        case Op::Gt:  return cmp > 0;
        case Op::Gte: return cmp >= 0;
        case Op::Lt:  return cmp < 0;
        case Op::Lte: return cmp <= 0;
    }
    return false;
}

inline bool satisfies(const Version& ver, const Range& range) {
    for (auto& c : range.constraints) {
        if (!check_constraint(ver, c)) return false;
    }
    return true;
}

// Parse a single constraint token: ">=1.2.3", ">1.0", "<=2.0.0", "1.2.3"
inline std::optional<std::pair<Op, Version>> parse_constraint_token(std::string_view tok) {
    while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
    while (!tok.empty() && tok.back() == ' ') tok.remove_suffix(1);
    if (tok.empty()) return std::nullopt;

    Op op = Op::Eq;
    if (tok.starts_with(">=")) { op = Op::Gte; tok.remove_prefix(2); }
    else if (tok.starts_with(">"))  { op = Op::Gt;  tok.remove_prefix(1); }
    else if (tok.starts_with("<=")) { op = Op::Lte; tok.remove_prefix(2); }
    else if (tok.starts_with("<"))  { op = Op::Lt;  tok.remove_prefix(1); }

    while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
    auto v = parse(tok);
    if (!v) return std::nullopt;
    return std::pair{op, *v};
}

// Parse range expression. Returns nullopt only for truly malformed input.
//
// Supported forms (N-segment versions allowed everywhere a version appears):
//   "1.2.3"           → Eq{1.2.3}          (bare ≥3 fields → exact-prefix)
//   "2.15.0.1"        → Eq{2.15.0.1}
//   "15"              → Gte{15} Lt{16}     (bare 1–2 fields → prefix range)
//   "15.1"            → Gte{15.1} Lt{15.2}
//   ">=1.0.0"         → Gte{1.0.0}
//   ">=2.15.0.1"      → Gte{2.15.0.1}
//   ">=1.0.0 <2.0.0"  → Gte{1.0.0} Lt{2.0.0}
//   "^1.2.3"          → Gte{1.2.3} Lt{2}   (first nonzero segment bumps)
//   "^0.2.3"          → Gte{0.2.3} Lt{0.3}
//   "~1.2.3"          → Gte{1.2.3} Lt{1.3} (second segment bumps)
//   "1.2.*"           → Gte{1.2} Lt{1.3}
//   "1.*"             → Gte{1} Lt{2}
inline std::optional<Range> parse_range(std::string_view expr) {
    while (!expr.empty() && expr.front() == ' ') expr.remove_prefix(1);
    while (!expr.empty() && expr.back() == ' ') expr.remove_suffix(1);
    if (expr.empty()) return std::nullopt;

    Range range;

    // ── Caret: ^1.2.3 — up to the next release of the first segment that
    //    is nonzero (or alpha, which reads as 0 and bumps to 1) ──
    if (expr.starts_with("^")) {
        auto v = parse(expr.substr(1));
        if (!v) return std::nullopt;
        std::size_t k = 0;
        while (k + 1 < v->segs.size()
               && v->segs[k].isNum && v->segs[k].num == 0) {
            ++k;
        }
        Version hi;
        for (std::size_t i = 0; i < k; ++i)
            hi.segs.push_back(Segment::number(detail_::num_at(*v, i)));
        hi.segs.push_back(Segment::number(detail_::num_at(*v, k) + 1));
        hi.components = static_cast<int>(hi.segs.size());
        range.constraints.push_back({Op::Gte, *v});
        range.constraints.push_back({Op::Lt, std::move(hi)});
        return range;
    }

    // ── Tilde: ~1.2.3 — up to the next second segment ──
    if (expr.starts_with("~")) {
        auto v = parse(expr.substr(1));
        if (!v) return std::nullopt;
        auto hi = detail_::num_version({detail_::num_at(*v, 0),
                                        detail_::num_at(*v, 1) + 1});
        range.constraints.push_back({Op::Gte, *v});
        range.constraints.push_back({Op::Lt, std::move(hi)});
        return range;
    }

    // ── Wildcard: 1.2.*, 1.* ──
    if (expr.find('*') != std::string_view::npos) {
        auto star = expr.find('*');
        auto prefix = expr.substr(0, star);
        while (!prefix.empty() && prefix.back() == '.') prefix.remove_suffix(1);
        if (prefix.empty()) return std::nullopt;
        auto v = parse(prefix);
        if (!v) return std::nullopt;
        Version hi;
        if (v->components == 1) {
            hi = detail_::num_version({detail_::num_at(*v, 0) + 1});
        } else {
            hi = detail_::num_version({detail_::num_at(*v, 0),
                                       detail_::num_at(*v, 1) + 1});
        }
        range.constraints.push_back({Op::Gte, *v});
        range.constraints.push_back({Op::Lt, std::move(hi)});
        return range;
    }

    // ── Comparison operators: >=, >, <=, <, space-separated conjunction ──
    if (expr.starts_with(">") || expr.starts_with("<")) {
        std::size_t pos = 0;
        while (pos < expr.size()) {
            while (pos < expr.size() && expr[pos] == ' ') ++pos;
            if (pos >= expr.size()) break;

            std::size_t start = pos;
            while (pos < expr.size() && (expr[pos] == '>' || expr[pos] == '<' || expr[pos] == '='))
                ++pos;
            while (pos < expr.size() && expr[pos] == ' ') ++pos;
            while (pos < expr.size() && expr[pos] != ' ') ++pos;

            auto tok = expr.substr(start, pos - start);
            auto parsed = parse_constraint_token(tok);
            if (!parsed) return std::nullopt;
            range.constraints.push_back({parsed->first, parsed->second});
        }
        return range.constraints.empty() ? std::nullopt : std::optional{range};
    }

    // ── Bare version ──
    auto v = parse(expr);
    if (!v) return std::nullopt;

    if (v->components >= 3 && v->prerelease.empty()) {
        // Three or more fields → exact (written-prefix, see check_constraint)
        range.constraints.push_back({Op::Eq, *v});
    } else if (!v->prerelease.empty()) {
        // A prerelease is a specific artifact → exact
        range.constraints.push_back({Op::Eq, *v});
    } else {
        // One or two fields → prefix range
        Version hi;
        if (v->components == 1) {
            hi = detail_::num_version({detail_::num_at(*v, 0) + 1});
        } else {
            hi = detail_::num_version({detail_::num_at(*v, 0),
                                       detail_::num_at(*v, 1) + 1});
        }
        range.constraints.push_back({Op::Gte, *v});
        range.constraints.push_back({Op::Lt, std::move(hi)});
    }
    return range;
}

// ── Utility functions ────────────────────────────────────────────────

// Compare two version strings. Returns -1, 0, or 1.
//
// A parseable version always outranks an unparseable string: sentinels
// ("latest", "res_versioned") must never win a version comparison. Two
// unparseable strings fall back to a lexicographic tiebreak so sorts stay
// deterministic. (This rule came from version_order, which now delegates
// here — one comparator, not two drifting ones.)
inline int compare(std::string_view a, std::string_view b) {
    auto va = parse(a);
    auto vb = parse(b);
    if (va && vb) {
        auto c = compare_versions(*va, *vb);
        if (c < 0) return -1;
        if (c > 0) return 1;
        return 0;
    }
    if (va && !vb) return 1;
    if (!va && vb) return -1;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// Sort version strings in descending order (highest first). Stable, so
// equal-comparing strings keep their incoming order.
inline void sort_desc(std::vector<std::string>& versions) {
    std::stable_sort(versions.begin(), versions.end(),
        [](const std::string& a, const std::string& b) {
            return compare(a, b) > 0;
        });
}

// Does one concrete version satisfy a constraint expression?
//
// This is the single-version counterpart of select_best, and it exists
// because "is what I already have good enough?" is a different question from
// "which of these should I fetch?" -- the answer decides whether an install
// happens at all.
//
// Empty `expr` is no constraint, so everything satisfies it. That is the
// whole point: a dependency written `xim:mcpp` with no `@` is asking for
// *some* mcpp, not for the newest one.
//
// Four-component releases (`2026.7.31.2`) and alpha-bearing versions are in
// the grammar now, so they take the range path like everything else. The
// component-boundary prefix fallback stays for what is STILL outside the
// grammar (flavor-tagged strings on the constraint side, and so on):
// `2026.7.31` must keep matching `2026.7.31.2`, and `1.1` must keep not
// matching `1.10`, which a bare starts_with would.
inline bool satisfies_expr(std::string_view version, std::string_view expr) {
    while (!expr.empty() && expr.front() == ' ') expr.remove_prefix(1);
    while (!expr.empty() && expr.back() == ' ') expr.remove_suffix(1);
    if (expr.empty()) return true;
    if (version.empty()) return false;
    if (version == expr) return true;

    auto v = parse(version);
    auto range = parse_range(expr);
    if (v && range) return satisfies(*v, *range);

    // Component-boundary prefix fallback for anything outside the grammar.
    if (version.size() <= expr.size()) return false;
    if (!version.starts_with(expr)) return false;
    const char next = version[expr.size()];
    return next == '.' || next == '-';
}

// Select the highest version from `available` that satisfies `range_expr`.
// Skips "latest" tags. Returns "" if no match.
//
// Every parseable version is a candidate. The old grammar dropped what it
// could not parse, which made a four-component KEY resolvable exactly yet
// invisible to every range — writable, installable, unselectable.
inline std::string select_best(std::span<const std::string> available,
                               std::string_view range_expr) {
    auto range = parse_range(range_expr);
    if (!range) return {};

    std::string best;
    for (auto& vs : available) {
        if (vs == "latest") continue;
        auto v = parse(vs);
        if (!v) continue;
        if (!satisfies(*v, *range)) continue;
        if (best.empty() || compare(vs, best) > 0) {
            best = vs;
        }
    }
    return best;
}

} // namespace xlings::semver
