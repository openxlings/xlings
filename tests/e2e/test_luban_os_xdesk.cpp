// xdesk under the Luban OS window (Luban OS design 2026-10-19 §17): its
// arrangements, its pointer and keys, its menus and drags, its settings,
// driven headless through the app as test_luban_os.cpp drives the app.
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

#include "luban_os_support.hpp"

using namespace luban_os_e2e;

// xdesk (luban/xdesk/app), built beside the app; XDESK_BIN names another.
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
# in shared memory. Closes when asked. argv: RRGGBB [--log FILE] [--copy TEXT] [--cursor 1]
#   --log   text-input-v3 on: what is committed, and the selection it is
#           offered, written to FILE ("commit <text>", "paste <text>")
#   --copy  then its own selection: TEXT
#   --cursor  its own pointer over it: 8x8 in its colour, hotspot (3, 4)
#   --app-id  its application id
import os, socket, struct, sys, mmap, tempfile, array
colour = int(sys.argv[1], 16)
opts = dict(zip(sys.argv[2::2], sys.argv[3::2]))
log = open(opts["--log"], "a", buffering=1) if "--log" in opts else None
copy = opts.get("--copy")
cursor = "--cursor" in opts
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
if "--app-id" in opts: send(toplevel, 3, string(opts["--app-id"]))   # set_app_id
keyboard = text = device = source = pointer = cursor_surface = None
seat = bind("wl_seat", 5) if log or cursor else None
if cursor:
    pointer = new(); send(seat, 0, struct.pack("<I", pointer))              # get_pointer
    cursor_surface = new(); send(compositor, 0, struct.pack("<I", cursor_surface))
if log:
    keyboard = new(); send(seat, 1, struct.pack("<I", keyboard))            # get_keyboard
    inputs = bind("zwp_text_input_manager_v3", 1)                         # ids in the order they are sent
    text = new(); send(inputs, 1, struct.pack("<II", text, seat))
    manager = bind("wl_data_device_manager", 3)
    device = new(); send(manager, 1, struct.pack("<II", device, seat))     # get_data_device
send(surface, 6)                                             # commit
size = (0, 0); drawn = None; offers = {}; pasted = False; enter_serial = 0
def draw(w, h, on=None):
    on = on or surface
    stride = w * 4
    f = tempfile.TemporaryFile()          # unlinked: memory the compositor maps
    fd = f.fileno()
    os.ftruncate(fd, stride * h)
    with mmap.mmap(fd, stride * h) as m:
        m.write(struct.pack("<I", 0xff000000 | colour) * (w * h))
    pool = new(); send(shm, 0, struct.pack("<Ii", pool, stride * h), [fd])
    buffer = new(); send(pool, 0, struct.pack("<IiiiiI", buffer, 0, w, h, stride, 1))
    f.close()
    send(on, 1, struct.pack("<Iii", buffer, 0, 0))          # attach
    send(on, 2, struct.pack("<iiii", 0, 0, w, h))           # damage
    send(on, 6)
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
    elif obj == pointer and op == 0:                         # enter: its own pointer
        serial, = struct.unpack("<I", body[:4])
        send(pointer, 0, struct.pack("<IIii", serial, cursor_surface, 3, 4))   # set_cursor
        draw(8, 8, cursor_surface)
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

XTEST(LubanOs, XdeskSwitchesArrangementsAndComesBackToTheSameLayout, .area = "luban",
      .covers = {"XDESK-COMPOSITOR", "XDESK-ARRANGEMENTS"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-arrangements");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto split = home.root() / "split.ppm", whole = home.root() / "whole.ppm", back = home.root() / "back.ppm";
    // Tiled: blue joins green's column (Super+[); stacked (Super+m): blue the
    // whole output (Super+f), then free again; on to the canvas and back to
    // tiling: the columns as they were.
    auto r = xdesk_with(home, "classic", std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 3000
key Alt+[
sleep 1000
screenshot {}
key Alt+m
sleep 500
key Alt+f
sleep 1000
screenshot {}
key Alt+f
sleep 500
key Alt+m
sleep 500
key Alt+m
sleep 1000
screenshot {}
screenshot {}
)", split.string(), whole.string(), back.string(), artifact("xdesk-arrangements-back.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // Two columns: red, then green above blue.
    const auto a = read_ppm(split);
    EXPECT_EQ(a.at(275, 400), kRed);
    EXPECT_EQ(a.at(825, 250), kGreen);
    EXPECT_EQ(a.at(825, 600), kBlue);
    EXPECT_EQ(read_ppm(whole).at(275, 400), kBlue) << "stacked, Super+f is the whole output";
    const auto c = read_ppm(back);
    EXPECT_EQ(c.at(275, 400), kRed);
    EXPECT_EQ(c.at(825, 250), kGreen);
    EXPECT_EQ(c.at(825, 600), kBlue);
}

XTEST(LubanOs, TheHostDrawsTheDesktopsPointerAsItSaysIt, .area = "luban",
      .covers = {"LUBAN-OS-POINTER", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-pointer");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto panel = home.root() / "panel.json", over = home.root() / "over.json", back = home.root() / "back.json";
    // Over xdesk's own panel: a shape by name; over a window that draws its
    // own pointer: that image; off the window again: the shape. The host
    // draws each (no frame in between) and nothing is drawn into the frames.
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
move 550 8
sleep 800
state {}
move 550 400
sleep 1000
state {}
move 550 8
sleep 800
state {}
)", panel.string(), over.string(), back.string()),
                        {"--desktop-session", "xdesk", "--desktop-exec", xdesk_binary()->string(), "--profile", "classic",
                         "--autostart", std::format("python3 {} c04040 --cursor 1", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    const auto a = read_state(panel)["pointer"], b = read_state(over)["pointer"], c = read_state(back)["pointer"];
    EXPECT_EQ(a["said"], "default") << a.dump();
    EXPECT_EQ(a["shape"], "default") << a.dump();
    EXPECT_FALSE(a["host_arrow"].get<bool>()) << "an xdesk that asks draws no arrow of the host's own";
    EXPECT_EQ(b["shape"], "custom") << b.dump();
    EXPECT_EQ(b["size"], Json::array({8, 8})) << b.dump();
    EXPECT_EQ(c["shape"], "default") << c.dump();
}

XTEST(LubanOs, AWindowThatDrawsNoPointerGetsXdesksInItsFrames, .area = "luban",
      .covers = {"LUBAN-OS-POINTER", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-soft-pointer");
    // A pointer theme of one shape: an 8x8 green square, its hotspot the corner.
    const auto icons = home.root() / "icons";
    fs::create_directories(icons / "default" / "cursors");
    std::string xcur = "Xcur";
    auto u32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) xcur.push_back(static_cast<char>(v >> (8 * k) & 0xff)); };
    for (const std::uint32_t v : {16u, 0x10000u, 1u, 0xfffd0002u, 24u, 28u, 36u, 0xfffd0002u, 24u, 1u, 8u, 8u, 0u, 0u, 0u}) u32(v);
    for (int k = 0; k < 64; ++k) u32(0xff00ff00u);
    tk::write_file(icons / "default" / "cursors" / "default", xcur);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    // A window that does not draw pointers (an older Luban OS): xdesk draws
    // it into the frames, where the pointer is.
    const auto shot = home.root() / "soft.ppm";
    tk::RunOptions o;
    o.argv = {"python3", (fs::path(__FILE__).parent_path() / "luban_desktop_client.py").string(), "--exec", "{move:200,150}",
              shot.string(), "--", xdesk_binary()->string(), "--profile", "classic"};
    o.env = home.env();
    o.env["XDG_RUNTIME_DIR"] = runtime.string();
    o.env["XCURSOR_PATH"] = icons.string();
    const auto r = tk::run(o);
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(read_ppm(shot).at(203, 153), (std::array<int, 3>{0x00, 0xff, 0x00})) << "the pointer, drawn where it is";
}

// Super+m held: the arrangements as cards, the next one lit; m again for the
// one after; Super let go chooses it -- and the session after starts as it
// was left (design 2026-10-19 §17.5, §17.12). A state.json that cannot be read
// is said and never written over.
XTEST(LubanOs, XdeskChoosesArrangementsHeldWithSuperAndKeepsThem, .area = "luban",
      .covers = {"XDESK-ARRANGEMENTS", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-chooser");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    auto session = [&](std::string_view script) {
        return run_script(home, script,
                          {"--desktop-exec", xdesk_binary()->string(), "--profile", "xdesk", "--autostart",
                           std::format("python3 {} c04040", client)},
                          {{"XDG_RUNTIME_DIR", runtime.string()}});
    };
    const auto state = home.root() / ".config" / "xdesk" / "state.json";
    auto arrangement = [&] { return Json::parse(tk::read_file(state))["arrangements"][0].get<std::string>(); };
    const auto held = home.root() / "held.ppm";
    auto r = session(std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
keydown Alt
key m
sleep 800
screenshot {}
screenshot {}
key m
keyup Alt
sleep 800
)", held.string(), artifact("xdesk-chooser.bmp").string()));
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(read_ppm(held).at(550, 380), kRed) << "the chooser over the desktop while Super is held";
    EXPECT_EQ(arrangement(), "canvas") << "tile, then stack, then canvas: chosen on letting Super go";
    EXPECT_EQ(Json::parse(tk::read_file(state))["last"], "canvas");
    // The next session starts as it was left: the next one from canvas is tile.
    r = session(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 1500
key Alt+m
sleep 800
)");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(arrangement(), "tile");
    // Unreadable: started as new, and the file is the person's still.
    tk::write_file(state, "{\"arrangements\": [");
    r = session(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 1500
key Alt+m
sleep 800
)");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(tk::read_file(state), "{\"arrangements\": [");
}

// A window's title bar, the other button: its menu (design 2026-10-19 §18.1).
XTEST(LubanOs, XdeskTitleBarMenuActsOnItsWindow, .area = "luban",
      .covers = {"XDESK-WINDOW-MENU", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-menu");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto menu = home.root() / "menu.ppm", closed = home.root() / "closed.ppm";
    // One window, its title bar under the panel; the menu's last item closes it
    // (4 items, a line, 4 workspaces, a line, Close: rows of 30, lines of 9).
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
click 300 50 right
sleep 600
screenshot {}
screenshot {}
click 420 331
sleep 1200
screenshot {}
)", menu.string(), artifact("xdesk-window-menu.bmp").string(), closed.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                         std::format("python3 {} c04040", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(read_ppm(menu).at(420, 120), kRed) << "the menu where the button was pressed";
    EXPECT_NE(read_ppm(closed).at(550, 400), kRed) << "Close closed it";

    // An application's rule (kept in state.json): it opens on workspace 2.
    tk::write_file(home.root() / ".config" / "xdesk" / "state.json",
                   R"({"rules": {"luban-test-red": {"workspace": 2}}, "hinted": true})");
    const auto one = home.root() / "one.ppm", two = home.root() / "two.ppm";
    r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
screenshot {}
key Alt+2
sleep 1000
screenshot {}
)", one.string(), two.string()),
                   {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic", "--autostart",
                    std::format("python3 {} c04040 --app-id luban-test-red", client)},
                   {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(read_ppm(one).at(550, 400), kRed) << "not on the first workspace";
    EXPECT_EQ(read_ppm(two).at(550, 400), kRed) << "on the second, as its rule says";
}

// A window dragged by its title (design 2026-10-19 §17.9): a preview where it
// would land, and there it goes when let go -- red over the top of green is
// red above green in green's column.
XTEST(LubanOs, XdeskDraggedWindowLandsWhereItsPreviewWas, .area = "luban",
      .covers = {"XDESK-DRAG-SNAP", "XDESK-COMPOSITOR"}, .requires_ = {"linux", "xlings-bin"}) {
    if (!app_binary()) GTEST_SKIP() << "no luban-os beside the xlings under test";
    if (!xdesk_binary()) GTEST_SKIP() << "no xdesk beside luban-os (mcpp build -p xdesk-app)";
    auto home = tk::Home::isolated("luban-os-xdesk-drag");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto runtime = home.root() / "run";
    fs::create_directories(runtime);
    fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
    const auto client = colour_client(home).string();
    const auto during = home.root() / "during.ppm", after = home.root() / "after.ppm";
    auto r = run_script(home, std::format(R"(wait desktop
waitfor desktop.texture.0 1100
sleep 2500
press 200 50
move 400 120
move 820 150
sleep 600
screenshot {}
screenshot {}
release 820 150
sleep 1200
screenshot {}
)", during.string(), artifact("xdesk-drag-preview.bmp").string(), after.string()),
                        {"--desktop-exec", xdesk_binary()->string(), "--profile", "classic",
                         "--autostart", std::format("python3 {} c04040", client),
                         "--autostart", std::format("sh -c 'sleep 0.6; exec python3 {} 40c040'", client)},
                        {{"XDG_RUNTIME_DIR", runtime.string()}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // While dragged: green's top half shows where red would go (the accent over green).
    EXPECT_NE(read_ppm(during).at(1000, 250), kGreen) << "a preview over the top of green";
    // Let go: one column, red above green.
    const auto a = read_ppm(after);
    EXPECT_EQ(a.at(550, 200), kRed);
    EXPECT_EQ(a.at(550, 600), kGreen);
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
