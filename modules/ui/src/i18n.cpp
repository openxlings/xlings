module xlings.i18n;

import std;
import xlings.platform;
import xlings.i18n.en;
import xlings.i18n.zh;

namespace xlings::i18n {

namespace {

std::string gLang_;

// Linear scan over a constexpr table.
//
// Deliberately not a hash map. The catalogues are tens of entries and every
// lookup happens while something is already about to touch a terminal or the
// filesystem; a map would cost a static initialiser and an allocation to save
// time that is not being spent. Revisit at a few hundred entries per language.
[[nodiscard]] std::optional<std::string_view> find_(std::span<const Entry> tbl,
                                                    std::string_view key) {
    for (const auto& e : tbl) {
        if (e.key == key) return e.text;
    }
    return std::nullopt;
}

[[nodiscard]] std::span<const Entry> overlay_for_(std::string_view lang) {
    if (lang == "zh") return zh::kEntries;
    return {};   // English is the base; nothing to overlay
}

}  // namespace

void set_language(std::string lang) {
    // "auto" and "" both mean follow the system, spelled explicitly because
    // `xlings config --lang auto` is how a user goes BACK to following it, and
    // an unrecognised value must not silently pin.
    if (lang.empty() || lang == "auto") {
        gLang_.clear();          // resolved lazily by language()
        return;
    }
    gLang_ = std::move(lang);
}

[[nodiscard]] const std::string& language() {
    if (gLang_.empty()) {
        gLang_ = platform::get_system_language();
        // Only the languages that have a catalogue resolve to themselves.
        // Everything else is English -- which is the base, so it is a complete
        // answer rather than a degraded one, and saying so on every command
        // would be noise.
        if (overlay_for_(gLang_).empty()) gLang_ = "en";
    }
    return gLang_;
}

[[nodiscard]] std::string_view tr(std::string_view key) {
    if (auto hit = find_(overlay_for_(language()), key)) return *hit;
    if (auto hit = find_(en::kEntries, key)) return *hit;
    // Neither catalogue has it. Render the key: visible, greppable, and
    // impossible to mistake for finished text -- whereas returning "" would
    // silently delete a label and leave a blank column.
    return key;
}

[[nodiscard]] std::span<const Entry> english_catalogue() { return en::kEntries; }

}  // namespace xlings::i18n
