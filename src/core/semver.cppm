module;

export module xlings.core.semver;
import std;

export namespace xlings::semver {

// ── Version ──────────────────────────────────────────────────────────

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    int components = 0;       // how many segments the user wrote
    std::string prerelease;   // "" = release, "beta.1" = prerelease
};

// Three-way compare: major → minor → patch → prerelease.
// Prerelease sorts lower than the same triple without prerelease
// (semver §11: 1.0.0-alpha < 1.0.0).
inline std::strong_ordering compare_versions(const Version& a, const Version& b) {
    if (auto c = a.major <=> b.major; c != 0) return c;
    if (auto c = a.minor <=> b.minor; c != 0) return c;
    if (auto c = a.patch <=> b.patch; c != 0) return c;
    // Both have prerelease → lexicographic (alpha < beta < rc)
    if (!a.prerelease.empty() && !b.prerelease.empty())
        return a.prerelease <=> b.prerelease;
    // One has, other doesn't → prerelease is lower
    if (a.prerelease.empty() && !b.prerelease.empty())
        return std::strong_ordering::greater;
    if (!a.prerelease.empty() && b.prerelease.empty())
        return std::strong_ordering::less;
    return std::strong_ordering::equal;
}

// ── Parse ────────────────────────────────────────────────────────────

// Parse "15.1.0", "15.1", "15", "1.3.3-beta.1"
inline std::optional<Version> parse(std::string_view s) {
    if (s.empty()) return std::nullopt;

    Version v;
    // Strip leading whitespace
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return std::nullopt;

    // Split off prerelease: everything after first '-' that follows digits
    std::string_view numpart = s;
    std::string_view prepart;
    if (auto dash = s.find('-'); dash != std::string_view::npos) {
        // Make sure there's at least one digit before the dash
        bool has_digit = false;
        for (std::size_t i = 0; i < dash; ++i) {
            if (s[i] >= '0' && s[i] <= '9') { has_digit = true; break; }
        }
        if (has_digit) {
            numpart = s.substr(0, dash);
            prepart = s.substr(dash + 1);
        }
    }

    // Parse numeric components (up to 3)
    int parts[3] = {0, 0, 0};
    int count = 0;
    std::size_t pos = 0;
    while (pos < numpart.size() && count < 3) {
        // Must start with digit
        if (numpart[pos] < '0' || numpart[pos] > '9') return std::nullopt;

        int val = 0;
        while (pos < numpart.size() && numpart[pos] >= '0' && numpart[pos] <= '9') {
            val = val * 10 + (numpart[pos] - '0');
            ++pos;
        }
        parts[count++] = val;

        if (pos < numpart.size()) {
            if (numpart[pos] == '.') {
                ++pos; // skip dot
            } else {
                return std::nullopt; // unexpected char
            }
        }
    }
    // Trailing content after 3 components is invalid
    if (pos < numpart.size()) return std::nullopt;
    if (count == 0) return std::nullopt;

    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    v.components = count;
    if (!prepart.empty()) v.prerelease = std::string(prepart);

    return v;
}

// Format version back to string
inline std::string to_string(const Version& v) {
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

// Check if a single constraint is satisfied
inline bool check_constraint(const Version& ver, const Constraint& c) {
    auto cmp = compare_versions(ver, c.ver);
    switch (c.op) {
        case Op::Eq:  return cmp == 0;
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

// Parse a single constraint token: ">=1.2.3", ">1.0", "<=2.0.0", "<3", "1.2.3"
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
// Supported forms:
//   "1.2.3"           → Eq{1.2.3}
//   "15"              → Gte{15.0.0} Lt{16.0.0}   (bare prefix → range)
//   "15.1"            → Gte{15.1.0} Lt{15.2.0}
//   ">=1.0.0"         → Gte{1.0.0}
//   ">=1.0.0 <2.0.0"  → Gte{1.0.0} Lt{2.0.0}
//   "^1.2.3"          → Gte{1.2.3} Lt{2.0.0}
//   "^0.2.3"          → Gte{0.2.3} Lt{0.3.0}
//   "~1.2.3"          → Gte{1.2.3} Lt{1.3.0}
//   "1.2.*"           → Gte{1.2.0} Lt{1.3.0}
//   "1.*"             → Gte{1.0.0} Lt{2.0.0}
inline std::optional<Range> parse_range(std::string_view expr) {
    while (!expr.empty() && expr.front() == ' ') expr.remove_prefix(1);
    while (!expr.empty() && expr.back() == ' ') expr.remove_suffix(1);
    if (expr.empty()) return std::nullopt;

    Range range;

    // ── Caret: ^1.2.3 ──
    if (expr.starts_with("^")) {
        auto v = parse(expr.substr(1));
        if (!v) return std::nullopt;
        Version lo = *v;
        lo.components = 3;
        Version hi{};
        hi.components = 3;
        if (v->major != 0) {
            hi.major = v->major + 1; // ^1.2.3 → < 2.0.0
        } else if (v->minor != 0) {
            hi.minor = v->minor + 1; // ^0.2.3 → < 0.3.0
        } else {
            hi.patch = v->patch + 1; // ^0.0.3 → < 0.0.4
        }
        range.constraints.push_back({Op::Gte, lo});
        range.constraints.push_back({Op::Lt, hi});
        return range;
    }

    // ── Tilde: ~1.2.3 ──
    if (expr.starts_with("~")) {
        auto v = parse(expr.substr(1));
        if (!v) return std::nullopt;
        Version lo = *v;
        lo.components = 3;
        Version hi{};
        hi.components = 3;
        hi.major = v->major;
        hi.minor = v->minor + 1; // ~1.2.3 → < 1.3.0
        range.constraints.push_back({Op::Gte, lo});
        range.constraints.push_back({Op::Lt, hi});
        return range;
    }

    // ── Wildcard: 1.2.*, 1.* ──
    if (expr.find('*') != std::string_view::npos) {
        // Replace * with 0, parse, then build range
        std::string clean;
        // "1.2.*" → parse "1.2" then make range
        auto star = expr.find('*');
        auto prefix = expr.substr(0, star);
        while (!prefix.empty() && prefix.back() == '.') prefix.remove_suffix(1);
        if (prefix.empty()) return std::nullopt;
        auto v = parse(prefix);
        if (!v) return std::nullopt;
        // Build range from prefix components
        Version lo = *v;
        lo.components = 3;
        Version hi{};
        hi.components = 3;
        if (v->components == 1) {
            hi.major = v->major + 1; // 1.* → < 2.0.0
        } else {
            hi.major = v->major;
            hi.minor = v->minor + 1; // 1.2.* → < 1.3.0
        }
        range.constraints.push_back({Op::Gte, lo});
        range.constraints.push_back({Op::Lt, hi});
        return range;
    }

    // ── Comparison operators: >=, >, <=, <, or space-separated multiple ──
    if (expr.starts_with(">") || expr.starts_with("<")) {
        // Split by spaces for multiple constraints: ">=1.0.0 <2.0.0"
        std::size_t pos = 0;
        while (pos < expr.size()) {
            while (pos < expr.size() && expr[pos] == ' ') ++pos;
            if (pos >= expr.size()) break;

            // Find end of this constraint token
            std::size_t start = pos;
            // Skip operator
            while (pos < expr.size() && (expr[pos] == '>' || expr[pos] == '<' || expr[pos] == '='))
                ++pos;
            // Skip spaces between operator and version
            while (pos < expr.size() && expr[pos] == ' ') ++pos;
            // Skip version chars
            while (pos < expr.size() && expr[pos] != ' ') ++pos;

            auto tok = expr.substr(start, pos - start);
            auto parsed = parse_constraint_token(tok);
            if (!parsed) return std::nullopt;
            range.constraints.push_back({parsed->first, parsed->second});
        }
        return range.constraints.empty() ? std::nullopt : std::optional{range};
    }

    // ── Bare version: "15.1.0" (exact) or "15" / "15.1" (prefix range) ──
    auto v = parse(expr);
    if (!v) return std::nullopt;

    if (v->components == 3 && v->prerelease.empty()) {
        // Full 3-component version → exact match
        range.constraints.push_back({Op::Eq, *v});
    } else if (!v->prerelease.empty()) {
        // Version with prerelease → exact match
        v->components = 3;
        range.constraints.push_back({Op::Eq, *v});
    } else {
        // Partial version (1 or 2 components) → prefix range
        Version lo = *v;
        lo.components = 3;
        Version hi{};
        hi.components = 3;
        if (v->components == 1) {
            hi.major = v->major + 1; // "15" → [15.0.0, 16.0.0)
        } else {
            hi.major = v->major;
            hi.minor = v->minor + 1; // "15.1" → [15.1.0, 15.2.0)
        }
        range.constraints.push_back({Op::Gte, lo});
        range.constraints.push_back({Op::Lt, hi});
    }
    return range;
}

// ── Utility functions ────────────────────────────────────────────────

// Compare two version strings. Returns -1, 0, or 1.
// Falls back to lexicographic comparison for unparseable strings.
inline int compare(std::string_view a, std::string_view b) {
    auto va = parse(a);
    auto vb = parse(b);
    if (va && vb) {
        auto c = compare_versions(*va, *vb);
        if (c < 0) return -1;
        if (c > 0) return 1;
        return 0;
    }
    // Fallback: lexicographic
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// Sort version strings in descending order (highest first).
inline void sort_desc(std::vector<std::string>& versions) {
    std::sort(versions.begin(), versions.end(),
        [](const std::string& a, const std::string& b) {
            return compare(a, b) > 0;
        });
}

// Select the highest version from `available` that satisfies `range_expr`.
// Skips "latest" tags. Returns "" if no match.
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
