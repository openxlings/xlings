// Who a Luban system is and who logs in (part 3 §6, §7): its users from its
// passwd, its host name, the prompt's mark, the welcome's shape.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.root_etc;
import luban.service;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace etc = xlings::subos::root_etc;

XTEST(LubanIdentity, AUserIsMadeInTheSystemsOwnEtc, .area = "luban", .covers = {"USER-LOGIN", "USER-PASSWORD"}) {
    auto home = tk::Home::isolated("luban-user");
    const auto tree = home.dir() / "tree";
    tk::write_file(tree / "etc/passwd", "root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:Nobody:/:/bin/false\n");
    tk::write_file(tree / "etc/group", "root:x:0:\nwheel:x:10:\n");
    EXPECT_FALSE(etc::valid_user_name("Alice"));
    EXPECT_FALSE(etc::valid_user_name("1x"));
    EXPECT_TRUE(etc::valid_user_name("lubanoser"));
    const auto hash = std::string("$6$salt$abc");
    auto made = etc::add_user(tree, "alice", "/bin/sh", hash, 4242);
    ASSERT_TRUE(made) << made.error();
    EXPECT_EQ(made->uid, 4242u) << "the host user's uid when free";
    EXPECT_EQ(made->home, "/home/alice");
    EXPECT_TRUE(fs::is_directory(tree / "home/alice"));
    EXPECT_NE(tk::read_file(tree / "etc/group").find("wheel:x:10:alice"), std::string::npos);
    const auto shadow = tk::read_file(tree / "etc/shadow");
    EXPECT_NE(shadow.find("alice:" + hash + ":"), std::string::npos) << shadow;
    EXPECT_NE(shadow.find("root:!:"), std::string::npos) << "root stays locked: its way in is --user root";
    EXPECT_FALSE(etc::add_user(tree, "alice", "/bin/sh", "")) << "a second alice";
    auto second = etc::add_user(tree, "bob", "/bin/sh", "");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->uid, 1000u);
    // Who luban enter logs in as.
    EXPECT_EQ(etc::login_user(tree / "etc"), "root");
    ASSERT_TRUE(etc::set_login_user(tree / "etc", "alice"));
    EXPECT_EQ(etc::login_user(tree / "etc"), "alice");
    EXPECT_FALSE(etc::set_login_user(tree / "etc", "nobody-here"));
    // A desktop's system is never root's: what it is built on decides.
    luban::service::NewPreview desktop{.lineage = {.layers = {{"subos:luban-desktop@2026.10.13.1", "xim", {}}}}};
    EXPECT_TRUE(luban::service::needs_user(desktop));
    luban::service::NewPreview on_desktop{.lineage = {.layers = {{"awesome:myos@1", "awesome", {}}, {"luban:desktop@2026.10.19.1", "xim", {}}}}};
    EXPECT_TRUE(luban::service::needs_user(on_desktop));
    luban::service::NewPreview core{.lineage = {.layers = {{"luban:core@2026.10.19.1", "xim", {}}}}};
    EXPECT_FALSE(luban::service::needs_user(core));
}

XTEST(LubanIdentity, ASystemIsNamedByItsNameAndMarkedInThePrompt, .area = "luban", .covers = {"ID-HOSTNAME", "ID-PROMPT", "LUBAN-PROMPT-MARK"}) {
    auto home = tk::Home::isolated("luban-name");
    const auto e = home.dir() / "etc";
    EXPECT_EQ(etc::hostname(e, "box"), "box") << "no /etc/hostname";
    tk::write_file(e / "hostname", "luban\n");
    EXPECT_EQ(etc::hostname(e, "box"), "box") << "the edition's placeholder is no name";
    tk::write_file(e / "hostname", "workbench\n");
    EXPECT_EQ(etc::hostname(e, "box"), "workbench") << "the system's own choice";

    EXPECT_EQ(etc::prompt("sandbox", "all", "dumb", false), "[luban] \\u@\\h:\\w\\$ ");
    EXPECT_EQ(etc::prompt("machine", "none", "", false), "<luban:temp> \\u@\\h:\\w\\$ ");
    EXPECT_EQ(etc::prompt("sandbox", "home", "xterm-256color", true), "[luban:home] \\u@\\h:\\w\\$ ") << "NO_COLOR";
    const auto painted = etc::prompt("machine", "all", "xterm-256color", false);
    EXPECT_NE(painted.find("\\[\\033[35m\\]<luban>\\[\\033[0m\\]"), std::string::npos) << painted;

    auto first = etc::ensure_prompt_script(e);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, (std::vector<std::string>{"profile.d/luban.sh", "fish/conf.d/luban.fish"}));
    const auto fish = tk::read_file(e / "fish/conf.d/luban.fish");
    EXPECT_NE(fish.find("function fish_prompt"), std::string::npos) << "fish reads no PS1 (F26)";
    EXPECT_NE(fish.find("<luban$keep>"), std::string::npos);
    tk::write_file(e / "profile.d/luban.sh", "# mine\n");
    auto again = etc::ensure_prompt_script(e);
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->empty());
    EXPECT_EQ(tk::read_file(e / "profile.d/luban.sh"), "# mine\n") << "machine state: an edit stays";
}

XTEST(LubanIdentity, TheWelcomeFitsSeventyTwoColumnsAndReadsWithoutColour, .area = "luban", .covers = {"ID-WELCOME"}) {
    luban::service::Environment e;
    e.name = "a-system-with-a-rather-long-name";
    e.kind = "root";
    e.edition = "subos:luban-core@2026.10.11.1";
    e.packages = 412;
    e.generation = 3;
    e.settings.persist = "all";
    luban::service::WelcomeFacts facts;
    facts.kernel = "Linux 6.8.0-a-very-long-kernel-release-string-from-a-distribution-that-likes-them";
    for (const bool utf8 : {true, false}) {
        const auto lines = luban::service::welcome(e, "sandbox", false, {.zh = false, .color = false, .utf8 = utf8}, facts);
        ASSERT_GE(lines.size(), 8u);
        EXPECT_NE(lines[0].find("[luban] " + e.name), std::string::npos) << lines[0];
        for (const auto& line : lines) {
            EXPECT_EQ(line.find('\x1b'), std::string::npos) << "colour without asking: " << line;
            if (!utf8) EXPECT_TRUE(std::ranges::all_of(line, [](char c) { return static_cast<unsigned char>(c) < 0x80; })) << line;
            std::size_t columns = 0;
            for (std::size_t i = 0; i < line.size(); ++i)
                if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80) ++columns;
            EXPECT_LE(columns, 72u) << line;
        }
    }
    const auto machine = luban::service::welcome(e, "machine", true, {.zh = false, .color = false, .utf8 = true}, facts);
    EXPECT_NE(machine[0].find("<luban:temp>"), std::string::npos) << machine[0];
    EXPECT_NE(machine[machine.size() - 2].find("Ctrl-] p"), std::string::npos);
    const auto zh = luban::service::welcome(e, "sandbox", false, {.zh = true, .color = false, .utf8 = true}, facts);
    EXPECT_NE(zh[2].find("系统"), std::string::npos) << zh[2];
}

XTEST(LubanIdentity, AUsersOwnPackagesComeBeforeTheSystems, .area = "luban", .covers = {"LUBAN-USER-LAYER"}) {
    auto home = tk::Home::isolated("luban-user-layer");
    const auto e = home.dir() / "etc";
    auto first = etc::ensure_user_layer_scripts(e);
    ASSERT_TRUE(first) << first.error();
    EXPECT_EQ(*first, (std::vector<std::string>{"profile.d/xlings-user.sh", "fish/conf.d/xlings-user.fish"}));
    EXPECT_NE(tk::read_file(e / "profile.d/xlings-user.sh").find("$HOME/.xlings/subos/current/bin"), std::string::npos);
    EXPECT_NE(tk::read_file(e / "fish/conf.d/xlings-user.fish").find("$HOME/.xlings/subos/current/bin"), std::string::npos);
    tk::write_file(e / "profile.d/xlings-user.sh", "# mine\n");
    auto again = etc::ensure_user_layer_scripts(e);
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->empty());
    EXPECT_EQ(tk::read_file(e / "profile.d/xlings-user.sh"), "# mine\n") << "machine state: an edit stays";
}
