module xlings.i18n;

import std;

namespace xlings::i18n {

namespace {

struct Registry {
    // domain -> language -> table, in the order domains were registered.
    std::vector<std::pair<std::string, std::map<std::string, std::span<const Entry>, std::less<>>>> domains;
};

Registry& registry_() {
    static Registry r;
    return r;
}

auto* domain_(std::string_view name) {
    auto& d = registry_().domains;
    auto it = std::ranges::find(d, name, &std::pair<std::string, std::map<std::string, std::span<const Entry>, std::less<>>>::first);
    return it == d.end() ? nullptr : &it->second;
}

// Linear scan over a constexpr table.
//
// Deliberately not a hash map. The catalogues are a few hundred entries and
// every lookup happens while something is already about to touch a terminal or
// the filesystem; a map would cost a static initialiser and an allocation to
// save time that is not being spent.
std::optional<std::string_view> find_(std::span<const Entry> tbl, std::string_view key) {
    for (const auto& e : tbl)
        if (e.key == key) return e.text;
    return std::nullopt;
}

// The domain a key belongs to: the longest registered domain it starts with
// ("luban.os.quick.create" -> "luban.os"), else xlings' own.
std::string_view domain_of_(std::string_view key) {
    std::string_view best;
    for (const auto& [name, _] : registry_().domains)
        if (name != "xlings" && name.size() > best.size() && key.size() > name.size() && key.starts_with(name)
            && key[name.size()] == '.')
            best = name;
    return best.empty() ? std::string_view("xlings") : best;
}

// The `{}`/`{0}` arguments a text takes, counted as std::format reads them.
int arguments_(std::string_view text) {
    int n = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            if (i + 1 < text.size() && text[i + 1] == '{') { ++i; continue; }
            ++n;
        }
    }
    return n;
}

}  // namespace

void register_catalogue(std::string_view domain, std::string_view lang, std::span<const Entry> entries) {
    auto* d = domain_(domain);
    if (!d) {
        registry_().domains.emplace_back(std::string(domain), std::map<std::string, std::span<const Entry>, std::less<>>{});
        d = &registry_().domains.back().second;
    }
    (*d)[normalize(lang)] = entries;
    forget_resolved_language();
}

std::vector<std::string> domains() {
    std::vector<std::string> out;
    for (const auto& [name, _] : registry_().domains) out.push_back(name);
    return out;
}

std::span<const Entry> catalogue(std::string_view domain, std::string_view lang) {
    if (auto* d = domain_(domain))
        if (auto it = d->find(normalize(lang)); it != d->end()) return it->second;
    return {};
}

bool speaks(std::string_view lang) {
    for (const auto& [_, langs] : registry_().domains)
        if (langs.contains(lang)) return true;
    return false;
}

std::string_view tr(std::string_view key) {
    if (auto* d = domain_(domain_of_(key)))
        for (const auto& lang : fallback_chain(language()))
            if (auto it = d->find(lang); it != d->end())
                if (auto hit = find_(it->second, key)) return *hit;
    // Nobody has it. Render the key: visible, greppable, and impossible to
    // mistake for finished text -- whereas returning "" would silently delete
    // a label and leave a blank column.
    return key;
}

std::span<const Entry> english_catalogue() { return catalogue("xlings", "en"); }

std::vector<std::string> check(std::string_view domain) {
    std::vector<std::string> problems;
    auto* d = domain_(domain);
    if (!d) return {std::format("{}: no catalogue registered", domain)};
    const auto en = d->find("en");
    if (en == d->end()) return {std::format("{}: no English catalogue", domain)};
    std::set<std::string_view> keys;
    for (const auto& e : en->second) {
        if (e.text.empty()) problems.push_back(std::format("{}: {} is empty in English", domain, e.key));
        if (!keys.insert(e.key).second) problems.push_back(std::format("{}: {} twice in English", domain, e.key));
        if (domain != "xlings" && !e.key.starts_with(std::string(domain) + "."))
            problems.push_back(std::format("{}: {} does not start with its domain", domain, e.key));
    }
    for (const auto& [lang, table] : *d) {
        if (lang == "en") continue;
        for (const auto& e : table) {
            // A key written as English text ("Search results:") is its own
            // English: it renders as itself when English has no entry.
            const bool ownEnglish = std::ranges::any_of(e.key, [](char c) { return c == ' ' || (c >= 'A' && c <= 'Z'); });
            auto base = find_(en->second, e.key);
            if (!base && ownEnglish) base = e.key;
            if (!base) problems.push_back(std::format("{}: {} in {} is not in English", domain, e.key, lang));
            else if (arguments_(*base) != arguments_(e.text))
                problems.push_back(std::format("{}: {} in {} takes {} arguments, English {}", domain, e.key, lang,
                                               arguments_(e.text), arguments_(*base)));
        }
    }
    return problems;
}

}  // namespace xlings::i18n
