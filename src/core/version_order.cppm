export module xlings.core.version_order;

import std;
import xlings.core.semver;

// A thin, stable facade over the ONE version comparator.
//
// This module used to carry its own N-component numeric parser, which meant
// the codebase had two answers to "which version is newer" — semver's and
// this one — differing exactly on the inputs nobody tests (alpha segments,
// sentinels, malformed strings). Two comparators is the reporter/repairer
// drift pattern wearing a different hat, so this one now delegates.
//
// What this module contributed to the merged semantics, and semver now
// honors: a parseable version always outranks an unparseable string
// (sentinels like "latest" and "res_versioned" must never win a version
// comparison), and two unparseable strings tiebreak lexicographically so
// sorts stay deterministic.
export namespace xlings::version_order {

std::strong_ordering compare(std::string_view lhs, std::string_view rhs) {
    const int c = semver::compare(lhs, rhs);
    if (c < 0) return std::strong_ordering::less;
    if (c > 0) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

bool is_internal_key(std::string_view value) {
    return value == "res_versioned";
}

void sort_desc(std::vector<std::string>& versions) {
    std::ranges::stable_sort(versions, [](const auto& lhs, const auto& rhs) {
        return compare(lhs, rhs) > 0;
    });
}

}  // namespace xlings::version_order
