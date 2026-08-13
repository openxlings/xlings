module xlings.core.version_order;

import std;
import xlings.core.semver;

namespace xlings::version_order {

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

}
