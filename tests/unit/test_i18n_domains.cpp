// The shared i18n mechanism and the domains that use it (design 2026-10-19
// §7): each registers its words; English is whole; no key without English.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.i18n;
import luban.service;

XTEST(I18nDomains, EachDomainsWordsAreWholeInEnglish, .area = "core", .covers = {"I18N-DOMAINS"}) {
    using xlings::i18n::Entry;
    luban::service::register_words();
    xlings::i18n::set_language("en");
    // luban's: every word there, and the domain checks clean.
    for (const auto& e : luban::service::words_en()) {
        EXPECT_FALSE(e.text.empty()) << e.key;
        EXPECT_EQ(xlings::i18n::tr(e.key), e.text) << e.key;
    }
    const auto problems = xlings::i18n::check("luban");
    EXPECT_TRUE(problems.empty()) << problems.front();
    EXPECT_EQ(xlings::i18n::trf("luban.notes.relayout", 12), "keeps what it was before its move into its own userland (12 MB)");
    // A language without the word: English, per key; a key nobody wrote: itself.
    static constexpr Entry zh[] = {{"luban.status.versions", "版本"}};
    xlings::i18n::register_catalogue("luban", "zh", zh);
    xlings::i18n::set_language("zh");
    EXPECT_EQ(xlings::i18n::tr("luban.status.versions"), "版本");
    EXPECT_EQ(xlings::i18n::tr("luban.status.network"), "network");
    EXPECT_EQ(xlings::i18n::tr("luban.nobody.wrote.this"), "luban.nobody.wrote.this");
    // Data's words (a recipe's title) along the same chain.
    EXPECT_EQ(xlings::i18n::pick({{"en", "Compositor"}, {"zh", "合成器"}}), "合成器");
    xlings::i18n::set_language("en");
}
