export module xlings.i18n;

import std;
export import :locale;

// Translation lookup (design 2026-10-19 §7): the mechanism, shared by every
// program; the words are each program's, registered here by domain.
//
// KEYS ARE STABLE STRINGS, NOT AN ENUM
//
// The first table was `enum class Msg` plus a parallel array of {en, zh}
// pairs: an enum block edited by every message addition is a permanent merge
// conflict, and adding a language meant adding a column to every row. So:
// string keys, and one catalogue per language and domain. Keys are dotted and
// stable; a domain's keys start with the domain (`luban.os.quick.create`), and
// xlings' own -- the domain "xlings", registered first -- predate that and are
// any key no other domain claims. For a diagnostic the key IS its `diag` code.
export namespace xlings::i18n {

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

// The domains registered, and one's table in a language (empty when none).
[[nodiscard]] std::vector<std::string> domains();
[[nodiscard]] std::span<const Entry> catalogue(std::string_view domain, std::string_view lang);

// Whether some catalogue speaks `lang` (a normalized tag).
[[nodiscard]] bool speaks(std::string_view lang);

// The language: an override variable (set_override_env; LUBAN_OS_LANG for the
// Luban OS app) wins over the setting, which wins over the system. "auto" /
// "" mean follow the system; a tag no catalogue speaks falls back along its
// chain to English. Resolved once, again after a change.
void set_override_env(std::string variable);
void set_language(std::string lang);
[[nodiscard]] const std::string& language();

// The text for `key` in the active language, along its fallback chain to
// English, and `key` itself when English does not have it either.
//
// The last fallback is deliberate: an untranslated key renders as something
// searchable rather than as a blank line. A missing message must not be able
// to erase output.
[[nodiscard]] std::string_view tr(std::string_view key);

template <typename... Args>
[[nodiscard]] std::string trf(std::string_view key, Args&&... args) {
    return std::vformat(tr(key), std::make_format_args(args...));
}

// Every key xlings' English catalogue defines (the domain "xlings").
[[nodiscard]] std::span<const Entry> english_catalogue();

// What is wrong with a domain's catalogues, one line each: an empty English
// text, a dotted key another language has and English does not, a translation
// whose format arguments are not English's. A key written as English text
// (with a space or a capital: "Search results:") is its own English. Empty when nothing is. Each program's
// tests assert it is empty for its domains.
[[nodiscard]] std::vector<std::string> check(std::string_view domain);

// Words that come with data rather than with the program (a recipe's
// `extensions.luban.title = { en = "...", zh = "..." }`): the text for the
// active language along its chain, English, then any. Empty when there is none.
[[nodiscard]] std::string pick(const std::map<std::string, std::string, std::less<>>& texts);

// Terms every language writes in English (Compositor, Wayland, KDE Plasma),
// each with the key of its explanation (design 2026-10-19 §7.5).
struct Term {
    std::string_view term;
    std::string_view key;
};
void register_glossary(std::string_view domain, std::span<const Term> terms);
[[nodiscard]] std::vector<Term> glossary(std::string_view domain);
// The explanation of `term` in the active language, when a glossary has it.
[[nodiscard]] std::optional<std::string_view> explain(std::string_view term);

}  // namespace xlings::i18n

namespace xlings::i18n {
// A catalogue registered: the resolved language is asked again.
void forget_resolved_language();
}  // namespace xlings::i18n
