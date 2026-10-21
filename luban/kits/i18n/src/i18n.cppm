// i18n: words in a person's language (design 2026-10-19 §7) -- the
// mechanism, shared by every program; the words are each program's,
// registered by domain. Nothing here knows a platform: the system's
// language is the environment's (LC_ALL, LC_MESSAGES, LANG) unless the
// program gives its own source (Windows' user locale).
//
// KEYS ARE STABLE STRINGS, NOT AN ENUM
//
// An enum block edited by every message addition is a permanent merge
// conflict, and adding a language meant adding a column to every row. So:
// string keys, and one catalogue per language and domain. Keys are dotted and
// stable; a domain's keys start with the domain (`luban.os.quick.create`);
// a key no domain claims is the default domain's (set_default_domain).
export module i18n;

import std;

export namespace i18n {

// One catalogue entry. `constexpr`, so a language file is a compile-time table
// and costs no startup work.
struct Entry {
    std::string_view key;
    std::string_view text;
};

// A program's words in one language: called from its main, before any thread
// asks. `entries` must outlive the program (a constexpr table). Registering a
// domain and language again replaces the table.
void register_catalogue(std::string_view domain, std::string_view lang, std::span<const Entry> entries);
// The domain of a key no registered domain's prefix claims.
void set_default_domain(std::string domain);

// The domains registered, and one's table in a language (empty when none).
[[nodiscard]] std::vector<std::string> domains();
[[nodiscard]] std::span<const Entry> catalogue(std::string_view domain, std::string_view lang);

// Whether some catalogue speaks `lang` (a normalized tag).
[[nodiscard]] bool speaks(std::string_view lang);

// The language: an override variable (set_override_env) wins over the
// setting, which wins over the system. "auto" / "" mean follow the system;
// a tag no catalogue speaks falls back along its chain to English. Resolved
// once, again after a change.
void set_override_env(std::string variable);
void set_language(std::string lang);
// Where the system's language comes from (unset: the environment's).
void set_system_language_source(std::function<std::string()> source);
[[nodiscard]] const std::string& language();

// The text for `key` in the active language, along its fallback chain to
// English, and `key` itself when English does not have it either: an
// untranslated key renders as something searchable, never as a blank.
[[nodiscard]] std::string_view tr(std::string_view key);

template <typename... Args>
[[nodiscard]] std::string trf(std::string_view key, Args&&... args) {
    return std::vformat(tr(key), std::make_format_args(args...));
}

// What is wrong with a domain's catalogues, one line each: an empty English
// text, a dotted key another language has and English does not, a translation
// whose format arguments are not English's. A key written as English text
// (with a space or a capital) is its own English. Empty when nothing is.
[[nodiscard]] std::vector<std::string> check(std::string_view domain);

// Words that come with data rather than with the program (a recipe's
// title = { en = "...", zh = "..." }): the text for the active language
// along its chain, English, then any. Empty when there is none.
[[nodiscard]] std::string pick(const std::map<std::string, std::string, std::less<>>& texts);

// Terms every language writes in English (Compositor, Wayland), each with
// the key of its explanation.
struct Term {
    std::string_view term;
    std::string_view key;
};
void register_glossary(std::string_view domain, std::span<const Term> terms);
[[nodiscard]] std::vector<Term> glossary(std::string_view domain);
// The explanation of `term` in the active language, when a glossary has it.
[[nodiscard]] std::optional<std::string_view> explain(std::string_view term);

// Language tags: "zh_CN.UTF-8", "zh-cn" -> "zh-CN"; "C", "POSIX" -> "en";
// "" and "auto" stay "" (follow the system).
[[nodiscard]] std::string normalize(std::string_view tag);
// The languages to look a word up in, most specific first, English last.
[[nodiscard]] std::vector<std::string> fallback_chain(std::string_view tag);

}  // namespace i18n

namespace i18n {
// A catalogue registered: the resolved language is asked again.
void forget_resolved_language();
}  // namespace i18n
