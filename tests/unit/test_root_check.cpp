// A Luban system's self-check (design 2026-10-16 L §11): what it asks of a
// root, which failures make it not usable, and the command that runs it.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.subos.root_check;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace rc = xlings::subos::root_check;

namespace {
void exe(const fs::path& p) {
    tk::write_file(p, "#!/bin/sh\n");
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add);
}
const rc::Item* item(const rc::Report& r, std::string_view id) {
    const auto it = std::ranges::find(r.items, std::string(id), &rc::Item::id);
    return it == r.items.end() ? nullptr : &*it;
}
// A root as a distribution lays one out, its /usr a directory.
fs::path a_root(const fs::path& dir) {
    exe(dir / "usr/bin/sh");
    exe(dir / "usr/bin/fish");
    fs::create_directory_symlink("usr/bin", dir / "bin");
    tk::write_file(dir / "etc/passwd", "root:x:0:0:root:/root:/usr/bin/fish\n");
    tk::write_file(dir / "etc/hostname", "box\n");
    tk::write_file(dir / "usr/share/terminfo/x/xterm-256color", "entry");
    return dir;
}
}  // namespace

XTEST(RootCheck, AUsableRootPassesTheFastCheck, .area = "luban", .covers = {"LUBAN-SELF-CHECK"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "Luban roots are Linux";
    auto home = tk::Home::isolated("root-check");
    const auto root = a_root(home.root() / "tree");
    const auto r = rc::check(root, rc::Depth::Fast, {.term = "xterm-256color", .run = false});
    EXPECT_TRUE(r.ok());
    for (const auto* id : {"usr", "passwd", "shell", "hostname", "terminfo"}) {
        ASSERT_NE(item(r, id), nullptr) << id;
        EXPECT_TRUE(item(r, id)->ok) << id << ": " << item(r, id)->detail;
    }
    EXPECT_EQ(item(r, "loader"), nullptr) << "the closure is a full check's";
    const auto kitty = rc::check(root, rc::Depth::Fast, {.term = "xterm-kitty", .run = false});
    EXPECT_TRUE(kitty.ok()) << "a terminal it does not describe is not a reason to refuse it";
    EXPECT_FALSE(item(kitty, "terminfo")->ok);
}

XTEST(RootCheck, NoUsrOrNoRootUserIsNotUsable, .area = "luban", .covers = {"LUBAN-SELF-CHECK"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "Luban roots are Linux";
    auto home = tk::Home::isolated("root-check-bad");
    const auto root = a_root(home.root() / "tree");
    tk::write_file(root / "etc/passwd", "nobody:x:65534:65534::/:/bin/false\n");
    auto r = rc::check(root, rc::Depth::Fast, {.run = false});
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(item(r, "passwd")->critical);
    EXPECT_FALSE(item(r, "passwd")->ok);
    fs::remove_all(root / "usr/bin");
    r = rc::check(root, rc::Depth::Fast, {.run = false});
    EXPECT_FALSE(item(r, "usr")->ok);
    const auto json = rc::to_json(r);
    EXPECT_FALSE(json["ok"].get<bool>());
    EXPECT_GE(json["failed"].get<int>(), 2);
}

XTEST(RootCheck, ADesktopListsApplicationsAndHasCursors, .area = "luban", .covers = {"LUBAN-SELF-CHECK"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "Luban roots are Linux";
    auto home = tk::Home::isolated("root-check-desktop");
    const auto root = a_root(home.root() / "tree");
    tk::write_file(root / "usr/share/wayland-sessions/labwc.desktop", "[Desktop Entry]");
    auto r = rc::check(root, rc::Depth::Full, {.run = false});
    EXPECT_TRUE(r.ok()) << "what a desktop lacks is said, not a refusal";
    EXPECT_FALSE(item(r, "applications")->ok);
    EXPECT_FALSE(item(r, "cursors")->ok);
    tk::write_file(root / "usr/share/applications/foot.desktop", "[Desktop Entry]");
    tk::write_file(root / "usr/share/icons/Adwaita/cursors/left_ptr", "xcursor");
    r = rc::check(root, rc::Depth::Full, {.run = false});
    EXPECT_TRUE(item(r, "applications")->ok);
    EXPECT_TRUE(item(r, "cursors")->ok);
}

// A package's checks (design 2026-10-21 §6.1): `launcher` -- its desktop
// entry where launchers look, and shown; `gui` and `sound` are a desktop
// session's -- from outside a system nothing is run, and only what a file
// says (a launcher's entry) is checked.
XTEST(RootCheck, APackagesLauncherCheckLooksWhereLaunchersLook, .area = "luban", .covers = {"LUBAN-PACKAGE-CHECKS"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "Luban roots are Linux";
    auto home = tk::Home::isolated("root-check-package");
    const auto root = a_root(home.root() / "tree");
    tk::write_file(root / rc::kDeclaredDir / "falkon.json", R"({"checks": [
        {"id": "falkon-listed", "what": "the launcher lists it", "launcher": "org.kde.falkon"},
        {"id": "falkon-window", "what": "it opens a page", "gui": "falkon file:///x.html", "timeout": 30},
        {"id": "falkon-sound", "what": "it is heard", "sound": "falkon --play x.ogg"}]})");
    auto r = rc::check(root, rc::Depth::Full, {.run = false});
    ASSERT_NE(item(r, "falkon-listed"), nullptr);
    EXPECT_FALSE(item(r, "falkon-listed")->ok);
    EXPECT_NE(item(r, "falkon-listed")->detail.find("org.kde.falkon.desktop"), std::string::npos) << item(r, "falkon-listed")->detail;
    // From outside the system nothing is run: the session's checks are not listed here.
    for (const auto* id : {"falkon-window", "falkon-sound"}) EXPECT_EQ(item(r, id), nullptr) << id;
    tk::write_file(root / "usr/share/applications/org.kde.falkon.desktop", "[Desktop Entry]\nName=Falkon\n");
    r = rc::check(root, rc::Depth::Full, {.run = false});
    EXPECT_TRUE(item(r, "falkon-listed")->ok) << item(r, "falkon-listed")->detail;
    tk::write_file(root / "usr/share/applications/org.kde.falkon.desktop", "[Desktop Entry]\nNoDisplay=true\n");
    r = rc::check(root, rc::Depth::Full, {.run = false});
    EXPECT_FALSE(item(r, "falkon-listed")->ok) << "hidden: not listed";
}

XTEST(RootCheck, TheCommandAnswersOnlyInsideASystem, .area = "luban", .covers = {"LUBAN-SELF-CHECK"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Luban systems are Linux";
    if (fs::exists("/etc/xlings/usr")) GTEST_SKIP() << "this host is a Luban system";
    auto home = tk::Home::isolated("root-check-cmd");
    const auto outside = home.xlings({"subos", "__check", "--json"});
    EXPECT_EQ(outside.exit_code, 2) << outside.transcript();
    EXPECT_NE(outside.transcript().find("not a Luban system"), std::string::npos) << outside.transcript();
    const auto inside = home.xlings({"subos", "__check", "--json", "--fast"}, {{"XLINGS_ROOT_INSTANCE", "box"}});
    const auto lines = inside.json_lines();
    ASSERT_FALSE(lines.empty()) << inside.transcript();
    const auto& json = lines.back();
    ASSERT_TRUE(json.contains("items")) << inside.transcript();
    EXPECT_EQ(inside.exit_code, json["ok"].get<bool>() ? 0 : 1);
}

XTEST(RootCheck, AProgramThatAnsweredStarted, .area = "luban", .covers = {"LUBAN-SELF-CHECK"}) {
    EXPECT_TRUE(rc::started(0, false, "bash 5.2"));
    EXPECT_TRUE(rc::started(1, false, "unknown option --version")) << "no --version is not a failure to start";
    EXPECT_TRUE(rc::started(127, false, "lsattr: applet not found")) << "BusyBox ran: an applet it was not built with";
    EXPECT_FALSE(rc::started(127, false, "sh: foo: not found"));
    EXPECT_FALSE(rc::started(1, false, "foo: error while loading shared libraries: libx.so.1"));
    EXPECT_FALSE(rc::started(139, false, ""));
    EXPECT_TRUE(rc::started(255, false, "error: Running xmake as root is extremely dangerous"))
        << "a program's own -1 is not a signal: it ran";
    EXPECT_TRUE(rc::started(-1, true, "")) << "still running when the limit came: it started";
}

XTEST(RootCheck, AProgramIsCheckedByItsPackagesChecksOrWithoutADisplay, .area = "luban",
      .covers = {"LUBAN-SELF-CHECK", "LUBAN-PACKAGE-CHECKS"}) {
    // A desktop's check said "falkon starts -- exit 134" though Falkon runs
    // (part 2 J42): it declares checks of its own, and Qt without a display aborts.
    if constexpr (!tk::is_posix) GTEST_SKIP() << "Luban roots are Linux";
    auto home = tk::Home::isolated("root-check-starts");
    const auto root = a_root(home.root() / "tree");
    const auto program = [&](std::string_view package, std::string_view body) {
        const auto payload = home.root() / "xpkgs" / std::format("luban-x-{}", package) / "1.0" / "bin" / package;
        tk::write_file(payload, std::format("#!/bin/sh\n{}\n", body));
        fs::permissions(payload, fs::perms::owner_all, fs::perm_options::add);
        fs::create_symlink(payload, root / "usr/bin" / package);
    };
    // Aborts as Qt does without a platform it can open.
    const std::string qt = R"([ "$QT_QPA_PLATFORM" = offscreen ] && { echo "1.0"; exit 0; }
echo "error while loading shared libraries: could not load the Qt platform plugin" >&2; exit 134)";
    program("falkon", qt);
    program("viewer", qt);
    tk::write_file(root / rc::kDeclaredDir / "falkon.json",
                   R"({"checks": [{"id": "falkon-listed", "launcher": "org.kde.falkon"}]})");
    const rc::Report r{.items = rc::programs_start(root, {.run = true})};
    EXPECT_EQ(item(r, "starts:falkon"), nullptr) << "checked by its own checks";
    ASSERT_NE(item(r, "starts:viewer"), nullptr);
    EXPECT_TRUE(item(r, "starts:viewer")->ok) << item(r, "starts:viewer")->detail;
}
