// xdesk.apps (Luban OS desktop part 2 §2.4): the applications a desktop
// offers, from freedesktop's desktop entries -- found, searched, started.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xdesk.apps;

namespace apps = xdesk::apps;
namespace fs = std::filesystem;

XTEST(XdeskApps, ADesktopEntryIsReadInTheUsersLanguage, .area = "luban", .covers = {"XDESK-APPS"}) {
    const auto a = apps::parse("[Desktop Entry]\nType=Application\nName=Files\nName[zh]=文件\nName[zh_CN]=文件管理\n"
                               "Comment=Browse\nComment[zh_CN]=浏览文件\nExec=nautilus %U\nKeywords=folder;explorer;\n"
                               "Terminal=false\n[Desktop Action new]\nExec=nope\n",
                               "org.gnome.Nautilus", "zh_CN.UTF-8");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->name, "文件管理");          // the full locale over the language
    EXPECT_EQ(a->comment, "浏览文件");
    EXPECT_EQ(a->exec, "nautilus %U");      // the entry's, not an action's
    EXPECT_EQ(apps::parse("[Desktop Entry]\nName=Files\nName[zh]=文件\nExec=x\n", "f", "zh_TW")->name, "文件");
    EXPECT_EQ(apps::parse("[Desktop Entry]\nName=Files\nName[zh]=文件\nExec=x\n", "f", "")->name, "Files");
    // What is not shown.
    EXPECT_FALSE(apps::parse("[Desktop Entry]\nName=x\nExec=x\nNoDisplay=true\n", "x"));
    EXPECT_FALSE(apps::parse("[Desktop Entry]\nName=x\nExec=x\nHidden=true\n", "x"));
    EXPECT_FALSE(apps::parse("[Desktop Entry]\nName=x\nType=Link\nURL=y\n", "x"));
    EXPECT_FALSE(apps::parse("[Desktop Entry]\nName=x\n", "x"));
}

XTEST(XdeskApps, ExecBecomesArgvWithoutFieldCodes, .area = "luban", .covers = {"XDESK-APPS"}) {
    EXPECT_EQ(apps::argv("foot"), (std::vector<std::string>{"foot"}));
    EXPECT_EQ(apps::argv("nvim %F"), (std::vector<std::string>{"nvim"}));
    EXPECT_EQ(apps::argv("app --file=%f --x"), (std::vector<std::string>{"app", "--file=", "--x"}));
    EXPECT_EQ(apps::argv(R"(sh -c "echo \"hi there\"; exit")"), (std::vector<std::string>{"sh", "-c", "echo \"hi there\"; exit"}));
    EXPECT_EQ(apps::argv("printf 100%%"), (std::vector<std::string>{"printf", "100%"}));
    EXPECT_TRUE(apps::argv("").empty());
}

XTEST(XdeskApps, TheMoreSpecificDirectoryWinsAndSearchRanksTheName, .area = "luban", .covers = {"XDESK-APPS"}) {
    const auto root = fs::temp_directory_path() / std::format("xdesk-apps-{}", std::random_device{}());
    const auto user = root / "user" / "applications", sys = root / "sys" / "applications";
    fs::create_directories(user);
    fs::create_directories(sys / "kde");
    std::ofstream(sys / "foot.desktop") << "[Desktop Entry]\nName=Foot\nGenericName=Terminal\nExec=foot\n";
    std::ofstream(sys / "nvim.desktop") << "[Desktop Entry]\nName=Neovim\nKeywords=editor;text;\nExec=nvim %F\nTerminal=true\n";
    std::ofstream(sys / "htop.desktop") << "[Desktop Entry]\nName=Htop\nComment=Process viewer\nExec=htop\n";
    std::ofstream(sys / "kde" / "konsole.desktop") << "[Desktop Entry]\nName=Konsole\nGenericName=Terminal Emulator\nExec=konsole\n";
    // The user's own hides the system's htop.
    std::ofstream(user / "htop.desktop") << "[Desktop Entry]\nName=Htop\nExec=htop\nNoDisplay=true\n";
    const auto found = apps::scan({user, sys});
    std::vector<std::string> ids;
    for (const auto& a : found) ids.push_back(a.id);
    EXPECT_EQ(ids, (std::vector<std::string>{"foot", "kde-konsole", "nvim"}));   // by name; htop hidden
    auto names = [](const std::vector<const apps::App*>& v) {
        std::vector<std::string> out;
        for (const auto* a : v) out.push_back(a->name);
        return out;
    };
    EXPECT_EQ(names(apps::search(found, "term")), (std::vector<std::string>{"Foot", "Konsole"}));   // generic names
    // A name's prefix, then anywhere in a name, then a generic name.
    EXPECT_EQ(names(apps::search(found, "N")), (std::vector<std::string>{"Neovim", "Konsole", "Foot"}));
    EXPECT_EQ(names(apps::search(found, "editor")), (std::vector<std::string>{"Neovim"}));          // keywords
    EXPECT_EQ(apps::search(found, "").size(), 3u);
    EXPECT_TRUE(apps::search(found, "zzz").empty());
    // Where they are looked for.
    const auto d = apps::dirs({{"HOME", "/home/u"}, {"XDG_DATA_DIRS", "/a:/b"}});
    EXPECT_EQ(d, (std::vector<fs::path>{"/home/u/.local/share/applications", "/a/applications", "/b/applications"}));
    std::error_code ec;
    fs::remove_all(root, ec);
}
