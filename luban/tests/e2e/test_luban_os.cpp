// The Luban OS app (Luban OS architecture design §14), headless (SDL's
// offscreen driver): what a person sees first, with no system and with one;
// the management panel; and that it draws something.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;
import opensal;
import openhal.audio;

namespace tk = xlings::testkit;
namespace platform = xlings::platform;
namespace fs = std::filesystem;
using Json = nlohmann::json;

#include "luban_os_support.hpp"

using namespace luban_os_e2e;

XTEST(LubanOs, NoSystemYetIsQuickStartOneIsTheLauncherWithItsCountdown, .area = "luban", .covers = {"LUBAN-OS-APP"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test (mcpp build -p luban-os, or LUBAN_OS_BIN)";
    auto home = tk::Home::isolated("luban-os-first");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    auto first = app(home, {});
    ASSERT_EQ(first.exit_code, 0) << first.transcript();
    EXPECT_EQ(state(first).value("view", ""), "quick-start") << first.transcript();
    // One system: the launcher, counting down to it.
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto second = app(home, {});
    ASSERT_EQ(second.exit_code, 0) << second.transcript();
    const auto s = state(second);
    EXPECT_EQ(s.value("view", ""), "launcher") << second.transcript();
    EXPECT_EQ(s["systems"], Json::array({"box"})) << second.transcript();
    EXPECT_EQ(s.value("countdown_to", ""), "box") << second.transcript();
    // A countdown of 0 enters nothing by itself.
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0})");
    EXPECT_FALSE(state(app(home, {})).contains("countdown_to"));
}

XTEST(LubanOs, TheManagementPanelOpensAndTheWindowIsDrawn, .area = "luban", .covers = {"LUBAN-OS-APP"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-admin");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto shot = home.root() / "admin.ppm";
    auto r = app(home, {"--view", "admin", "--screenshot", shot.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(state(r).value("view", ""), "admin");
    const auto bytes = tk::read_file(shot);
    ASSERT_TRUE(bytes.starts_with("P6")) << "a screenshot";
    // More than one colour: the panel drew its controls.
    std::set<std::string> colours;
    const auto at = bytes.find("255\n") + 4;
    for (std::size_t k = at; k + 3 <= bytes.size() && colours.size() < 4; k += 3 * 101) colours.insert(bytes.substr(k, 3));
    EXPECT_GE(colours.size(), 3u);
}

// ── interaction, layout and what is drawn ───────────────────────────────
//
// A script (luban-os --script) drives the app with input as SDL delivers a
// person's -- keys, text, the pointer, the window's close and size -- and
// checks its state between frames; the screenshots are read here (pixels)
// and kept in the artifacts directory for people to look at.


XTEST(LubanOs, KeysAndThePointerReachEveryViewAndTheHighlightFollows, .area = "luban",
      .covers = {"LUBAN-OS-APP", "LUBAN-OS-INTERACTION"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-keys");
    const auto shot = home.root() / "down.ppm", st = home.root() / "down.json";
    auto r = run_script(home, std::format(R"(wait launcher
expect countdown_to alpha
key Down
expect selected 1
expect countdown_to absent
frames 2
screenshot {}
screenshot {}
state {}
key Down
expect selected 2
key Down
expect selected 0
key Up
expect selected 2
key Return
wait quick-start
waitfor host.known true 60
waitfor host.editions_known true 60
click card:nano
expect edition nano
expect name nano-1
click card:tiny
expect edition tiny
expect name tiny-1
click more
waitfor anchors.persist:0 present 5
frames 2
screenshot {}
click back
wait launcher
click gear
wait admin
click tab:activity
expect admin_tab activity
frames 2
screenshot {}
click tab:settings
expect admin_tab settings
frames 2
screenshot {}
click tab:systems
click admin:beta
expect admin_selected beta
click admin:back
wait launcher
key F2
wait admin
)", shot.string(), artifact("launcher-down.bmp").string(), st.string(), artifact("quick-start-more.bmp").string(),
                                          artifact("admin-activity.bmp").string(), artifact("admin-settings.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // The highlight is on the row the keys chose, not on the other.
    const auto img = read_ppm(shot);
    const auto s = read_state(st);
    const auto chosen = rect(s, "row:beta"), other = rect(s, "row:alpha");
    const auto in_chosen = img.at(static_cast<int>(chosen[2]) - 60, static_cast<int>((chosen[1] + chosen[3]) / 2));
    const auto in_other = img.at(static_cast<int>(other[2]) - 60, static_cast<int>((other[1] + other[3]) / 2));
    EXPECT_NE(in_chosen, in_other) << "the chosen row looks like the others";
    EXPECT_GT(in_chosen[2], in_other[2]) << "the highlight is the accent (blue)";
}

XTEST(LubanOs, ADesktopIsDrawnAndGivenTheWindowsInputAndSize, .area = "luban",
      .covers = {"LUBAN-OS-APP", "LUBAN-OS-INTERACTION", "LUBAN-DESKTOP-WIRE"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-desktop");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // A desktop of the test's own: one colour, and everything it is sent, logged.
    const auto fake = home.root() / "desktop.py", log = home.root() / "desktop.log";
    tk::write_file(fake, R"(import struct, sys
log = open(sys.argv[1], "w", buffering=1)
W, H = 320, 200
out = sys.stdout.buffer
def send(kind, payload):
    out.write(kind.encode() + struct.pack(">I", len(payload)) + payload)
    out.flush()
send("h", b"15")
send("s", b"%d %d" % (W, H))
send("f", b"0 0 %d %d %d\n" % (W, H, W * 4) + bytes([0x40, 0x80, 0xC0, 0xFF]) * (W * H))
inp = sys.stdin.buffer
while True:
    head = inp.read(5)
    if len(head) < 5:
        break
    body = inp.read(struct.unpack(">I", head[1:])[0])
    log.write("%s %s\n" % (chr(head[0]), body.decode(errors="replace")))
log.write("eof\n")
)");
    const auto shot = home.root() / "desktop.ppm", st = home.root() / "desktop.json";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 320
frames 2
screenshot {}
screenshot {}
state {}
click desktop
type ab
resize 900 600
sleep 800
key Ctrl+Alt
waitfor menu true
screenshot {}
click menu:back-to-the-list
wait launcher
sleep 300
)", shot.string(), artifact("desktop.bmp").string(), st.string(), artifact("desktop-menu.bmp").string()),
                        {"--desktop-exec", "python3", fake.string(), log.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Its picture, where the window put it (1:1, centred).
    const auto img = read_ppm(shot);
    const auto d = rect(read_state(st), "desktop");
    EXPECT_FLOAT_EQ(d[2] - d[0], 320.0f);
    EXPECT_EQ(img.at(static_cast<int>(d[0]) + 20, static_cast<int>(d[1]) + 20), (std::array<int, 3>{0xC0, 0x80, 0x40}));
    const auto sent = tk::read_file(log);
    using I = opensal::Interface;
    const auto provided = I::Surface | I::Pointer | I::Keyboard | I::Mode | I::Text | I::Clipboard | I::Shell | I::Cursor | I::CursorImage
                          | I::PointerLock | I::Displays;
    EXPECT_NE(sent.find(std::format("H {} 1100 700 1 1", provided)), std::string::npos)
        << "the window's interfaces (surface, pointer, keyboard, mode, text, clipboard, shell, the pointer's shapes, images and lock, "
           "one of several displays), size, density and pacing, first\n" << sent;
    // Pacing: once it has shown the frame, it says so (the next may come).
    EXPECT_NE(sent.find("E presented"), std::string::npos) << sent;
    // The click, in the desktop's own pixels (its middle), down and up.
    EXPECT_NE(sent.find("E motion 160 100"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E button 1 1 0"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E button 1 0 0"), std::string::npos) << sent;
    // Keys as keysyms ('a' is 97), down and up.
    EXPECT_NE(sent.find("E key 1 97 0 0"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E key 0 98 0 0"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E resize 900 600"), std::string::npos) << "the window's new size\n" << sent;
    // Leaving it ends its stream.
    EXPECT_NE(sent.find("eof"), std::string::npos) << sent;
}

// A desktop without a shell of its own asks the window to be it (design
// 2026-10-18 §7, T1): its applications and windows on the window's bar, an
// application started and a window brought forward by a click.
XTEST(LubanOs, ADesktopHoldsTheKeyboardAndCtrlAltKLetsItGo, .area = "luban",
      .covers = {"LUBAN-OS-KEYBOARD", "LUBAN-OS-WINDOW-TITLE", "LUBAN-OS-POINTER"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-keyboard");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // A desktop that asks to hold the pointer (a game would), and logs what it gets.
    const auto fake = home.root() / "desktop.py", log = home.root() / "desktop.log";
    using I = opensal::Interface;
    tk::write_file(fake, std::format(R"(import struct, sys
log = open(sys.argv[1], "w", buffering=1)
W, H = 320, 200
out = sys.stdout.buffer
def send(kind, payload):
    out.write(kind.encode() + struct.pack(">I", len(payload)) + payload)
    out.flush()
send("h", b"{}")
send("s", b"%d %d" % (W, H))
send("f", b"0 0 %d %d %d\n" % (W, H, W * 4) + bytes([0x40, 0x80, 0xC0, 0xFF]) * (W * H))
send("l", b"1")
inp = sys.stdin.buffer
while True:
    head = inp.read(5)
    if len(head) < 5:
        break
    body = inp.read(struct.unpack(">I", head[1:])[0])
    log.write("%s %s\n" % (chr(head[0]), body.decode(errors="replace")))
)", I::Surface | I::Mode | I::Pointer | I::Keyboard | I::PointerLock));
    const auto held = home.root() / "held.json", gone = home.root() / "gone.json", again = home.root() / "again.json";
    const auto refused = home.root() / "refused.json", retried = home.root() / "retried.json";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 320
frames 3
state {}
key Ctrl+Alt+K
frames 3
state {}
key Ctrl+Alt+K
frames 3
state {}
key Ctrl+Alt+K
frames 3
refuse-grabs 1000
key Ctrl+Alt+K
frames 3
state {}
refuse-grabs 0
sleep 700
frames 3
state {}
)", held.string(), gone.string(), again.string(), refused.string(), retried.string()),
                        {"--desktop-exec", "python3", fake.string(), log.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = read_state(held)["window"], b = read_state(gone)["window"], c = read_state(again)["window"];
    EXPECT_EQ(a["title"], "Luban OS (desktop) · keyboard held (Ctrl+Alt+K lets it go)") << "the system it shows, in the title";
    EXPECT_TRUE(a["keyboard_grabbed"].get<bool>()) << "a desktop entered holds the keyboard";
    EXPECT_FALSE(b["keyboard_grabbed"].get<bool>()) << "Ctrl+Alt+K lets it go";
    EXPECT_EQ(b["title"], "Luban OS (desktop)") << "let go, the title says nothing of it";
    EXPECT_EQ(b["toast"], "Keyboard: let go") << "and the change is said at the top";
    EXPECT_FALSE(b["pointer_locked"].get<bool>()) << "and the pointer the desktop held";
    EXPECT_TRUE(c["keyboard_grabbed"].get<bool>()) << "and holds it again";
    EXPECT_FALSE(c["pointer_locked"].get<bool>()) << "the pointer only when the desktop asks again";
    // The host refusing (design 2026-10-20 §5, G8): not held, said, asked again.
    const auto e = read_state(refused)["window"], f = read_state(retried)["window"];
    EXPECT_TRUE(e["keyboard_wanted"].get<bool>()) << e.dump();
    EXPECT_FALSE(e["keyboard_grabbed"].get<bool>()) << "a refusal is not held\n" << e.dump();
    EXPECT_NE(e["toast"].get<std::string>().find("did not let it be held"), std::string::npos) << e.dump();
    EXPECT_EQ(e["title"], "Luban OS (desktop)") << e.dump();
    EXPECT_TRUE(f["keyboard_grabbed"].get<bool>()) << "held on the next try\n" << f.dump();
    // The chord is the window's: the desktop never sees its K.
    const auto sent = tk::read_file(log);
    EXPECT_EQ(sent.find(std::format("E key 1 {} ", static_cast<int>('k'))), std::string::npos) << sent;

    // Set not to hold it: a desktop entered leaves the keyboard alone. And an
    // xdesk of before that does not ask for the pointer (it cannot draw one)
    // is given the host's arrow over its picture (luban.compat).
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"lock_keyboard": false})");
    const auto unheld = home.root() / "unheld.json";
    r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 320
move 550 350
frames 3
state {}
)", unheld.string()),
                   {"--desktop-session", "xdesk-classic", "--desktop-exec", "python3", fake.string(), log.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto d = read_state(unheld);
    EXPECT_FALSE(d["window"]["keyboard_grabbed"].get<bool>()) << "the setting is honoured";
    EXPECT_TRUE(d["pointer"]["host_arrow"].get<bool>()) << d["pointer"].dump();
    EXPECT_EQ(d["pointer"]["shape"], "default") << d["pointer"].dump();
}

XTEST(LubanOs, TheWindowIsTheShellOfADesktopWithoutOne, .area = "luban",
      .covers = {"LUBAN-DESKTOP-SHELL"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-shell");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto fake = home.root() / "desktop.py", log = home.root() / "desktop.log";
    tk::write_file(fake, R"(import struct, sys
log = open(sys.argv[1], "w", buffering=1)
W, H = 320, 200
out = sys.stdout.buffer
def send(kind, payload):
    out.write(kind.encode() + struct.pack(">I", len(payload)) + payload)
    out.flush()
send("h", b"%d" % (15 | 512))       # surface pointer keyboard mode, and shell
inp = sys.stdin.buffer
told = False
while True:
    head = inp.read(5)
    if len(head) < 5:
        break
    body = inp.read(struct.unpack(">I", head[1:])[0])
    log.write("%s %s\n" % (chr(head[0]), body.decode(errors="replace")))
    if chr(head[0]) == "H" and not told:
        told = True
        send("s", b"%d %d" % (W, H))
        send("f", b"0 0 %d %d %d\n" % (W, H, W * 4) + bytes([0x40, 0x80, 0xC0, 0xFF]) * (W * H))
        send("a", "foot\tFoot\norg.example.Editor\tEditor\n".encode())
        send("w", "7\t1\t0\tfoot\t~ shell\n9\t0\t1\teditor\tnotes.txt\n".encode())
log.write("eof\n")
)");
    const auto st = home.root() / "shell.json";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.shell.windows.1.title notes.txt 30
frames 2
click shell:apps
frames 2
expect desktop.shell.open true
screenshot {}
click app:org.example.Editor
frames 2
expect desktop.shell.open false
click window:9
frames 2
state {}
key Ctrl+Alt
waitfor menu true
click menu:back-to-the-list
wait launcher
sleep 300
)", artifact("desktop-shell.bmp").string(), st.string()),
                        {"--desktop-exec", "python3", fake.string(), log.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto shell = read_state(st)["desktop"]["shell"];
    EXPECT_EQ(shell["apps"], Json::array({"foot", "org.example.Editor"}));
    EXPECT_EQ(shell["windows"][0]["focused"], true);
    const auto sent = tk::read_file(log);
    EXPECT_NE(sent.find("E launch org.example.Editor"), std::string::npos) << sent;
    EXPECT_NE(sent.find("E activate 9"), std::string::npos) << sent;
    // The bar is the window's: the click on it is not the desktop's.
    EXPECT_EQ(sent.find("E button"), std::string::npos) << sent;
}

// A pointer moving over a desktop is sent on, and the window is not drawn
// again until the desktop sends a new picture (design 2026-10-18 §13.2, E13:
// every move redrew the whole window, half a core without a GPU).
XTEST(LubanOs, APointerMovingOverAStillDesktopDrawsNothing, .area = "luban",
      .covers = {"LUBAN-OS-PERF"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-still-desktop");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // One picture, then nothing: what it is sent, counted.
    const auto fake = home.root() / "desktop.py", log = home.root() / "desktop.log";
    tk::write_file(fake, R"(import struct, sys
log = open(sys.argv[1], "w", buffering=1)
W, H = 320, 200
out = sys.stdout.buffer
def send(kind, payload):
    out.write(kind.encode() + struct.pack(">I", len(payload)) + payload)
    out.flush()
send("h", b"15")
send("s", b"%d %d" % (W, H))
send("f", b"0 0 %d %d %d\n" % (W, H, W * 4) + bytes([0x40, 0x80, 0xC0, 0xFF]) * (W * H))
inp = sys.stdin.buffer
while True:
    head = inp.read(5)
    if len(head) < 5:
        break
    body = inp.read(struct.unpack(">I", head[1:])[0])
    log.write("%s %s\n" % (chr(head[0]), body.decode(errors="replace")))
)");
    std::string moves;
    for (int i = 0; i < 60; ++i) moves += std::format("move {} {}\nframes 1\n", 400 + i * 3, 300 + i);
    const auto st = home.root() / "still.json";
    auto r = run_script(home, std::format("wait desktop\nwaitfor desktop.texture.0 320\nframes 3\nperf reset\n{}state {}\n",
                                          moves, st.string()),
                        {"--desktop-exec", "python3", fake.string(), log.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto p = read_state(st)["perf"];
    EXPECT_GE(p["frames"].get<int>(), 60) << p.dump();
    EXPECT_LE(p["presented"].get<int>(), 3) << "sixty moves over a still picture\n" << p.dump();
    EXPECT_EQ(p["renderer"], "software") << "asked for (SDL_RENDER_DRIVER)";
    const auto sent = tk::read_file(log);
    EXPECT_NE(sent.find("E motion"), std::string::npos) << "the moves reach the desktop\n" << sent;
}

// Without a GPU a desktop that draws as fast as it may is shown at most 30
// times a second: its frames are paced by when it is told the last was shown.
XTEST(LubanOs, WithoutAGpuADesktopIsShownThirtyTimesASecond, .area = "luban",
      .covers = {"LUBAN-OS-PERF"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-thirty");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // A new picture each time it is told the last one was shown.
    const auto fake = home.root() / "desktop.py";
    tk::write_file(fake, R"(import struct, sys
W, H = 64, 64
out = sys.stdout.buffer
n = 0
def send(kind, payload):
    out.write(kind.encode() + struct.pack(">I", len(payload)) + payload)
    out.flush()
def frame():
    global n
    n += 1
    send("f", b"0 0 %d %d %d\n" % (W, H, W * 4) + bytes([n % 256, 0x80, 0xC0, 0xFF]) * (W * H))
send("h", b"15")
send("s", b"%d %d" % (W, H))
frame()
inp = sys.stdin.buffer
while True:
    head = inp.read(5)
    if len(head) < 5:
        break
    body = inp.read(struct.unpack(">I", head[1:])[0])
    if body.startswith(b"presented"):
        frame()
)");
    const auto st = home.root() / "thirty.json";
    auto r = run_script(home, std::format("wait desktop\nwaitfor desktop.texture.0 64\nperf reset\nsleep 2000\nstate {}\n", st.string()),
                        {"--desktop-exec", "python3", fake.string()});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto p = read_state(st)["perf"];
    EXPECT_GE(p["presented"].get<int>(), 20) << "it keeps showing new pictures\n" << p.dump();
    EXPECT_LE(p["presented"].get<int>(), 70) << "two seconds, at most 30 a second\n" << p.dump();
}

// SDL's software renderer when nobody asked for it is said, with the remedy
// (E13): on a Linux host it is a luban-os that does not reach the graphics
// stack.
XTEST(LubanOs, DrawingWithoutTheGpuIsSaid, .area = "luban", .covers = {"LUBAN-OS-PERF"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-renderer");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto st = home.root() / "renderer.json";
    auto r = run_script(home, std::format("frames 3\nstate {}\n", st.string()), {}, {{"SDL_RENDER_DRIVER", ""}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto s = read_state(st);
    const auto renderer = s["perf"]["renderer"].get<std::string>();
    EXPECT_FALSE(renderer.empty()) << s["perf"].dump();
    const auto notice = s.value("notice", std::string{});
    if (renderer == "software") EXPECT_NE(notice.find("xlings install graphics"), std::string::npos) << notice;
    else EXPECT_EQ(notice.find("GPU"), std::string::npos) << renderer << ": " << notice;
}

XTEST(LubanOs, EveryControlIsOnScreenApartAndBigEnoughAtEverySizeInBothLanguages, .area = "luban",
      .covers = {"LUBAN-OS-APP", "LUBAN-OS-LAYOUT"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-layout");
    for (const auto& [w, h] : {std::pair{820, 520}, std::pair{1100, 700}, std::pair{1600, 1000}}) {
        for (const std::string lang : {"en", "zh"}) {
            const auto tag = std::format("{}x{}-{}", w, h, lang);
            std::vector<std::string> views{"launcher", "quick-start", "admin"};
            auto r = run_script(home, std::format(R"(resize {0} {1}
wait launcher
key Down
frames 3
state {2}/launcher.json
screenshot {3}
click row:new
wait quick-start
waitfor host.known true 60
waitfor host.editions_known true 60
frames 3
state {2}/quick-start.json
screenshot {4}
click back
wait launcher
key F2
wait admin
frames 3
state {2}/admin.json
screenshot {5}
)", w, h, home.root().string(), artifact(tag + "-launcher.bmp").string(), artifact(tag + "-quick-start.bmp").string(),
                                                  artifact(tag + "-admin.bmp").string()),
                                {}, {{"LUBAN_OS_LANG", lang == "zh" ? "zh_CN.UTF-8" : "en_US.UTF-8"}});
            ASSERT_EQ(r.exit_code, 0) << tag << "\n" << r.transcript();
            for (const auto& view : views) {
                const auto s = read_state(home.root() / (view + ".json"));
                std::vector<std::pair<std::string, std::array<float, 4>>> controls;
                for (auto it = s["anchors"].begin(); it != s["anchors"].end(); ++it)
                    controls.emplace_back(it.key(), std::array<float, 4>{it.value()[0].get<float>(), it.value()[1].get<float>(),
                                                                         it.value()[2].get<float>(), it.value()[3].get<float>()});
                ASSERT_FALSE(controls.empty()) << tag << " " << view;
                for (const auto& [name, c] : controls) {
                    EXPECT_GE(c[0], 0) << tag << " " << view << " " << name << " starts off the window";
                    EXPECT_GE(c[1], 0) << tag << " " << view << " " << name;
                    EXPECT_LE(c[2], w) << tag << " " << view << " " << name << " runs off the window";
                    EXPECT_LE(c[3], h) << tag << " " << view << " " << name << " runs off the window";
                    EXPECT_GE(c[3] - c[1], 20) << tag << " " << view << " " << name << " is too small to hit";
                }
                for (std::size_t i = 0; i < controls.size(); ++i)
                    for (std::size_t k = i + 1; k < controls.size(); ++k) {
                        const auto& a = controls[i].second;
                        const auto& b = controls[k].second;
                        const bool apart = a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1];
                        EXPECT_TRUE(apart) << tag << " " << view << ": " << controls[i].first << " and " << controls[k].first
                                           << " overlap";
                    }
            }
        }
    }
}

XTEST(LubanOs, TheCountdownMovesFrameByFrame, .area = "luban", .covers = {"LUBAN-OS-APP", "LUBAN-OS-LAYOUT"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-countdown");
    const auto a = home.root() / "c1.ppm", b = home.root() / "c2.ppm";
    auto r = run_script(home, std::format("wait launcher\nscreenshot {}\nsleep 1200\nscreenshot {}\nexpect countdown_to alpha\n",
                                          a.string(), b.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(tk::read_file(a), tk::read_file(b)) << "the countdown did not change what is drawn";
}

XTEST(LubanOs, AConsoleShowsColourAndWideTextAndTakesWhatIsTyped, .area = "luban",
      .covers = {"LUBAN-OS-INTERACTION", "LUBAN-TERM"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-console");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto shot = home.root() / "console.ppm", st = home.root() / "console.json";
    auto r = run_script(home, std::format(R"(wait console
waitfor console.rows.1 ready 10
type hello
key Return
waitfor console.rows.3 hello 10
frames 2
screenshot {}
screenshot {}
state {}
)", shot.string(), artifact("console.bmp").string(), st.string()),
                        {"--console-exec", "/bin/sh", "-c", R"(printf '\033[31mred\033[0m 中文 wide\nready\n'; exec cat)"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto s = read_state(st);
    const auto rows = s["console"]["rows"];
    EXPECT_EQ(rows[0], "red 中文 wide") << s.dump(2);
    EXPECT_EQ(rows[2], "hello") << "the terminal's echo of what was typed\n" << s.dump(2);
    EXPECT_EQ(rows[3], "hello") << "the program's answer\n" << s.dump(2);
    // The red is drawn red: a reddish pixel in the first line.
    const auto img = read_ppm(shot);
    const auto c = rect(s, "console");
    bool red = false;
    for (int y = static_cast<int>(c[1]); y < static_cast<int>(c[1]) + 18 && !red; ++y)
        for (int x = static_cast<int>(c[0]); x < static_cast<int>(c[0]) + 40 && !red; ++x) {
            const auto p = img.at(x, y);
            red = p[0] > 150 && p[1] < 130 && p[2] < 130;
        }
    EXPECT_TRUE(red) << "no red in the first line";
}

// The console's pointer (design 2026-10-18 §12, E16): a program that takes
// the mouse is sent it; otherwise it selects -- a word, a line -- and the
// middle button pastes what was selected, the right one has Copy and Paste.
XTEST(LubanOs, TheConsolesPointerIsTheProgramsOrSelects, .area = "luban",
      .covers = {"LUBAN-CONSOLE-MOUSE"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-console-mouse");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    // A program that asks for the mouse (buttons, SGR) and says what it gets.
    const auto prog = home.root() / "mouse.py";
    tk::write_file(prog, R"(import os, sys, tty
tty.setraw(0)
os.write(1, b"\033[?1000h\033[?1006hready\r\n")
while True:
    got = os.read(0, 64)
    if not got: break
    os.write(1, b"got " + got.replace(b"\033", b"ESC") + b"\r\n")
)");
    auto taken = run_script(home, R"(wait console
see ready
cell 2 5
see got ESC[<0;6;3M
see got ESC[<0;6;3m
cell 2 5 right
see got ESC[<2;6;3M
)", {"--console-exec", "python3", prog.string()}, {{"LUBAN_OS_LANG", "en_US.UTF-8"}});
    EXPECT_EQ(taken.exit_code, 0) << taken.transcript();

    // Clicks count on one cell (xterm's rule): the line is three clicks on a
    // cell of its own, not a third after the word's two -- five quick clicks
    // on one cell are the line's three and the next word's two, and whether
    // the machine is quick enough to make them one run is not the test's to
    // decide (design 2026-10-19 F12).
    const auto words = home.root() / "words.json", line = home.root() / "line.json", menu = home.root() / "menu.json";
    const auto five = home.root() / "five.json";
    auto selected = run_script(home, std::format(R"(wait console
see ready
cell 0 9 double
frames 2
state {}
cell 0 2 triple
frames 2
state {}
cell 0 23 double
cell 0 23 triple
frames 2
state {}
cell 3 0 middle
see alpha beta-gamma.txt delta
cell 3 2 right
frames 3
state {}
)", words.string(), line.string(), five.string(), menu.string()),
                               {"--console-exec", "/bin/sh", "-c", R"(printf 'alpha beta-gamma.txt delta\nready\n'; exec cat)"},
                               {{"LUBAN_OS_LANG", "en_US.UTF-8"}});
    ASSERT_EQ(selected.exit_code, 0) << selected.transcript();
    EXPECT_EQ(read_state(words)["clipboard"], "beta-gamma.txt") << "two clicks: the word";
    EXPECT_EQ(read_state(line)["clipboard"], "alpha beta-gamma.txt delta") << "three: the line";
    EXPECT_EQ(read_state(five)["clipboard"], "delta") << "five on one cell, quickly: the line's three, then the word's two";
    const auto anchors = read_state(menu)["anchors"];
    EXPECT_TRUE(anchors.contains("console:copy") && anchors.contains("console:paste")) << anchors.dump();
}

// What the app can do, asked before anything relies on it (design
// 2026-10-19 §10.2): answered with no display at all (SSH, CI), at once, in
// JSON; the `--version` after the question is for an app that predates it.
XTEST(LubanOs, ItSaysWhatItCanDoWithoutADisplay, .area = "luban",
      .covers = {"LUBAN-OS-APP-CAPABILITIES"}, .requires_ = {"xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "__capabilities", "--json", "--version"};
    o.env = {{"PATH", "/usr/bin:/bin"}};   // no DISPLAY, no WAYLAND_DISPLAY, no SDL driver
    o.timeout = std::chrono::seconds(10);
    const auto r = tk::run(o);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto j = Json::parse(r.out, nullptr, false);
    ASSERT_TRUE(j.is_object()) << r.out;
    EXPECT_FALSE(j.value("version", "").empty());
    EXPECT_EQ(j["provides"].value("app-capabilities", 0), 1) << j.dump();
    EXPECT_GE(j["provides"].value("root-layout", 0), 2) << j.dump();
    EXPECT_TRUE(j["needs"].is_array());
    EXPECT_LT(r.elapsed, std::chrono::seconds(3)) << "it opened nothing";
}

// A person's first time: nothing yet, the quick start, a system made for
// real (downloaded, unpacked, set up), and then used -- what a new user sees,
// end to end, as they would do it.
XTEST(LubanOs, AFirstTimeUserMakesASystemFromTheQuickStartAndUsesIt, .area = "luban",
      .covers = {"LUBAN-OS-APP", "LUBAN-OS-FIRST-USE", "LUBAN-WELCOME"}, .requires_ = {"linux", "xlings-bin", "static-xlings", "sandbox", "network"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-first-use");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto made = home.root() / "made.ppm", st = home.root() / "first.json";
    auto r = run_script(home, std::format(R"(wait quick-start
waitfor host.known true 60
waitfor host.editions_known true 60
frames 2
screenshot {}
click card:tiny
expect edition tiny
click make
wait job
see 5s tiny-1
screenshot {}
wait console 900
see 120s root@tiny-1
see 5s Luban OS
see 5s shares this host's kernel
see 5s exit to leave
screenshot {}
screenshot {}
type echo luban-$((6*7))
key Return
see 30s luban-42
state {}
)", artifact("first-use-1-quick-start.bmp").string(), artifact("first-use-2-making.bmp").string(),
                                          artifact("first-use-3-console.bmp").string(), made.string(), st.string()),
                        {}, {}, std::chrono::seconds(1500));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto s = read_state(st);
    EXPECT_EQ(s["view"], "console");
    EXPECT_EQ(s["current"], "tiny-1");
    EXPECT_EQ(s["job"]["code"], 0) << s["job"].dump();
    // It is in the list from now on: the next start shows the launcher.
    auto again = run_script(home, "wait launcher\nexpect systems.0 tiny-1\n");
    EXPECT_EQ(again.exit_code, 0) << again.transcript();
}

// A .luban archive, as luban export writes one (POSIX ustar): its manifest
// and the files of its state.
void write_luban_(const fs::path& file, const Json& manifest, const std::vector<std::pair<std::string, std::string>>& state = {}) {
    std::string out;
    auto put = [&](const std::string& name, char type, const std::string& data, unsigned mode) {
        std::array<char, 512> h{};
        auto field = [&](std::size_t at, std::size_t len, const std::string& v) { std::memcpy(h.data() + at, v.data(), std::min(len, v.size())); };
        auto octal = [&](std::size_t at, std::size_t len, std::uint64_t v) { field(at, len, std::format("{:0{}o}", v, len - 1)); };
        field(0, 100, name);
        octal(100, 8, mode);
        octal(108, 8, 0);
        octal(116, 8, 0);
        octal(124, 12, data.size());
        octal(136, 12, 0);
        h[156] = type;
        field(257, 6, std::string("ustar\0", 6));
        field(263, 2, "00");
        std::memset(h.data() + 148, ' ', 8);
        unsigned sum = 0;
        for (char c : h) sum += static_cast<unsigned char>(c);
        field(148, 8, std::format("{:06o}", sum) + std::string("\0 ", 2));
        out.append(h.data(), h.size());
        out += data;
        out.append((512 - data.size() % 512) % 512, '\0');
    };
    put("manifest.json", '0', manifest.dump(), 0644);
    for (const auto& [path, content] : state) put("state/" + path, '0', content, 0644);
    out.append(1024, '\0');
    tk::write_file(file, out);
}

Json shared_box_manifest_() {
    return Json{{"format", "luban-environment"}, {"version", 1}, {"name", "shared-box"},
                {"edition", {{"ref", "subos:luban-tiny"}, {"chain", Json::array()}, {"packages", Json::object()}}},
                {"packages", Json::object()},
                {"policy", {{"extends", "dev"},
                            {"isolation", {{"net", "host"}, {"identity", "host"}, {"grants", Json::array({"clipboard"})},
                                           {"grants_allowed", Json::array()}, {"env_pass", Json::array()},
                                           {"disable_userns", false}, {"no_degrade", false}}},
                            {"mounts", Json::array()}}}};
}

XTEST(LubanOs, TheQuickStartSaysWhatEachSystemIsAndWhatThisComputerCanMake, .area = "luban",
      .covers = {"LUBAN-OS-QUICK-START"}, .requires_ = {"xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-quick");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto nano = home.root() / "nano.json", core = home.root() / "core.json";
    // A computer that can run a system makes them (once the index has
    // answered); one that cannot says why before anything is tried.
    const bool can_run = app_can_run_(home);
    const std::string can = can_run ? "true" : "false";
    auto r = run_script(home, std::format(R"(wait quick-start
expect edition core
expect name core-1
waitfor quick_start.can_make {0} 30
waitfor host.editions_known true 60
frames 2
state {1}
screenshot {3}
click card:nano
expect edition nano
expect name nano-1
frames 2
state {2}
click card:desktop
expect edition desktop
expect name desktop-1
click card:tiny
expect edition tiny
)", can, core.string(), nano.string(), artifact(std::string("quick-start-") + std::string(platform::OS_NAME) + ".bmp").string()),
                        {}, {{"LUBAN_OS_LANG", "en_US.UTF-8"}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto said_core = read_state(core)["quick_start"]["said"].get<std::string>();
    const auto said_nano = read_state(nano)["quick_start"]["said"].get<std::string>();
    if (can_run) {
        // What it is -- or, as the index answered for this home, why it
        // cannot be made: never a card that fails only once clicked.
        auto expect_said = [&](const Json& st, const std::string& edition, const std::string& said, std::string_view detail) {
            const auto answer = st.value("available", Json::object()).value(edition, std::string("checking"));
            if (answer == "unlisted") EXPECT_EQ(said, "Not published yet.") << edition;
            else if (answer == "not-here") EXPECT_NE(said.find("Not built for"), std::string::npos) << said;
            else EXPECT_NE(said.find(detail), std::string::npos) << edition << ": " << said;
        };
        expect_said(read_state(core), "core", said_core, "glibc");
        // nano is said to have nothing to log into (it has no shell).
        expect_said(read_state(nano), "nano", said_nano, "no shell");
    } else {
        EXPECT_NE(said_core.find("cannot run Luban systems yet"), std::string::npos) << said_core;
        EXPECT_NE(said_core.find("do it:"), std::string::npos) << said_core;
    }
    // Both groups are on the screen.
    const auto s = read_state(core);
    for (const auto* card : {"card:nano", "card:tiny", "card:core", "card:desktop", "card:desktop-xdesk", "make", "import"})
        EXPECT_TRUE(s["anchors"].contains(card)) << card;
}

// What the quick start offers besides its cards comes from the indexes
// (design 2026-10-18 §6.4): systems and desktops by category, another
// index's said where it comes from and agreed to before it is made.
XTEST(LubanOs, TheQuickStartOffersWhatTheIndexesHave, .area = "luban",
      .covers = {"LUBAN-OS-QUICK-START"}, .requires_ = {"xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-offers");
    const auto primary = home.root() / "primary", other = home.root() / "other";
    auto recipe = [](std::string_view ns, std::string_view name, std::string_view type, std::string_view categories,
                     std::string_view description) {
        return std::format(R"LUA(
package = {{ spec = '1', name = '{}', namespace = '{}', description = '{}', type = '{}',
            categories = {{ {} }}, archs = {{'x86_64', 'aarch64'}},
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }}, macosx = {{ ['1.0.0'] = {{}} }}, windows = {{ ['1.0.0'] = {{}} }} }} }}
)LUA", name, ns, description, type, categories);
    };
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(primary / "pkgs/luban/core.lua", recipe("luban", "core", "subos", "'distribution'", "the console"));
    tk::write_file(primary / "pkgs/luban/desktop.lua",
                   recipe("luban", "desktop", "subos", "'distribution'", "core and the xdesk classic desktop"));
    tk::write_file(primary / "pkgs/luban/studio.lua", recipe("luban", "studio", "subos", "'distribution'", "a newer edition"));
    tk::write_file(primary / "pkgs/luban/labwc.lua", recipe("luban", "labwc", "package", "'luban-desktop'", "labwc and a panel"));
    tk::write_file(other / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(other / "pkgs/m/myos.lua", recipe("other", "myos", "subos", "'distribution'", "a system of someone else"));
    const Json config{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                     {{"name", "other"}, {"url", other.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto offered = home.root() / "offered.json", desktop = home.root() / "desktop.json", agree = home.root() / "agree.json";
    auto r = run_script(home, std::format(R"(wait quick-start
resize 1100 1100
waitfor quick_start.offers present 60
click card:desktop
frames 2
state {0}
click card:other:myos
expect name myos-1
click more
frames 2
click desktop:luban:labwc
frames 2
state {1}
click make
frames 2
state {2}
)", offered.string(), desktop.string(), agree.string()), {}, {{"LUBAN_OS_LANG", "en_US.UTF-8"}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto st = read_state(offered);
    const auto offers = st["quick_start"]["offers"];
    EXPECT_EQ(offers["systems"], Json::array({"luban:core", "luban:desktop", "luban:studio", "other:myos"})) << offers.dump();
    EXPECT_EQ(offers["desktops"], Json::array({"luban:labwc"}));
    // Cards for what the window has none of: an official one, and another index's.
    EXPECT_TRUE(st["anchors"].contains("card:luban:studio"));
    EXPECT_TRUE(st["anchors"].contains("card:other:myos"));
    EXPECT_FALSE(st["anchors"].contains("card:luban:core")) << "core has its own card";
    const auto chosen = read_state(desktop);
    EXPECT_EQ(chosen["quick_start"]["offer"], "other:myos");
    EXPECT_EQ(chosen["quick_start"]["desktop"], 0);
    if (chosen["quick_start"]["can_make"].get<bool>()) {
        EXPECT_EQ(chosen["quick_start"]["said"], "a system of someone else");
        // Another index's system is agreed to before anything is made.
        EXPECT_EQ(read_state(agree)["confirm"], "Not from Luban");
    }
}

XTEST(LubanOs, TheQuickStartGroupsDesktopsByWhatTheyBring, .area = "luban",
      .covers = {"LUBAN-OS-QUICK-START", "LUBAN-OS-QUICK-START-DESKTOPS"}, .requires_ = {"xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-desktop-groups");
    const auto primary = home.root() / "primary", other = home.root() / "other";
    // A recipe as the index has it: its status, and what extensions.luban says.
    auto recipe = [](std::string_view ns, std::string_view name, std::string_view type, std::string_view categories,
                     std::string_view status, std::string_view luban) {
        return std::format(R"LUA(
package = {{ spec = '1', name = '{}', namespace = '{}', description = 'the {} recipe', type = '{}', status = '{}',
            categories = {{ {} }}, archs = {{'x86_64', 'aarch64'}}, extensions = {{ luban = {{ {} }} }},
            xpm = {{ linux = {{ ['1.0.0'] = {{}} }}, macosx = {{ ['1.0.0'] = {{}} }}, windows = {{ ['1.0.0'] = {{}} }} }} }}
)LUA", name, ns, name, type, status, categories, luban);
    };
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(primary / "pkgs/luban/core.lua", recipe("luban", "core", "subos", "'distribution'", "stable", ""));
    tk::write_file(primary / "pkgs/luban/xdesk.lua", recipe("luban", "xdesk", "package", "'luban-desktop'", "stable", ""));
    tk::write_file(primary / "pkgs/luban/lxqt.lua",
                   recipe("luban", "lxqt", "package", "'luban-desktop'", "stable", "tier = 3, title = { en = 'LXQt', zh = 'LXQt' }"));
    tk::write_file(primary / "pkgs/luban/labwc.lua",
                   recipe("luban", "labwc", "package", "'luban-desktop'", "stable", "tier = 2, summary = { en = 'a light panel', zh = '轻量面板' }"));
    tk::write_file(primary / "pkgs/luban/compositor.lua",
                   recipe("luban", "compositor", "package", "'luban-desktop'", "dev", "tier = 1, gpu = 'better', title = { en = 'Compositor' }"));
    tk::write_file(primary / "pkgs/luban/old.lua", recipe("luban", "old", "package", "'luban-desktop'", "deprecated", "tier = 2"));
    tk::write_file(other / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(other / "pkgs/f/fancy.lua", recipe("other", "fancy", "package", "'luban-desktop'", "stable", ""));
    const Json config{
        {"mirror", "GLOBAL"},
        {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                     {{"name", "other"}, {"url", other.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto groups = home.root() / "groups.json", lxqt = home.root() / "lxqt.json", keys = home.root() / "keys.json";
    const auto small = home.root() / "small.json";
    auto r = run_script(home, std::format(R"(wait quick-start
resize 1300 1000
waitfor quick_start.offers present 60
frames 2
state {0}
screenshot {4}
click card:luban:lxqt
frames 2
state {1}
key Left
frames 2
key Down
frames 2
state {2}
click card:luban:labwc
click more
resize 1000 560
frames 4
state {3}
)", groups.string(), lxqt.string(), keys.string(), small.string(), artifact("quick-start-groups.bmp").string()), {},
                        {{"LUBAN_OS_LANG", "en_US.UTF-8"}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Four groups: consoles, xdesk, the traditional desktops by tier (the
    // unrated last, xdesk not among them, the deprecated not at all), more.
    const auto qs = read_state(groups)["quick_start"];
    std::map<std::string, Json> by;
    for (const auto& g : qs["groups"]) by[g["key"].get<std::string>()] = g["cards"];
    EXPECT_EQ(by["xdesk"], Json::array({"card:desktop", "card:desktop-xdesk"}));
    EXPECT_EQ(by["desktops"], Json::array({"card:luban:compositor", "card:luban:labwc", "card:luban:lxqt", "card:other:fancy"}));
    EXPECT_TRUE(by["console"].size() >= 5u);
    // A traditional desktop chosen: made on core as a machine, named after it.
    const auto t = read_state(lxqt);
    EXPECT_EQ(t["quick_start"]["traditional"], "luban:lxqt");
    EXPECT_EQ(t["quick_start"]["base"], "core");
    EXPECT_EQ(t["mode"], "machine");
    EXPECT_EQ(t["name"], "lxqt-1");
    // Left in its group, Down to the next group that has cards.
    // (no more systems here: from the second desktop, round to the second console)
    EXPECT_EQ(read_state(keys)["quick_start"]["card"], "card:tiny");
    // A short window with every option open: making it stays on the screen.
    const auto sm = read_state(small);
    const auto make = sm["anchors"]["make"];
    ASSERT_TRUE(make.is_array()) << sm["anchors"].dump();
    EXPECT_LE(make[3].get<double>(), 560.0) << make.dump();
}

XTEST(LubanOs, AnArchiveIsPreviewedBeforeItIsImportedAndWhatItMayDoIsAgreedTo, .area = "luban",
      .covers = {"LUBAN-OS-IMPORT"}, .requires_ = {"xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-import-preview");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto file = home.root() / "shared-box.luban", st = home.root() / "import.json";
    write_luban_(file, shared_box_manifest_());
    tk::write_file(home.root() / "notes.txt", "not an archive");
    const std::string can = app_can_run_(home) ? "true" : "false";
    auto r = run_script(home, std::format(R"(wait quick-start
drop {0}
wait import
expect import.edition subos:luban-tiny
expect import.name shared-box
expect import.wider.0 can copy to this computer's clipboard
expect import.can_go false
frames 2
screenshot {2}
click import:confirm
expect import.can_go {3}
state {1}
click import:cancel
wait quick-start
drop {4}
frames 3
expect view quick-start
)", file.string(), st.string(), artifact("import-preview.bmp").string(), can, (home.root() / "notes.txt").string()),
                        {}, {{"LUBAN_OS_LANG", "en_US.UTF-8"}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(read_state(st)["import"]["error"], "");
}

// Someone's system, imported from its archive, is made from its edition and
// has what the archive carried -- the whole way, as a person does it.
XTEST(LubanOs, AnImportedSystemHasWhatItsArchiveCarried, .area = "luban",
      .covers = {"LUBAN-OS-IMPORT", "LUBAN-OS-FIRST-USE"}, .requires_ = {"linux", "xlings-bin", "static-xlings", "sandbox", "network"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-import");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto file = home.root() / "shared-box.luban";
    write_luban_(file, shared_box_manifest_(), {{"root/carried.txt", "carried-across\n"}});
    auto r = run_script(home, std::format(R"(wait quick-start
drop {}
wait import
click import:confirm
click import:go
wait job
wait console 900
see 120s root@shared-box
type cat /root/carried.txt
key Return
see 30s carried-across
screenshot {}
)", file.string(), artifact("import-console.bmp").string()),
                        {}, {{"LUBAN_OS_LANG", "en_US.UTF-8"}}, std::chrono::seconds(1500));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
}


// The status-area icon's menu reaches every page, and closing the window
// with the app kept leaves it running -- in the tray where the desktop has
// one, minimized where it has none (design 2026-10-16 L §13.12).
XTEST(LubanOs, TheTrayReachesEveryPageAndClosingKeepsTheApp, .area = "luban", .covers = {"LUBAN-OS-TRAY"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-tray");
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0,"keep_in_tray":true})");
    const auto st = home.root() / "after-close.json";
    auto r = run_script(home, std::format(R"(wait launcher
tray new
wait quick-start
tray settings
wait admin
expect admin_tab settings
tray open
frames 2
close
frames 4
expect view launcher
state {}
tray quit
)", st.string()), {}, {{"DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/bus"}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto s = read_state(st);
    // No bus here: no icon, said why, and the window was kept (minimized).
    EXPECT_FALSE(s["tray"]["error"].get<std::string>().empty()) << s["tray"].dump();
    EXPECT_FALSE(s["tray"]["hidden"].get<bool>());
}

// One tray process for a home, however many windows (design 2026-10-19 §3):
// each window finds it -- the first starts it -- and it ends when no window
// needs it and nothing runs.
XTEST(LubanOs, EveryWindowOfAHomeSharesOneTrayThatEndsWhenNothingNeedsIt, .area = "luban",
      .covers = {"LUBAN-OS-TRAY-PROCESS", "LUBAN-OS-TRAY"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-one-tray");
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0})");
    std::array<fs::path, 2> states{home.root() / "a.json", home.root() / "b.json"};
    std::array<tk::RunResult, 2> runs;
    std::array<std::thread, 2> windows;
    for (std::size_t k = 0; k < 2; ++k)
        windows[k] = std::thread([&, k] {
            // Each its own script file: the two run at once.
            const auto file = home.root() / std::format("script-{}.txt", k);
            tk::write_file(file, std::format(R"(wait launcher
tray new
wait quick-start
sleep 1500
state {}
)", states[k].string()));
            tk::RunOptions o;
            o.argv = {app_binary()->string(), "--headless", "--script", file.string()};
            o.env = home.env();
            o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
            o.env["SDL_VIDEODRIVER"] = "offscreen";
            o.env["SDL_RENDER_DRIVER"] = "software";
            o.env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/nonexistent/bus";
            o.timeout = std::chrono::seconds(120);
            runs[k] = tk::run(o);
        });
    for (auto& w : windows) w.join();
    for (const auto& r : runs) ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = read_state(states[0])["tray"], b = read_state(states[1])["tray"];
    // The menu's "new" reached each window through the tray, and it was one tray.
    EXPECT_GT(a["pid"].get<int>(), 0) << a.dump();
    EXPECT_EQ(a["pid"], b["pid"]) << a.dump() << b.dump();
    EXPECT_FALSE(a["error"].get<std::string>().empty()) << "no status area here: said";
    // No window left, nothing running, the icon not kept: the tray ends.
    const auto proc = fs::path("/proc") / std::to_string(a["pid"].get<int>());
    for (int k = 0; k < 100 && fs::exists(proc); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(fs::exists(proc)) << "the tray outlived what it was for";
}

// A tray kept with nothing running (design 2026-10-20 §3.1, G20, G21): still
// it does next to nothing, and `luban-os --quit` ends it -- not only its menu.
XTEST(LubanOs, AKeptTrayIsStillAndQuitEndsIt, .area = "luban", .covers = {"LUBAN-OS-TRAY-PROCESS"},
      .requires_ = {"linux", "xlings-bin"}, .resources = {"cpu"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-tray-quit");
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0,"keep_in_tray":true})");
    const auto state = home.root() / "state.json";
    auto r = run_script(home, std::format(R"(wait launcher
tray open
frames 3
state {}
)", state.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const int pid = read_state(state)["tray"]["pid"].get<int>();
    ASSERT_GT(pid, 0);
    const auto proc = fs::path("/proc") / std::to_string(pid);
    auto cpu_ticks = [&] {
        std::istringstream in(tk::read_file(proc / "stat"));
        std::string field;
        long long ticks = 0;
        for (int k = 1; k <= 15 && in >> field; ++k)
            if (k == 14 || k == 15) ticks += std::stoll(field);
        return ticks;
    };
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_TRUE(fs::exists(proc)) << "kept: the tray stays with no window";
    const auto before = cpu_ticks();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    EXPECT_LE(cpu_ticks() - before, 10) << "a still tray took more than 100 ms of CPU in 5 s";
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "--quit"};
    o.env = home.env();
    const auto quit = tk::run(o);
    EXPECT_EQ(quit.exit_code, 0) << quit.transcript();
    for (int k = 0; k < 50 && fs::exists(proc); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(fs::exists(proc)) << "luban-os --quit left the tray";
    const auto again = tk::run(o);
    EXPECT_NE(again.out.find("not running"), std::string::npos) << again.transcript();
}

// Visual verification (design 2026-10-16 L §13.8): the key pages at three
// scales and in both languages -- every control on the screen, apart, whole
// and big enough to hit; where each lies matches its baseline
// (luban/tests/luban-os/baseline, the layout reviewed and kept); and one contact
// sheet of them all for a person (and the review) to look at. A baseline
// that is not there yet is recorded into the artifacts, to be reviewed and
// committed.
namespace {
// Several screenshots side by side, each scaled down to `cell` wide.
void contact_sheet_(const std::vector<std::pair<std::string, fs::path>>& shots, const fs::path& out, int cell = 320) {
    const int cols = 6;
    std::vector<Image> images;
    int cellH = 0;
    for (const auto& [_, f] : shots) {
        images.push_back(read_ppm(f));
        cellH = std::max(cellH, images.back().height * cell / std::max(1, images.back().width));
    }
    const int rows = (static_cast<int>(images.size()) + cols - 1) / cols;
    const int W = cols * (cell + 4), H = rows * (cellH + 4);
    std::string rgb(static_cast<std::size_t>(W) * H * 3, '\x10');
    for (std::size_t i = 0; i < images.size(); ++i) {
        const auto& img = images[i];
        const int ox = static_cast<int>(i % cols) * (cell + 4) + 2, oy = static_cast<int>(i / cols) * (cellH + 4) + 2;
        const int h = img.height * cell / std::max(1, img.width);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < cell; ++x) {
                const auto px = img.at(x * img.width / cell, y * img.height / std::max(1, h));
                const auto k = (static_cast<std::size_t>(oy + y) * W + ox + x) * 3;
                rgb[k] = static_cast<char>(px[0]); rgb[k + 1] = static_cast<char>(px[1]); rgb[k + 2] = static_cast<char>(px[2]);
            }
    }
    std::ofstream(out, std::ios::binary) << std::format("P6\n{} {}\n255\n", W, H) << rgb;
}
}  // namespace

XTEST(LubanOs, KeyPagesHoldTheirLayoutAtEveryScaleInBothLanguages, .area = "luban",
      .covers = {"LUBAN-OS-LAYOUT", "LUBAN-OS-VISUAL"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-visual");
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0})");
    const fs::path baseline_dir = fs::path(__FILE__).parent_path().parent_path() / "luban-os" / "baseline";
    std::vector<std::pair<std::string, fs::path>> shots;
    std::vector<std::string> recorded;
    bool unchecked = false;
    const std::vector<std::string> pages{"launcher", "quick-start", "quick-start-more", "admin", "app-settings"};
    for (const auto* scale : {"1", "1.5", "2"}) {
        for (const std::string lang : {"en", "zh"}) {
            const auto tag = std::format("{}x-{}", scale, lang);
            const auto dir = home.root() / tag;
            fs::create_directories(dir);
            // The same page at every scale: a window as large, in its points.
            const int ww = static_cast<int>(1280 * std::stof(scale)), wh = static_cast<int>(800 * std::stof(scale));
            auto r = run_script(home, std::format(R"(resize {1} {2}
wait launcher
frames 3
state {0}/launcher.json
screenshot {0}/launcher.ppm
click new
wait quick-start
waitfor host.known true 60
waitfor host.editions_known true 60
frames 3
state {0}/quick-start.json
screenshot {0}/quick-start.ppm
click more
frames 3
state {0}/after-more.json
screenshot {0}/after-more.ppm
waitfor quick_start.more true 5
frames 3
state {0}/quick-start-more.json
screenshot {0}/quick-start-more.ppm
click back
wait launcher
click gear
wait admin
frames 3
state {0}/admin.json
screenshot {0}/admin.ppm
click tab:settings
frames 3
state {0}/app-settings.json
screenshot {0}/app-settings.ppm
)", dir.string(), ww, wh), {"--scale", scale}, {{"LUBAN_OS_LANG", lang == "zh" ? "zh_CN.UTF-8" : "en_US.UTF-8"}});
            // What the run had drawn when it stopped, for the reader of a failure.
            if (r.exit_code != 0)
                for (const auto& e : fs::directory_iterator(dir)) {
                    std::error_code ec;
                    fs::create_directories(artifact("layout") / tag, ec);
                    fs::copy_file(e.path(), artifact("layout") / tag / e.path().filename(), fs::copy_options::overwrite_existing, ec);
                }
            ASSERT_EQ(r.exit_code, 0) << tag << "\n" << r.transcript();
            for (const auto& page : pages) {
                const auto s = read_state(dir / (page + ".json"));
                // What the page was, beside the run's other evidence.
                tk::write_file(artifact("layout") / std::format("{}-{}.json", page, tag), s.dump(1) + "\n");
                // The baselines are the shipped fonts' layout: with this
                // machine's fonts a control is in the window, apart and large
                // enough to hit, but not where the baseline has it.
                const bool shipped = !s.value("fonts", std::string()).empty();
                if (const char* f = std::getenv("LUBAN_OS_FONTS"); f && *f)
                    ASSERT_TRUE(shipped) << tag << " " << page << ": LUBAN_OS_FONTS=" << f << " is not what the app drew with";
                shots.emplace_back(tag + " " + page, dir / (page + ".ppm"));
                const float W = static_cast<float>(ww), H = static_cast<float>(wh);
                std::vector<std::pair<std::string, std::array<float, 4>>> controls;
                for (auto it = s["anchors"].begin(); it != s["anchors"].end(); ++it)
                    controls.emplace_back(it.key(), std::array<float, 4>{it.value()[0].get<float>(), it.value()[1].get<float>(),
                                                                         it.value()[2].get<float>(), it.value()[3].get<float>()});
                ASSERT_FALSE(controls.empty()) << tag << " " << page;
                for (const auto& [name, c] : controls) {
                    EXPECT_GE(c[0], 0) << tag << " " << page << " " << name;
                    EXPECT_GE(c[1], 0) << tag << " " << page << " " << name;
                    EXPECT_LE(c[2], W) << tag << " " << page << " " << name << " runs off the window";
                    EXPECT_LE(c[3], H) << tag << " " << page << " " << name << " runs off the window";
                    EXPECT_GE(c[3] - c[1], 20) << tag << " " << page << " " << name << " is too small to hit";
                }
                for (std::size_t i = 0; i < controls.size(); ++i)
                    for (std::size_t k = i + 1; k < controls.size(); ++k) {
                        const auto& a = controls[i].second;
                        const auto& b = controls[k].second;
                        const bool apart = a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1];
                        EXPECT_TRUE(apart) << tag << " " << page << ": " << controls[i].first << " and " << controls[k].first << " overlap";
                    }
                // The baseline: the same controls, where they were (within
                // what another machine's fonts move them).
                const auto baseline = baseline_dir / std::format("{}-{}.json", page, tag);
                Json layout = Json::object();
                for (const auto& [name, c] : controls) layout[name] = Json::array({c[0], c[1], c[2], c[3]});
                if (!shipped) {
                    unchecked = true;
                    continue;
                }
                if (!fs::exists(baseline)) {
                    tk::write_file(artifact("baseline") / baseline.filename(), layout.dump(1) + "\n");
                    recorded.push_back(baseline.filename().string());
                    continue;
                }
                const auto want = Json::parse(tk::read_file(baseline));
                const auto clipped = s.value("clipped", Json::array());
                for (auto it = want.begin(); it != want.end(); ++it) {
                    // Another machine's fonts can make a page taller than the
                    // window: a control then scrolled partly out is there, a
                    // scroll away -- not gone, and not where it was.
                    if (!layout.contains(it.key()) && std::ranges::find(clipped, Json(it.key())) != clipped.end()) continue;
                    EXPECT_TRUE(layout.contains(it.key())) << tag << " " << page << ": " << it.key() << " is gone";
                    if (!layout.contains(it.key())) continue;
                    for (int k = 0; k < 4; ++k) {
                        const float was = it.value()[k].get<float>(), now = layout[it.key()][k].get<float>();
                        EXPECT_NEAR(now, was, std::max(8.0f, (k % 2 ? H : W) * 0.03f))
                            << tag << " " << page << ": " << it.key() << " moved (edge " << k << ")";
                    }
                }
            }
        }
    }
    // The pages of a failed run, whole, beside their states.
    if (::testing::Test::HasFailure())
        for (const auto& [name, file] : shots) {
            std::error_code ec;
            fs::copy_file(file, artifact("layout") / (name + ".ppm"), fs::copy_options::overwrite_existing, ec);
        }
    contact_sheet_(shots, artifact("contact-sheet.ppm"));
    if (!recorded.empty())
        std::println(std::cerr, "luban-os: {} layout baselines recorded into {} (review, then commit to luban/tests/luban-os/baseline)",
                     recorded.size(), (tk::artifacts_dir() / "luban-os" / "baseline").string());
    if (unchecked)
        std::println(std::cerr, "luban-os: drawn with this machine's fonts, held to no baseline "
                                "(tools/ci/luban_fonts.sh fetches the shipped ones into LUBAN_OS_FONTS)");
}

// Keeping up (design 2026-10-16 L §13.3): a choice shows the frame after it
// is made -- the app does not sleep after input (it once waited a second,
// and a card took 1-2 s to look chosen); what the app itself spends on a
// frame is small, never a read of the disk per frame; and a still window
// waits instead of drawing for nothing. The renderer here is SDL's software
// one (a machine without a GPU), so the whole frame's budget is its.
XTEST(LubanOs, AChoiceShowsTheNextFrameAndAStillWindowRests, .area = "luban", .covers = {"LUBAN-OS-PERF"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = home_with_two("luban-os-perf");
    tk::write_file(home.dir() / "config" / "luban-os.json", R"({"countdown":0})");
    const auto st = home.root() / "perf.json", idle = home.root() / "idle.json";
    auto r = run_script(home, std::format(R"(resize 1280 800
wait launcher
frames 3
perf reset
key Down
expect selected 1
key Down
expect selected 2
click new
wait quick-start
waitfor host.known true 60
waitfor host.editions_known true 60
click card:nano
expect edition nano
click card:tiny
expect edition tiny
click card:core
expect edition core
click more
waitfor anchors.proxy present 5
click back
wait launcher
click gear
wait admin
click tab:activity
expect admin_tab activity
click tab:settings
expect admin_tab settings
click tab:systems
click admin:beta
expect admin_selected beta
click page:settings
expect admin_page 2
state {}
click admin:back
wait launcher
frames 30
state {}
)", st.string(), idle.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto p = read_state(st)["perf"];
    EXPECT_LE(p["wait_after_input_ms"].get<int>(), 16) << p.dump();
    EXPECT_LE(p["draw_ms_p95"].get<double>(), 8.0) << "the app's own work per frame\n" << p.dump();
    EXPECT_LE(p["draw_ms_max"].get<double>(), 60.0) << p.dump();
    // Most of a frame is SDL's software renderer presenting 1280x800 (a CI
    // runner: ~70 ms); the app's own share is draw_ms above.
    EXPECT_LE(p["frame_ms_p95"].get<double>(), 160.0) << "a frame, software renderer included\n" << p.dump();
    EXPECT_LE(p["input_to_next_frame_ms_max"].get<double>(), 300.0) << "input until its effect is on screen\n" << p.dump();
    EXPECT_GE(p["frames"].get<int>(), 20);
    const auto q = read_state(idle)["perf"];
    EXPECT_GE(q["idle_wait_ms"].get<int>(), 100) << "a still window rests\n" << q.dump();
}

// A system's sound plays on this computer (design 2026-10-20 §20.1): its
// openhal stream through the window's speaker -- SDL's, here its disk
// driver, a file -- the same samples.
XTEST(LubanOs, ASystemsSoundPlaysOnThisComputer, .area = "luban", .covers = {"LUBAN-SOUND"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    auto home = tk::Home::isolated("luban-os-sound");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    std::string wave;
    for (int k = 0; k < 4800; ++k)
        for (int ch = 0; ch < 2; ++ch) {
            const auto v = static_cast<std::uint16_t>((k * 53 + ch * 777) & 0x7fff);
            wave += static_cast<char>(v & 0xff);
            wave += static_cast<char>(v >> 8);
        }
    namespace audio = openhal::audio;
    const auto stream = home.root() / "stream.oha";
    tk::write_file(stream, audio::encode(audio::Open{.stream = 1, .format = {48000, 2}}) + audio::encode(audio::Samples{.stream = 1, .bytes = wave}));
    const auto played = home.root() / "played.raw";
    const auto r = run_script(home, std::format("play {}\n", stream.string()), {},
                              {{"SDL_AUDIO_DRIVER", "disk"}, {"SDL_AUDIO_DISK_OUTPUT_FILE", played.string()}, {"SDL_AUDIO_DISK_TIMESCALE", "4"}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto out = tk::read_file(played);
    EXPECT_NE(out.find(wave.substr(0, 4096)), std::string::npos) << "the samples are what was played (" << out.size() << " bytes)";
}
