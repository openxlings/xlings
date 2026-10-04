export module xlings.subos.model;

import std;

// The pure part of the instance model (design §23): how a name a person or
// an agent typed resolves to an instance, and what to suggest when it does
// not. Works on names alone, so it is tested without a home and shared by
// every surface that resolves one (use, exec, start, remove, the interface).
export namespace xlings::subos::model {

struct NameResolution {
    std::string selected;               // empty when nothing was chosen
    // missing_name | exact | case_insensitive_exact | unique_prefix |
    // ambiguous | not_found -- the interface reports this verbatim.
    std::string reason;
    std::vector<std::string> matches;   // the candidates worth showing, in order
    bool autoSelected { false };        // chosen by a rule weaker than "exact"
};

std::size_t edit_distance(std::string_view lhs, std::string_view rhs);

// Up to `max` names, related ones (substring either way) first, then by edit
// distance, then by name.
std::vector<std::string> suggestions(std::string_view query,
                                     std::span<const std::string> names,
                                     std::size_t max = 3);

// `names` sorted. Exact wins; then a unique case-insensitive match; then a
// unique prefix; more than one at a level is "ambiguous"; none is
// "not_found" with suggestions.
NameResolution resolve_name(std::string_view query, std::span<const std::string> names);

}  // namespace xlings::subos::model

namespace xlings::subos::model {

namespace {

std::string lowercase(std::string_view name) {
    std::string lowered(name);
    for (auto& ch : lowered) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return lowered;
}

}  // namespace

std::size_t edit_distance(std::string_view lhs, std::string_view rhs) {
    std::vector<std::size_t> prev(rhs.size() + 1);
    std::vector<std::size_t> next(rhs.size() + 1);
    std::iota(prev.begin(), prev.end(), std::size_t{0});
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        next[0] = i + 1;
        for (std::size_t j = 0; j < rhs.size(); ++j) {
            const auto replace = prev[j] + (lhs[i] == rhs[j] ? 0u : 1u);
            next[j + 1] = std::min({prev[j + 1] + 1, next[j] + 1, replace});
        }
        std::swap(prev, next);
    }
    return prev.back();
}

std::vector<std::string> suggestions(std::string_view query,
                                     std::span<const std::string> names,
                                     std::size_t max) {
    struct Scored {
        int substringRank;
        std::size_t distance;
        std::string name;
    };
    const auto loweredQuery = lowercase(query);
    std::vector<Scored> scored;
    scored.reserve(names.size());
    for (const auto& name : names) {
        const auto lowered = lowercase(name);
        const bool related = lowered.contains(loweredQuery) || loweredQuery.contains(lowered);
        scored.push_back({related ? 0 : 1, edit_distance(loweredQuery, lowered), name});
    }
    std::ranges::sort(scored, [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.substringRank, lhs.distance, lhs.name)
             < std::tie(rhs.substringRank, rhs.distance, rhs.name);
    });
    std::vector<std::string> result;
    for (std::size_t i = 0; i < std::min(max, scored.size()); ++i)
        result.push_back(std::move(scored[i].name));
    return result;
}

NameResolution resolve_name(std::string_view query, std::span<const std::string> names) {
    if (query.empty()) {
        return {.reason = "missing_name", .matches = {names.begin(), names.end()}};
    }
    if (std::ranges::find(names, query) != names.end()) {
        return {.selected = std::string(query), .reason = "exact",
                .matches = {std::string(query)}};
    }

    const auto loweredQuery = lowercase(query);
    std::vector<std::string> matches;
    for (const auto& name : names)
        if (lowercase(name) == loweredQuery) matches.push_back(name);
    if (matches.size() == 1) {
        return {.selected = matches.front(), .reason = "case_insensitive_exact",
                .matches = std::move(matches), .autoSelected = true};
    }
    if (matches.size() > 1) return {.reason = "ambiguous", .matches = std::move(matches)};

    for (const auto& name : names)
        if (lowercase(name).starts_with(loweredQuery)) matches.push_back(name);
    if (matches.size() == 1) {
        return {.selected = matches.front(), .reason = "unique_prefix",
                .matches = std::move(matches), .autoSelected = true};
    }
    if (matches.size() > 1) return {.reason = "ambiguous", .matches = std::move(matches)};

    return {.reason = "not_found", .matches = suggestions(query, names)};
}

}  // namespace xlings::subos::model
