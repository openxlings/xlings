module xlings.core.i18n;

import std;
import xlings.platform;

namespace xlings::i18n {

std::string gLang_;

void set_language(const std::string& lang) {
    gLang_ = lang;
}

[[nodiscard]] const std::string& language() {
    if (gLang_.empty()) {
        gLang_ = platform::get_system_language();
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
