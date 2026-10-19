// Language tags (design 2026-10-19 §7.2): what a setting, an environment
// variable or the system says, as one spelling, and the languages to try for
// it in order.
export module xlings.i18n:locale;

import std;

export namespace xlings::i18n {

// "zh_CN.UTF-8", "zh-cn", "zh_CN@pinyin" -> "zh-CN"; "C", "POSIX" -> "en";
// "" and "auto" stay "" (follow the system).
[[nodiscard]] std::string normalize(std::string_view tag);

// The languages to look a word up in, most specific first, English last:
// "zh-CN" -> {"zh-CN", "zh", "en"}; "en-GB" -> {"en-GB", "en"}; "" -> {"en"}.
[[nodiscard]] std::vector<std::string> fallback_chain(std::string_view tag);

}  // namespace xlings::i18n
