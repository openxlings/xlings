// The app's words and the terms it writes in English in every language
// (design 2026-10-19 §7.5), registered before anything is drawn.
module luban.os;

import std;
import xlings.i18n;

namespace luban::os {

namespace {
constexpr xlings::i18n::Term kGlossary[] = {
    { "Compositor", "luban.os.glossary.compositor" },
    { "Wayland", "luban.os.glossary.wayland" },
    { "GPU", "luban.os.glossary.gpu" },
    { "xdesk", "luban.os.glossary.xdesk" },
    { "labwc", "luban.os.glossary.labwc" },
    { "LXQt", "luban.os.glossary.lxqt" },
    { "KDE Plasma", "luban.os.glossary.kde" },
    { "T1", "luban.os.glossary.tier" },
};
}  // namespace

void register_words() {
    xlings::i18n::register_catalogue("luban.os", "en", words_en());
    xlings::i18n::register_catalogue("luban.os", "zh", words_zh());
    xlings::i18n::register_glossary("luban.os", kGlossary);
    // LUBAN_OS_LANG (tests, a person's one-off) wins over the setting.
    xlings::i18n::set_override_env("LUBAN_OS_LANG");
}

void set_language(std::string_view lang) { xlings::i18n::set_language(std::string(lang)); }

}  // namespace luban::os
