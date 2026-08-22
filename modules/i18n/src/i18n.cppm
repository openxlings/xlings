export module xlings.i18n;

import std;

// Translation lookup.
//
// KEYS ARE STABLE STRINGS, NOT AN ENUM
//
// The previous table was `enum class Msg` plus a parallel array of {en, zh}
// pairs, with a static_assert tying their sizes. It had 36 entries and ZERO
// call sites -- 36 finished Chinese translations that no code could reach, so
// "xlings has no Chinese" was true even though every string existed.
//
// It also could not have scaled: there are ~590 user-visible output points,
// and an enum block edited by every message addition is a permanent merge
// conflict. Worse, adding a language means adding a COLUMN, which rewrites
// every row.
//
// So: string keys, and one catalogue per language (see i18n/en.cppm). Keys are
// dotted and stable, and for a diagnostic the key IS its `diag` code -- that
// field is already mandatory and already searchable, so a new diagnostic
// cannot forget to have an id.
export namespace xlings::i18n {

// One catalogue entry. `constexpr`, so a language file is a compile-time table
// and costs no startup work.
struct Entry {
    std::string_view key;
    std::string_view text;
};

// "auto" / "" mean follow the system. An unrecognised name pins nothing and
// falls back to English rather than indexing a catalogue that does not exist.
void set_language(std::string lang);

[[nodiscard]] const std::string& language();

// The text for `key` in the active language, falling back to English, and to
// `key` itself when English does not have it either.
//
// The last fallback is deliberate: an untranslated key renders as something
// searchable rather than as a blank line. A missing message must not be able
// to erase output.
[[nodiscard]] std::string_view tr(std::string_view key);

template <typename... Args>
[[nodiscard]] std::string trf(std::string_view key, Args&&... args) {
    return std::vformat(tr(key), std::make_format_args(args...));
}

// Every key the English catalogue defines. Used by the test that pins "en is
// complete" -- the base has to be total or the last-resort fallback above
// starts leaking keys into the UI.
[[nodiscard]] std::span<const Entry> english_catalogue();

}  // namespace xlings::i18n
