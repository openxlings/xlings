// xdesk.apps (Luban OS desktop part 2 §2.4): the applications a desktop
// offers, from freedesktop's desktop entries -- found, searched, started.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xdesk.apps;

namespace tk = xlings::testkit;

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

// An entry reached through a link has the id of its name where it is, not of
// where the link points (E9: the launcher said `xlings-data-xpkgs-...-foot`).
XTEST(XdeskApps, AnEntryThroughALinkKeepsItsOwnId, .area = "luban", .covers = {"XDESK-APPS"}) {
    const auto root = fs::temp_directory_path() / std::format("xdesk-link-{}", std::random_device{}());
    const auto store = root / "store" / "foot" / "share" / "applications", sys = root / "sys" / "applications";
    fs::create_directories(store);
    fs::create_directories(sys);
    std::ofstream(store / "foot.desktop") << "[Desktop Entry]\nName=Foot\nExec=foot\n";
    fs::create_symlink(store / "foot.desktop", sys / "foot.desktop");
    const auto found = apps::scan({sys});
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].id, "foot");
    std::error_code ec;
    fs::remove_all(root, ec);
}

// The default terminal (design 2026-10-18 §6.3, E21): from xdg-terminals.list,
// else the first entry; a Terminal=true application runs in it titled and
// identified as itself, as the terminal's X-TerminalArg* keys say.
XTEST(XdeskApps, AnApplicationRunsInTheDefaultTerminalUnderItsOwnName, .area = "luban", .covers = {"XDESK-APPS"}) {
    // XDG directory lists are ':'-separated: a Linux desktop's (a Windows
    // path has a ':' of its own).
    if constexpr (tk::is_windows) GTEST_SKIP() << "XDG lists of Windows paths";
    const auto root = fs::temp_directory_path() / std::format("xdesk-term-{}", std::random_device{}());
    const auto data = root / "share", config = root / "config";
    fs::create_directories(data / "xdg-terminals");
    fs::create_directories(config);
    std::ofstream(data / "xdg-terminals" / "foot.desktop")
        << "[Desktop Entry]\nName=Terminal\nExec=foot\nX-TerminalArgExec=\nX-TerminalArgTitle=--title=\nX-TerminalArgAppId=--app-id=\n";
    std::ofstream(data / "xdg-terminals" / "xterm.desktop") << "[Desktop Entry]\nName=XTerm\nExec=xterm\n";
    const std::map<std::string, std::string> env{{"HOME", (root / "home").string()}, {"XDG_DATA_DIRS", data.string()},
                                                 {"XDG_CONFIG_DIRS", config.string()}};
    // No list: the first entry (by name).
    auto t = apps::default_terminal(env);
    ASSERT_TRUE(t);
    EXPECT_EQ(apps::in_terminal(*t, {"nvim", "a b"}, "Neovim", "nvim"),
              (std::vector<std::string>{"foot", "--title=Neovim", "--app-id=nvim", "nvim", "a b"}));
    EXPECT_EQ(apps::in_terminal(*t, {}), (std::vector<std::string>{"foot"}));
    // A list names another; one that declares nothing takes -e and no title.
    std::ofstream(config / "xdg-terminals.list") << "# mine\nmissing.desktop\nxterm.desktop\n";
    t = apps::default_terminal(env);
    ASSERT_TRUE(t);
    EXPECT_EQ(apps::in_terminal(*t, {"htop"}, "Htop", "htop"), (std::vector<std::string>{"xterm", "-e", "htop"}));
    // A configured terminal takes the title and the id as its entry says
    // (found by program name), its own arguments kept; an unknown one only
    // takes the command (F19: `terminal = foot` gave nvim no title).
    EXPECT_EQ(apps::in_terminal(apps::configured_terminal(env, {"foot", "--config=/etc/foot.ini"}), {"nvim"}, "Neovim", "nvim"),
              (std::vector<std::string>{"foot", "--config=/etc/foot.ini", "--title=Neovim", "--app-id=nvim", "nvim"}));
    EXPECT_EQ(apps::in_terminal(apps::configured_terminal(env, {"/usr/bin/xterm"}), {"htop"}, "Htop", "htop"),
              (std::vector<std::string>{"/usr/bin/xterm", "-e", "htop"}));
    EXPECT_EQ(apps::in_terminal(apps::configured_terminal(env, {"kitty"}), {"htop"}, "Htop", "htop"),
              (std::vector<std::string>{"kitty", "htop"}));
    // None declared: none.
    EXPECT_FALSE(apps::default_terminal({{"HOME", "/nonexistent"}, {"XDG_DATA_DIRS", (root / "none").string()},
                                         {"XDG_CONFIG_DIRS", (root / "none").string()}}));
    std::error_code ec;
    fs::remove_all(root, ec);
}

// The switcher's icons (design 2026-10-20 §10.1): Icon= found as the icon
// theme specification finds a PNG -- the size asked for or the next larger,
// hicolor before the others, pixmaps last; an SVG-only icon is none.
XTEST(XdeskApps, AnIconIsThePngNearestTheSizeInTheThemes, .area = "luban", .covers = {"XDESK-SWITCHERS", "XDESK-ICONS"}, .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("xdesk-icons");
    const auto share = home.root() / "share";
    for (const auto& f : {"icons/hicolor/32x32/apps/foot.png", "icons/hicolor/64x64/apps/foot.png", "icons/hicolor/256x256/apps/foot.png",
                          "icons/breeze/48x48/apps/foot.png", "icons/hicolor/scalable/apps/vector.svg", "pixmaps/old.png"})
        tk::write_file(share / f, "png");
    const std::map<std::string, std::string> env{{"XDG_DATA_HOME", (home.root() / "nothing").string()}, {"XDG_DATA_DIRS", share.string()}};
    EXPECT_EQ(apps::icon_file(env, "foot", 48), share / "icons/hicolor/64x64/apps/foot.png") << "the next larger, in hicolor";
    EXPECT_EQ(apps::icon_file(env, "foot", 512), share / "icons/hicolor/256x256/apps/foot.png") << "the largest there is";
    EXPECT_EQ(apps::icon_file(env, "old", 48), share / "pixmaps/old.png");
    // Scalable: drawn at the size shown (design 2026-10-24 §9.5); a PNG of
    // the theme first, the SVG before the pixmaps.
    EXPECT_EQ(apps::icon_file(env, "vector", 48), share / "icons/hicolor/scalable/apps/vector.svg") << "an SVG when there is no PNG";
    tk::write_file(share / "pixmaps/vector.png", "png");
    EXPECT_EQ(apps::icon_file(env, "vector", 48), share / "icons/hicolor/scalable/apps/vector.svg") << "the theme's SVG before the pixmaps";
    EXPECT_EQ(apps::icon_file(env, (share / "icons/hicolor/scalable/apps/vector.svg").string(), 48), share / "icons/hicolor/scalable/apps/vector.svg");
    EXPECT_EQ(apps::icon_file(env, (share / "pixmaps/old.png").string(), 48), share / "pixmaps/old.png") << "a path as it is";
    EXPECT_FALSE(apps::icon_file(env, "", 48));
}
