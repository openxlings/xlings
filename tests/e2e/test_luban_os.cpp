// The Luban OS app (Luban OS architecture design §14), headless (SDL's
// offscreen driver): what a person sees first, with no system and with one;
// the management panel; and that it draws something.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;

namespace tk = xlings::testkit;
namespace platform = xlings::platform;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::optional<fs::path> app_binary() {
    if (const char* forced = std::getenv("LUBAN_OS_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& c : {xlings.parent_path() / "luban-os", xlings.parent_path() / "luban-os" / "luban-os",
                          xlings.parent_path() / "luban-os.exe", xlings.parent_path() / "luban-os" / "luban-os.exe"})
        if (fs::is_regular_file(c)) return c;
    return std::nullopt;
}

tk::RunResult app(const tk::Home& home, std::vector<std::string> args) {
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "--headless", "--frames", "4", "--dump-state"};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    o.timeout = std::chrono::seconds(60);
    return tk::run(o);
}

Json state(const tk::RunResult& r) {
    std::istringstream in(r.out);
    Json last;
    for (std::string line; std::getline(in, line);)
        if (auto j = Json::parse(line, nullptr, false); j.is_object() && j.contains("view")) last = j;
    return last;
}

}  // namespace

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

namespace {

struct Image {
    int width { 0 }, height { 0 };
    std::string rgb;
    std::array<int, 3> at(int x, int y) const {
        const auto k = (std::size_t(y) * width + x) * 3;
        return {static_cast<unsigned char>(rgb[k]), static_cast<unsigned char>(rgb[k + 1]), static_cast<unsigned char>(rgb[k + 2])};
    }
};

Image read_ppm(const fs::path& file) {
    const auto bytes = tk::read_file(file);
    Image img;
    std::istringstream head(bytes);
    std::string magic;
    int maxval = 0;
    head >> magic >> img.width >> img.height >> maxval;
    const auto at = static_cast<std::size_t>(head.tellg()) + 1;
    img.rgb = bytes.substr(at);
    return img;
}

fs::path artifact(std::string_view name) {
    const auto dir = tk::artifacts_dir() / "luban-os";
    fs::create_directories(dir);
    return dir / name;
}

tk::RunResult run_script(const tk::Home& home, std::string_view script, std::vector<std::string> args = {},
                         std::map<std::string, std::string> env = {}, std::chrono::seconds timeout = std::chrono::seconds(240)) {
    const auto file = home.root() / "script.txt";
    tk::write_file(file, script);
    tk::RunOptions o;
    o.argv = {app_binary()->string(), "--headless", "--script", file.string()};
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.env = home.env();
    o.env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    o.env["SDL_VIDEODRIVER"] = "offscreen";
    // The renderer a machine without a GPU has, here as in CI.
    o.env["SDL_RENDER_DRIVER"] = "software";
    for (auto& [k, v] : env) o.env[k] = v;
    o.timeout = timeout;
    return tk::run(o);
}

Json read_state(const fs::path& file) { return Json::parse(tk::read_file(file)); }

// Whether this computer can run a Luban system, as the app learned it from
// the capability report (luban status): Linux with isolation, a Mac or a PC
// with its Luban host machine -- and, on a CI runner without them, not.
bool app_can_run_(const tk::Home& home) {
    const auto st = home.root() / "host.json";
    auto r = run_script(home, std::format("wait quick-start\nwaitfor host.known true 60\nstate {}\n", st.string()));
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    if (r.exit_code != 0) return platform::is_linux;
    return read_state(st)["host"]["can_run"].get<bool>();
}

std::array<float, 4> rect(const Json& state, const std::string& anchor) {
    const auto& r = state.at("anchors").at(anchor);
    return {r[0].get<float>(), r[1].get<float>(), r[2].get<float>(), r[3].get<float>()};
}

tk::Home home_with_two(std::string_view tag) {
    auto home = tk::Home::isolated(tag);
    EXPECT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    EXPECT_EQ(home.xlings({"subos", "new", "alpha"}).exit_code, 0);
    EXPECT_EQ(home.xlings({"subos", "new", "beta"}).exit_code, 0);
    return home;
}

}  // namespace

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
click card:nano
expect edition nano
expect name nano-1
click card:tiny
expect edition tiny
expect name tiny-1
click more
waitfor anchors.proxy present 5
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
    EXPECT_NE(sent.find("H 399 1100 700 1 1"), std::string::npos)
        << "the window's interfaces (surface, pointer, keyboard, mode, text, clipboard), size, density and pacing, first\n" << sent;
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

// xdesk (apps/xdesk), built beside the app; XDESK_BIN names another.
std::optional<fs::path> xdesk_binary() {
    if (const char* forced = std::getenv("XDESK_BIN"); forced && *forced) return fs::path(forced);
    const auto app = app_binary();
    if (!app) return std::nullopt;
    for (const auto& c : {app->parent_path() / "xdesk", app->parent_path().parent_path() / "xdesk-app" / "xdesk"})
        if (fs::is_regular_file(c)) return c;
    return std::nullopt;
}

XTEST(LubanOs, XdeskIsADesktopAtTheWindowsSize, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "LUBAN-OS-INTERACTION"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    const auto xdesk = xdesk_binary();
    if (!xdesk) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto shot = home.root() / "xdesk.ppm", small = home.root() / "xdesk-small.ppm";
    const auto st = home.root() / "xdesk.json", st_small = home.root() / "xdesk-small.json";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 500
frames 2
screenshot {}
screenshot {}
state {}
resize 900 600
waitfor desktop.texture.0 900
sleep 500
frames 2
screenshot {}
state {}
)", shot.string(), artifact("xdesk-classic.bmp").string(), st.string(), small.string(), st_small.string()),
                        {"--desktop-exec", xdesk->string(), "--profile", "classic"},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Its own picture -- the desktop's background -- at the size it was asked.
    EXPECT_EQ(read_state(st)["desktop"]["texture"], Json::array({1100, 700}));
    for (const auto& [img, state] : {std::pair{read_ppm(shot), read_state(st)}, std::pair{read_ppm(small), read_state(st_small)}}) {
        const auto d = rect(state, "desktop");
        const auto px = img.at(static_cast<int>(d[0]) + 40, static_cast<int>(d[1]) + 40);
        EXPECT_NEAR(px[0], 0x1b, 2) << state.dump();
        EXPECT_NEAR(px[1], 0x1e, 2);
        EXPECT_NEAR(px[2], 0x25, 2);
    }
}

// A Wayland client in one colour (xdg-shell over the plain wire, its pixels
// in shared memory), so where xdesk puts a window is read off the picture;
// with --log it also takes text (text-input-v3) and the clipboard, and says
// what it got.
fs::path colour_client(const tk::Home& home) {
    const auto file = home.root() / "client.py";
    tk::write_file(file, R"PY(# A Wayland client in one colour: xdg-shell over the plain wire, its pixels
# in shared memory. Closes when asked. argv: RRGGBB [--log FILE] [--copy TEXT]
#   --log   text-input-v3 on: what is committed, and the selection it is
#           offered, written to FILE ("commit <text>", "paste <text>")
#   --copy  then its own selection: TEXT
import os, socket, struct, sys, mmap, tempfile, array
colour = int(sys.argv[1], 16)
opts = dict(zip(sys.argv[2::2], sys.argv[3::2]))
log = open(opts["--log"], "a", buffering=1) if "--log" in opts else None
copy = opts.get("--copy")
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], os.environ["WAYLAND_DISPLAY"]))
next_id = [2]
def new():
    i = next_id[0]; next_id[0] += 1; return i
def string(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)
def read_string(body):
    n, = struct.unpack("<I", body[:4])
    return body[4:4 + n - 1].decode(errors="replace") if n else None, body[4 + n + (-n % 4):]
def send(obj, op, args=b"", fds=()):
    msg = struct.pack("<II", obj, ((8 + len(args)) << 16) | op) + args
    if fds: sock.sendmsg([msg], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("<%di" % len(fds), *fds))])
    else: sock.sendall(msg)
buf = b""
fds = []
def events():
    global buf
    while True:
        while len(buf) >= 8:
            obj, word = struct.unpack("<II", buf[:8])
            size, op = word >> 16, word & 0xffff
            if len(buf) < size: break
            body, buf = buf[8:size], buf[size:]
            yield obj, op, body
        data, anc, _, _ = sock.recvmsg(65536, socket.CMSG_SPACE(16 * 4))
        for level, kind, payload in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                a = array.array("i"); a.frombytes(payload[:len(payload) - len(payload) % 4]); fds.extend(a)
        if not data: return
        buf += data
it = events()
registry = new(); send(1, 1, struct.pack("<I", registry))
sync = new(); send(1, 0, struct.pack("<I", sync))
globals_ = {}
for obj, op, body in it:
    if obj == registry and op == 0:
        name, = struct.unpack("<I", body[:4]); iface, rest = read_string(body[4:])
        globals_[iface] = (name, struct.unpack("<I", rest[:4])[0])
    elif obj == sync: break
def bind(iface, ver):
    name, have = globals_[iface]; i = new()
    send(registry, 0, struct.pack("<I", name) + string(iface) + struct.pack("<II", min(ver, have), i))
    return i
compositor, shm, wm = bind("wl_compositor", 4), bind("wl_shm", 1), bind("xdg_wm_base", 1)
surface = new(); send(compositor, 0, struct.pack("<I", surface))
xdg_surface = new(); send(wm, 2, struct.pack("<II", xdg_surface, surface))
toplevel = new(); send(xdg_surface, 1, struct.pack("<I", toplevel))
send(toplevel, 2, string(sys.argv[1]))                      # set_title
keyboard = text = device = source = None
if log:
    seat = bind("wl_seat", 5)
    keyboard = new(); send(seat, 1, struct.pack("<I", keyboard))            # get_keyboard
    inputs = bind("zwp_text_input_manager_v3", 1)                         # ids in the order they are sent
    text = new(); send(inputs, 1, struct.pack("<II", text, seat))
    manager = bind("wl_data_device_manager", 3)
    device = new(); send(manager, 1, struct.pack("<II", device, seat))     # get_data_device
send(surface, 6)                                             # commit
size = (0, 0); drawn = None; offers = {}; pasted = False; enter_serial = 0
def draw(w, h):
    stride = w * 4
    f = tempfile.TemporaryFile()          # unlinked: memory the compositor maps
    fd = f.fileno()
    os.ftruncate(fd, stride * h)
    with mmap.mmap(fd, stride * h) as m:
        m.write(struct.pack("<I", 0xff000000 | colour) * (w * h))
    pool = new(); send(shm, 0, struct.pack("<Ii", pool, stride * h), [fd])
    buffer = new(); send(pool, 0, struct.pack("<IiiiiI", buffer, 0, w, h, stride, 1))
    f.close()
    send(surface, 1, struct.pack("<Iii", buffer, 0, 0))     # attach
    send(surface, 2, struct.pack("<iiii", 0, 0, w, h))      # damage
    send(surface, 6)
for obj, op, body in it:
    if obj == toplevel and op == 0:
        w, h = struct.unpack("<ii", body[:8]); size = (w or 400, h or 300)
    elif obj == toplevel and op == 1:
        break                                                # close
    elif obj == xdg_surface and op == 0:
        send(xdg_surface, 4, body[:4])                       # ack_configure
        if size != drawn: draw(*size); drawn = size
    elif obj == wm and op == 0:
        send(wm, 3, body[:4])                                # pong
    elif obj == keyboard and op == 0:
        os.close(fds.pop(0))                                 # the keymap: not needed
    elif obj == keyboard and op == 1:
        enter_serial, = struct.unpack("<I", body[:4])
    elif obj == text and op == 0:                            # enter: on, a caret at 20,30
        send(text, 1); send(text, 6, struct.pack("<iiii", 20, 30, 2, 18)); send(text, 7)
    elif obj == text and op == 3:
        s, _ = read_string(body)
        if s: log.write("commit %s\n" % s)
    elif obj == text and op == 5:
        send(text, 7)                                        # done: commit the state again
    elif obj == device and op == 0:
        offers[struct.unpack("<I", body[:4])[0]] = []        # data_offer
    elif obj in offers and op == 0:
        offers[obj].append(read_string(body)[0])             # offer: a mime type
    elif obj == device and op == 5:                          # selection
        offer, = struct.unpack("<I", body[:4])
        if offer and not pasted and "text/plain;charset=utf-8" in offers.get(offer, []):
            r, w = os.pipe()
            send(offer, 1, string("text/plain;charset=utf-8"), [w]); os.close(w)
            got = b""
            while True:
                chunk = os.read(r, 65536)
                if not chunk: break
                got += chunk
            os.close(r)
            log.write("paste %s\n" % got.decode(errors="replace"))
            pasted = True
            if copy:
                source = new(); send(manager, 0, struct.pack("<I", source))
                send(source, 0, string("text/plain;charset=utf-8"))
                send(device, 1, struct.pack("<II", source, enter_serial))
    elif obj == source and op == 1:                          # send: the text into the fd
        read_string(body)                                    # the mime type: there is one
        fd = fds.pop(0); os.write(fd, copy.encode()); os.close(fd)
)PY");
    return file;
}

tk::RunResult xdesk_with(const tk::Home& home, std::string_view profile, std::string_view script) {
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    // Three windows, one after the other: red, green, blue.
    return run_script(home, script,
                      {"--desktop-exec", xdesk_binary()->string(), "--profile", std::string(profile),
                       "--autostart", std::format("python3 {} c04040", client),
                       "--autostart", std::format("sh -c 'sleep 0.6; exec python3 {} 40c040'", client),
                       "--autostart", std::format("sh -c 'sleep 1.2; exec python3 {} 4040c0'", client)},
                      {{"XDG_RUNTIME_DIR", runtime.string()}});
}

constexpr std::array<int, 3> kRed { 0xc0, 0x40, 0x40 }, kGreen { 0x40, 0xc0, 0x40 }, kBlue { 0x40, 0x40, 0xc0 };

XTEST(LubanOs, XdeskClassicTilesWindowsAndScrollsToTheFocusedOne, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-LAYOUT"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-tile");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto three = home.root() / "three.ppm", left = home.root() / "left.ppm";
    auto r = xdesk_with(home, "classic", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
screenshot {}
screenshot {}
key Alt+Left
key Alt+Left
sleep 1000
screenshot {}
)", three.string(), artifact("xdesk-classic-three.bmp").string(), left.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Three halves: the last two shown, the newest (focused) on the right.
    const auto a = read_ppm(three);
    EXPECT_EQ(a.at(275, 400), kGreen);
    EXPECT_EQ(a.at(825, 400), kBlue);
    // The focus two to the left: the strip scrolled back to the first.
    const auto b = read_ppm(left);
    EXPECT_EQ(b.at(275, 400), kRed);
    EXPECT_EQ(b.at(825, 400), kGreen);
}

XTEST(LubanOs, XdeskCanvasShowsEveryWindowInItsOverviewAndAClickGoesIn, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-LAYOUT"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-canvas");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto all = home.root() / "overview.ppm", in = home.root() / "in.ppm";
    auto r = xdesk_with(home, "canvas", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
key Alt+o
sleep 1000
screenshot {}
screenshot {}
click desktop
sleep 1000
screenshot {}
)", all.string(), artifact("xdesk-canvas-overview.bmp").string(), in.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Zoomed out: the three side by side, the middle one in the middle.
    const auto a = read_ppm(all);
    std::set<std::array<int, 3>> seen;
    for (int x = 0; x < 1100; x += 10) seen.insert(a.at(x, 350));
    EXPECT_TRUE(seen.contains(kRed) && seen.contains(kGreen) && seen.contains(kBlue));
    EXPECT_EQ(a.at(550, 350), kGreen);
    // A click on it: back to 1, it in the middle of the view.
    const auto b = read_ppm(in);
    EXPECT_EQ(b.at(550, 350), kGreen);
    EXPECT_EQ(b.at(150, 350), kGreen) << "still zoomed out (it is as wide as it was made)";
    EXPECT_EQ(b.at(950, 350), kGreen);
    EXPECT_NE(a.at(150, 350), kGreen) << "the overview did not zoom out";
}

XTEST(LubanOs, XdeskLauncherStartsWhatIsTypedAndThePanelSwitchesWorkspaces, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-SHELL", "XDESK-APPS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-launcher");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", data = home.root() / "share";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    fs::create_directories(data / "applications");
    // The one application there: a green window.
    tk::write_file(data / "applications" / "greenery.desktop",
                   std::format("[Desktop Entry]\nType=Application\nName=Greenery\nExec=python3 {} 40c040\n",
                               colour_client(home).string()));
    const auto open = home.root() / "open.ppm", started = home.root() / "started.ppm", other = home.root() / "other.ppm";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 500
key Alt+space
sleep 300
type gree
sleep 300
screenshot {}
screenshot {}
key Return
sleep 2500
screenshot {}
screenshot {}
click 109 15
sleep 500
screenshot {}
)", open.string(), artifact("xdesk-launcher.bmp").string(), started.string(), artifact("xdesk-launched.bmp").string(),
                                          other.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic"},
                        {{"XDG_RUNTIME_DIR", runtime.string()}, {"XDG_DATA_DIRS", data.string()}, {"HOME", home.root().string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // The launcher is up, its first row chosen (the accent's blue).
    const auto a = read_ppm(open);
    EXPECT_NE(a.at(550, 210), (std::array<int, 3>{0x1b, 0x1e, 0x25})) << "no launcher over the desktop";
    // Enter started it: green fills the output under the panel.
    const auto b = read_ppm(started);
    EXPECT_EQ(b.at(550, 400), kGreen);
    // Workspace 2 from the panel: empty.
    const auto c = read_ppm(other);
    EXPECT_NE(c.at(550, 400), kGreen);
}

XTEST(LubanOs, XdeskGivesTheHostsInputMethodAndClipboardToItsWindows, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-TEXT-CLIPBOARD"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-text");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", got = home.root() / "client.log", st = home.root() / "text.json";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    auto r = run_script(home, std::format(R"(clipboard from-the-host
wait desktop
waitfor desktop.texture.0 1100
waitfor desktop_text true
commit 你好
commit abc
waitfor clipboard from-the-guest
state {}
)", st.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                         std::format("python3 {} c04040 --log {} --copy from-the-guest", colour_client(home).string(), got.string())},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript() << tk::read_file(got);
    const auto log = tk::read_file(got);
    // The host's clipboard reached the window as its selection...
    EXPECT_NE(log.find("paste from-the-host"), std::string::npos) << log;
    // ...what the input method committed reached it as text (a caret was asked for)...
    EXPECT_NE(log.find("commit 你好"), std::string::npos) << log;
    EXPECT_NE(log.find("commit abc"), std::string::npos) << log;
    EXPECT_EQ(log.find("commit a\n"), std::string::npos) << "a character arrived twice (as a key too)\n" << log;
    // ...and what it copied is the host's clipboard.
    EXPECT_EQ(read_state(st)["clipboard"], "from-the-guest");
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

XTEST(LubanOs, XdeskTakesAPersonsSettingsOverItsProfiles, .area = "luban", .covers = {"XDESK-CONFIG"},
      .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-settings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", config = home.root() / "config", shot = home.root() / "set.ppm";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    fs::create_directories(config / "xdesk");
    tk::write_file(config / "xdesk" / "classic.ini", "[theme]\nbackground = #203040\n[desktop]\ngap = 0\n");
    auto r = run_script(home, std::format("wait desktop\nwaitfor desktop.texture.0 1100\nsleep 500\nframes 2\nscreenshot {}\n", shot.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic"},
                        {{"XDG_RUNTIME_DIR", runtime.string()}, {"XDG_CONFIG_HOME", config.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(read_ppm(shot).at(550, 400), (std::array<int, 3>{0x20, 0x30, 0x40}));
}

XTEST(LubanOs, XdeskDrawsForTheScreensDensityAndThePointerLandsWhereItIs, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "LUBAN-OS-INTERACTION"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-density");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run", st = home.root() / "density.json";
    const auto open = home.root() / "open.ppm", closed = home.root() / "closed.ppm";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    // A screen of density 2: the desktop is asked for twice the pixels and
    // shown at the window's size; a click on a title bar's close button
    // (logical 1079, 51) closes that window.
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 2200
sleep 2000
frames 2
state {}
screenshot {}
screenshot {}
click 1079 51
sleep 1000
frames 2
screenshot {}
)", st.string(), open.string(), artifact("xdesk-density-2.bmp").string(), closed.string()),
                        {"--density", "2", "--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                         std::format("python3 {} c04040", colour_client(home).string())},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(read_state(st)["desktop"]["texture"], Json::array({2200, 1400}));
    const auto a = read_ppm(open);
    EXPECT_EQ(a.at(550, 400), kRed);                                 // the window, at the window's size
    EXPECT_NE(a.at(550, 15), (std::array<int, 3>{0x1b, 0x1e, 0x25})) << "no panel across the top";
    EXPECT_NE(read_ppm(closed).at(550, 400), kRed) << "the click did not reach the close button";
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

// Visual verification (design 2026-10-16 L §13.8): the key pages at three
// scales and in both languages -- every control on the screen, apart, whole
// and big enough to hit; where each lies matches its baseline
// (tests/luban-os/baseline, the layout reviewed and kept); and one contact
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
frames 3
state {0}/quick-start.json
screenshot {0}/quick-start.ppm
click more
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
            ASSERT_EQ(r.exit_code, 0) << tag << "\n" << r.transcript();
            for (const auto& page : pages) {
                const auto s = read_state(dir / (page + ".json"));
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
                    ASSERT_TRUE(layout.contains(it.key())) << tag << " " << page << ": " << it.key() << " is gone";
                    for (int k = 0; k < 4; ++k) {
                        const float was = it.value()[k].get<float>(), now = layout[it.key()][k].get<float>();
                        EXPECT_NEAR(now, was, std::max(8.0f, (k % 2 ? H : W) * 0.03f))
                            << tag << " " << page << ": " << it.key() << " moved (edge " << k << ")";
                    }
                }
            }
        }
    }
    contact_sheet_(shots, artifact("contact-sheet.ppm"));
    if (!recorded.empty())
        std::println(std::cerr, "luban-os: {} layout baselines recorded into {} (review, then commit to tests/luban-os/baseline)",
                     recorded.size(), (tk::artifacts_dir() / "luban-os" / "baseline").string());
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
frames 6
state {}
)", st.string(), idle.string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto p = read_state(st)["perf"];
    EXPECT_LE(p["wait_after_input_ms"].get<int>(), 16) << p.dump();
    EXPECT_LE(p["draw_ms_p95"].get<double>(), 8.0) << "the app's own work per frame\n" << p.dump();
    EXPECT_LE(p["draw_ms_max"].get<double>(), 60.0) << p.dump();
    EXPECT_LE(p["frame_ms_p95"].get<double>(), 120.0) << "a frame, software renderer included\n" << p.dump();
    EXPECT_LE(p["input_to_next_frame_ms_max"].get<double>(), 300.0) << "input until its effect is on screen\n" << p.dump();
    EXPECT_GE(p["frames"].get<int>(), 20);
    const auto q = read_state(idle)["perf"];
    EXPECT_GE(q["idle_wait_ms"].get<int>(), 100) << "a still window rests\n" << q.dump();
}
