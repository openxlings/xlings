// Terms written in English in every language, and where their explanations
// are (design 2026-10-19 §7.5): what a person hovers over.
module i18n;

import std;

namespace i18n {

namespace {
std::vector<std::pair<std::string, std::span<const Term>>>& glossaries_() {
    static std::vector<std::pair<std::string, std::span<const Term>>> g;
    return g;
}
}  // namespace

void register_glossary(std::string_view domain, std::span<const Term> terms) {
    auto& g = glossaries_();
    if (auto it = std::ranges::find(g, domain, &std::pair<std::string, std::span<const Term>>::first); it != g.end())
        it->second = terms;
    else
        g.emplace_back(std::string(domain), terms);
}

std::vector<Term> glossary(std::string_view domain) {
    for (const auto& [name, terms] : glossaries_())
        if (name == domain) return {terms.begin(), terms.end()};
    return {};
}

std::optional<std::string_view> explain(std::string_view term) {
    for (const auto& [_, terms] : glossaries_())
        for (const auto& t : terms)
            if (t.term == term) return tr(t.key);
    return std::nullopt;
}

}  // namespace i18n
