// "auto" has to be able to return something other than "en".
//
// THE BUG THIS LOCKS DOWN
//
// `get_system_language()` used to build `std::locale("")` and catch the
// failure into `return "en"`. Measured on both libcs with LANG=en_US.UTF-8:
//
//     glibc dynamic   setlocale -> (null)          std::locale("") threw
//     musl static     setlocale -> en_US.UTF-8     std::locale("") threw
//
// musl's newlocale() supports only C/POSIX, so libstdc++ throws for every real
// locale name -- and the RELEASED Linux binary is static musl. glibc only
// works when that exact locale was generated on the target machine. So the
// "follow the system language" feature could only ever answer "en", and
// "the system really is English" was indistinguishable from "detection failed".
//
// These tests would fail against that implementation, which is the only reason
// they are worth having.
#include <gtest/gtest.h>

#include <cstdlib>

import std;
import xlings.platform;

namespace {

// setenv/unsetenv spelled once. MSVC has neither; _putenv_s with an empty
// value is its documented removal form.
void set_env_(const char* k, const char* v) {
#if defined(_WIN32)
    ::_putenv_s(k, v ? v : "");
#else
    if (v) ::setenv(k, v, 1); else ::unsetenv(k);
#endif
}

// Restores whatever the harness was started with, so these tests do not leak
// a locale into the rest of the binary.
class LocaleEnv : public ::testing::Test {
protected:
    void SetUp() override {
        for (auto* k : kKeys) {
            const char* v = std::getenv(k);
            saved_.emplace_back(k, v ? std::optional<std::string>{v}
                                     : std::nullopt);
            set_env_(k, nullptr);
        }
    }
    void TearDown() override {
        for (auto& [k, v] : saved_) set_env_(k, v ? v->c_str() : nullptr);
    }
    static constexpr const char* kKeys[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    std::vector<std::pair<const char*, std::optional<std::string>>> saved_;
};

}  // namespace

#if !defined(_WIN32)

TEST_F(LocaleEnv, ChineseSystemIsDetectedAsChinese) {
    set_env_("LANG", "zh_CN.UTF-8");
    // The whole point of the feature. The old implementation returned "en"
    // here on every released Linux binary.
    EXPECT_EQ(xlings::platform::get_system_language(), "zh");
}

TEST_F(LocaleEnv, PrecedenceIsLcAllThenLcMessagesThenLang) {
    set_env_("LANG", "de_DE.UTF-8");
    EXPECT_EQ(xlings::platform::get_system_language(), "de");

    set_env_("LC_MESSAGES", "fr_FR.UTF-8");
    EXPECT_EQ(xlings::platform::get_system_language(), "fr");

    // LC_ALL overrides everything -- that is what it is for, and getting the
    // order wrong shows up only on machines that set more than one.
    set_env_("LC_ALL", "zh_CN.UTF-8");
    EXPECT_EQ(xlings::platform::get_system_language(), "zh");
}

TEST_F(LocaleEnv, CAndPosixMeanNoLocalisation) {
    set_env_("LANG", "C");
    EXPECT_EQ(xlings::platform::get_system_language(), "en");
    set_env_("LANG", "POSIX");
    EXPECT_EQ(xlings::platform::get_system_language(), "en");
}

TEST_F(LocaleEnv, NothingSetFallsBackToEnglish) {
    EXPECT_EQ(xlings::platform::get_system_language(), "en");
}

TEST_F(LocaleEnv, EmptyValueIsTreatedAsUnset) {
    // `LANG=` is how a wrapper script clears an inherited value; treating the
    // empty string as a language would yield "".
    set_env_("LANG", "");
    EXPECT_EQ(xlings::platform::get_system_language(), "en");
}

TEST_F(LocaleEnv, TagIsCutAtTheFirstSeparatorAndLowercased) {
    for (auto* tag : { "zh_CN.UTF-8", "zh-CN", "zh.UTF-8", "zh@pinyin", "ZH_CN" }) {
        set_env_("LANG", tag);
        EXPECT_EQ(xlings::platform::get_system_language(), "zh") << tag;
    }
}

#endif  // !_WIN32

TEST(SystemLanguage, AlwaysAnswersSomething) {
    // Whatever the host is, the answer is a non-empty lowercase tag: callers
    // index a table with it.
    const auto lang = xlings::platform::get_system_language();
    EXPECT_FALSE(lang.empty());
    EXPECT_EQ(lang, std::string(lang | std::views::transform([](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }) | std::ranges::to<std::string>()));
}
