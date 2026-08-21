module xlings.core.i18n;

import std;
import xlings.platform;

namespace xlings::i18n {

std::string gLang_;

void set_language(const std::string& lang) {
    // "auto" and "" both mean "follow the system". Spelled explicitly because
    // `xlings config --lang auto` is how a user goes BACK to following it after
    // pinning a language, and an unrecognised value must not silently pin.
    if (lang.empty() || lang == "auto") {
        gLang_.clear();          // resolved lazily by language()
        return;
    }
    gLang_ = lang;
}

[[nodiscard]] const std::string& language() {
    if (gLang_.empty()) {
        gLang_ = platform::get_system_language();
        // Only two translations exist. Anything else falls back to English
        // rather than indexing a table that has no column for it -- and the
        // fallback is silent on purpose: a French user getting English is the
        // expected outcome, not a problem to report on every command.
        if (gLang_ != "zh") {
            gLang_ = "en";
        }
    }
    return gLang_;
}

[[nodiscard]] bool is_chinese() {
    return language() == "zh";
}

[[nodiscard]] std::string_view tr(Msg id) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= MSG_COUNT) return "";
    return is_chinese() ? gMessages_[idx].zh : gMessages_[idx].en;
}

}
