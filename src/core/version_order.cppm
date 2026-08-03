export module xlings.core.version_order;

import std;

export namespace xlings::version_order {

namespace detail_ {

struct ParsedVersion {
    std::vector<std::string_view> components;
    std::string_view prerelease;
};

std::optional<ParsedVersion> parse_(std::string_view value) {
    if (value.empty()) return std::nullopt;

    ParsedVersion parsed;
    auto dash = value.find('-');
    auto numeric = value.substr(0, dash);
    if (numeric.empty()) return std::nullopt;
    if (dash != std::string_view::npos) {
        parsed.prerelease = value.substr(dash + 1);
        if (parsed.prerelease.empty()) return std::nullopt;
    }

    std::size_t start = 0;
    while (start <= numeric.size()) {
        auto dot = numeric.find('.', start);
        auto component = numeric.substr(
            start, dot == std::string_view::npos ? numeric.size() - start
                                                  : dot - start);
        if (component.empty()
            || !std::ranges::all_of(component, [](char c) {
                   return c >= '0' && c <= '9';
               })) {
            return std::nullopt;
        }
        while (component.size() > 1 && component.front() == '0') {
            component.remove_prefix(1);
        }
        parsed.components.push_back(component);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parsed;
}

std::strong_ordering compare_component_(std::string_view lhs,
                                        std::string_view rhs) {
    if (lhs.size() != rhs.size()) return lhs.size() <=> rhs.size();
    return lhs <=> rhs;
}

}  // namespace detail_

std::strong_ordering compare(std::string_view lhs, std::string_view rhs) {
    auto left = detail_::parse_(lhs);
    auto right = detail_::parse_(rhs);

    if (left && !right) return std::strong_ordering::greater;
    if (!left && right) return std::strong_ordering::less;
    if (!left && !right) return lhs <=> rhs;

    const auto count = std::max(left->components.size(),
                                right->components.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto l = i < left->components.size()
            ? left->components[i] : std::string_view{"0"};
        const auto r = i < right->components.size()
            ? right->components[i] : std::string_view{"0"};
        if (auto order = detail_::compare_component_(l, r); order != 0) {
            return order;
        }
    }

    if (left->prerelease.empty() && !right->prerelease.empty()) {
        return std::strong_ordering::greater;
    }
    if (!left->prerelease.empty() && right->prerelease.empty()) {
        return std::strong_ordering::less;
    }
    return left->prerelease <=> right->prerelease;
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
