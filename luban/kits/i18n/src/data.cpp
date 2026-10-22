// Words that come with data (design 2026-10-19 §7.4): a recipe's title or
// summary as a table of languages, read along the same chain as a catalogue.
module i18n;

import std;

namespace i18n {

std::string pick(const std::map<std::string, std::string, std::less<>>& texts) {
    if (texts.empty()) return {};
    for (const auto& lang : fallback_chain(language()))
        for (const auto& [tag, text] : texts)
            if (normalize(tag) == lang && !text.empty()) return text;
    for (const auto& [_, text] : texts)
        if (!text.empty()) return text;
    return {};
}

}  // namespace i18n
