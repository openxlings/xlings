// xdesk's words through the i18n kit (luban/kits/i18n): the desktop's text is
// its English, each word it shows has its Chinese, the catalogues check
// clean, and xlings' words go through the same mechanism (xlings.i18n is
// the kit with xlings' default domain).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import i18n;
import xdesk.words;

namespace fs = std::filesystem;

XTEST(XdeskWords, EveryWordTheDesktopShowsHasItsChineseAndTheCataloguesCheckClean, .area = "luban", .covers = {"XDESK-WORDS"}) {
    xdesk::words::register_words();
    EXPECT_TRUE(i18n::check("xdesk").empty()) << i18n::check("xdesk").front();
    // Every literal the desktop gives i18n::tr or trf, in its Chinese table.
    std::set<std::string, std::less<>> zh;
    for (const auto& e : i18n::catalogue("xdesk", "zh")) zh.emplace(e.key);
    const std::regex call(R"re(i18n::trf?\(\s*"((?:[^"\\]|\\.)*)")re");
    std::vector<std::string> missing;
    const auto root = fs::current_path() / "luban" / "xdesk";
    ASSERT_TRUE(fs::exists(root)) << "run from the repository's root";
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || (e.path().extension() != ".cpp" && e.path().extension() != ".cppm")) continue;
        std::ifstream in(e.path());
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        for (auto it = std::sregex_iterator(text.begin(), text.end(), call); it != std::sregex_iterator(); ++it) {
            std::string key = (*it)[1].str();
            // The source's escapes, as the compiler reads them.
            for (std::size_t k = 0; (k = key.find("\\n", k)) != std::string::npos;) key.replace(k, 2, "\n");
            for (std::size_t k = 0; (k = key.find("\\\""), k) != std::string::npos;) key.replace(k, 2, "\"");
            if (!zh.contains(key)) missing.push_back(std::format("{}: {}", e.path().filename().string(), key));
        }
    }
    EXPECT_TRUE(missing.empty()) << missing.size() << " without Chinese, the first: " << missing.front();
    // The language: Chinese read as Chinese, English as itself.
    i18n::set_language("zh-CN");
    EXPECT_TRUE(xdesk::words::chinese());
    EXPECT_EQ(i18n::tr("Apps"), "应用");
    EXPECT_EQ(i18n::trf("Workspace {}", 3), "工作区 3");
    i18n::set_language("en");
    EXPECT_FALSE(xdesk::words::chinese());
    EXPECT_EQ(i18n::tr("Apps"), "Apps");
    i18n::set_language("");
}
