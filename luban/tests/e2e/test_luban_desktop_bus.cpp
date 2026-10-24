// A desktop with no window (design 2026-10-24 §4, §5): its supervisor starts
// it on a virtual display, everything about it is a path on the bus -- the
// display (described, captured, given input), the desktop's own state, the
// session (status, displays, end) -- and the desktop's programs are placed
// under the system's path. Here the desktop is xdesk as built, run in this
// host's environment (`--here`): what the bus and the displays do, without a
// system to install.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.platform;
import luban.vm;
import xbus.address;
import xbus.node;
import openhal.display;
import openhal.emu;
import openbus;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

std::optional<fs::path> luban_bin_() {
    if (const char* forced = std::getenv("LUBAN_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& candidate : {xlings.parent_path() / "luban", xlings.parent_path() / "luban-tool" / "luban"})
        if (fs::is_regular_file(candidate)) return candidate;
    return std::nullopt;
}

std::optional<fs::path> xdesk_bin_() {
    if (const char* forced = std::getenv("XDESK_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& candidate : {xlings.parent_path() / "xdesk", xlings.parent_path() / "xdesk-app" / "xdesk"})
        if (fs::is_regular_file(candidate)) return candidate;
    return std::nullopt;
}

// A Wayland client in one colour, as the xdesk tests use (xdg-shell over the
// plain wire, its pixels in shared memory).
constexpr std::string_view kClient = R"PY(import os, socket, struct, sys, mmap, tempfile
colour = int(sys.argv[1], 16)
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.path.join(os.environ["XDG_RUNTIME_DIR"], os.environ["WAYLAND_DISPLAY"]))
nid = [2]
def new():
    i = nid[0]; nid[0] += 1; return i
def string(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)
def send(obj, op, args=b"", fds=()):
    msg = struct.pack("<II", obj, ((8 + len(args)) << 16) | op) + args
    if fds: sock.sendmsg([msg], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("<%di" % len(fds), *fds))])
    else: sock.sendall(msg)
buf = b""
def events():
    global buf
    while True:
        while len(buf) >= 8:
            obj, word = struct.unpack("<II", buf[:8]); size, op = word >> 16, word & 0xffff
            if len(buf) < size: break
            body, buf = buf[8:size], buf[size:]
            yield obj, op, body
        data = sock.recv(65536)
        if not data: return
        buf += data
it = events()
registry = new(); send(1, 1, struct.pack("<I", registry))
sync = new(); send(1, 0, struct.pack("<I", sync))
g = {}
for obj, op, body in it:
    if obj == registry and op == 0:
        name, = struct.unpack("<I", body[:4]); n, = struct.unpack("<I", body[4:8])
        iface = body[8:8 + n - 1].decode(); ver, = struct.unpack("<I", body[8 + n + (-n % 4):][:4]); g[iface] = (name, ver)
    elif obj == sync: break
def bind(iface, ver):
    i = new(); name, _ = g[iface]
    send(registry, 0, struct.pack("<I", name) + string(iface) + struct.pack("<II", ver, i)); return i
comp = bind("wl_compositor", 4); shm = bind("wl_shm", 1); wm = bind("xdg_wm_base", 1)
surf = new(); send(comp, 0, struct.pack("<I", surf))
xs = new(); send(wm, 2, struct.pack("<II", xs, surf))
top = new(); send(xs, 1, struct.pack("<I", top))
send(top, 2, string("colour"))
send(surf, 6)
w, h = 400, 300
for obj, op, body in it:
    if obj == top and op == 0:
        cw, ch = struct.unpack("<ii", body[:8])
        if cw > 0 and ch > 0: w, h = cw, ch
    elif obj == xs and op == 0:
        serial, = struct.unpack("<I", body[:4]); send(xs, 4, struct.pack("<I", serial))
        fd, path = tempfile.mkstemp(); os.unlink(path); size = w * h * 4; os.ftruncate(fd, size)
        m = mmap.mmap(fd, size); m.write(struct.pack("<I", 0xff000000 | colour) * (w * h))
        pool = new(); send(shm, 0, struct.pack("<Ii", pool, size), [fd])
        b = new(); send(pool, 0, struct.pack("<IiiiiI", b, 0, w, h, w * 4, 1))
        send(surf, 1, struct.pack("<Iii", b, 0, 0)); send(surf, 2, struct.pack("<iiii", 0, 0, w, h)); send(surf, 6)
    elif obj == top and op == 1: break
)PY";

}  // namespace

XTEST(LubanDesktopBus, ADesktopRunsOnAVirtualDisplayAndEverythingOfItIsOnTheBus, .area = "luban",
      .covers = {"LUBAN-DISPLAY-VIRTUAL", "LUBAN-BUS-DESKTOP", "LUBAN-DISPLAY-HOTPLUG", "LUBAN-DESKTOP-APP-LOGS", "LUBAN-DESKTOP-SESSION-END"}, .requires_ = {"linux", "xlings-bin"}) {
    const auto luban = luban_bin_();
    const auto xdesk = xdesk_bin_();
    if (!luban) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    if (!xdesk) GTEST_SKIP() << "no xdesk beside the xlings under test (mcpp build -p xdesk-app, or XDESK_BIN)";
    auto home = tk::Home::isolated("luban-desktop-bus");
    const auto root = home.root();
    // A session the desktop runs in this host's environment: xdesk with one window.
    tk::write_file(root / "client.py", std::string(kClient));
    fs::create_directories(root / "share" / "wayland-sessions");
    tk::write_file(root / "share" / "wayland-sessions" / "xdesk.desktop",
                   std::format("[Desktop Entry]\nName=xdesk\nExec={} --profile classic --autostart 'python3 {} c04040'"
                               " --autostart 'sh -c \"echo hello-from-an-app; sleep 3; kill -SEGV $$\"'\n"
                               "X-Opensal-Native=true\nX-Opensal-Interfaces=surface,keyboard,pointer\n",
                               xdesk->string(), (root / "client.py").string()));
    // State it as already toured: no first-session tour over the window.
    fs::create_directories(root / ".config" / "xdesk");
    tk::write_file(root / ".config" / "xdesk" / "state.json", R"({"toured": true})");
    fs::create_directories(root / "runtime");
    fs::permissions(root / "runtime", fs::perms::owner_all, fs::perm_options::replace);
    auto env = home.env();
    env["LUBAN_BIN"] = luban->string();
    env["LUBAN_XLINGS"] = tk::xlings_binary().string();
    env["XDG_DATA_DIRS"] = (root / "share").string();
    env["XDG_RUNTIME_DIR"] = (root / "runtime").string();
    for (const auto& [k, v] : env) xlings::platform::set_env_variable(k, v);
    const fs::path h = home.dir();

    tk::RunOptions o{.argv = {luban->string(), "__desktop-session", h.string(), "box", "--here", "--mode", "800x600", "--session", "xdesk"},
                     .env = env, .timeout = 120s};
    auto supervisor = std::async(std::launch::async, [o] { return tk::run(o); });
    struct End {
        fs::path h;
        ~End() { (void)luban::vm::desktop(h, "box", {{"op", "end"}}); }
    } end{h};
    std::expected<Json, std::string> st = std::unexpected(std::string("not yet"));
    for (int k = 0; k < 100 && !st; ++k) {
        std::this_thread::sleep_for(200ms);
        st = luban::vm::desktop(h, "box", {{"op", "status"}});
    }
    ASSERT_TRUE(st) << st.error();
    EXPECT_TRUE((*st)["running"].get<bool>());
    EXPECT_EQ((*st)["mode"], "800x600");
    const auto display = (*st)["virtual"].get<std::string>();
    ASSERT_EQ((*st)["displays"].size(), 1u) << st->dump();
    EXPECT_EQ((*st)["displays"][0]["kind"], "virtual");

    auto node = xbus::Node::connect({.name = "test", .router = xbus::Address{"unix", luban::vm::bus_socket(h).string()}, .telemetry = false});
    ASSERT_TRUE(node) << node.error();
    const auto control = openhal::display::control_path(display);
    // The desktop drew: frames shown on the virtual display; xdesk's own state
    // is under the system's path, its window there, the display its primary.
    const auto xdesk_state = luban::vm::system_bus_root("box") + "/service/xdesk/state";
    Json state;
    for (int k = 0; k < 150; ++k) {
        std::this_thread::sleep_for(200ms);
        auto got = (*node)->call(xdesk_state, {}, 5s);
        if (!got || got->empty()) continue;
        state = Json::parse(got->front(), nullptr, false);
        if (state.is_object() && state.value("windows", Json::array()).size() == 1) break;
    }
    ASSERT_TRUE(state.is_object()) << "xdesk is not on the bus at " << xdesk_state;
    EXPECT_EQ(state["windows"].size(), 1u) << state.dump();
    EXPECT_EQ(state["output"]["size"], Json::array({800, 600})) << state["output"].dump();
    ASSERT_EQ(state["displays"].size(), 1u);
    EXPECT_EQ(state["displays"][0]["role"], "primary");
    auto described = (*node)->call(control, R"({"op": "describe"})", 5s);
    ASSERT_TRUE(described && !described->empty());
    const auto d = Json::parse(described->front());
    EXPECT_GT(d["frames"].get<double>(), 0) << d.dump();
    EXPECT_EQ(d["mode"], "800x600");
    // A picture of it: a PNG of the display's size, with the window's colour in it.
    auto png = (*node)->call(control, R"({"op": "capture"})", 20s);
    ASSERT_TRUE(png && !png->empty());
    std::string bytes;
    for (const auto& p : *png) bytes += p;
    ASSERT_TRUE(bytes.starts_with("\x89PNG\r\n\x1a\n"));
    tk::write_file(tk::artifacts_dir() / "desktop-bus-virtual.png", bytes);
    const std::string red{"\xc0\x40\x40", 3};
    EXPECT_NE(bytes.find(red + red), std::string::npos) << "the window's colour is in the picture";
    // Input through the display (an agent's way, the same as a window's):
    // Super alone opens the launcher.
    auto given = (*node)->call(control, R"({"op": "input", "events": [{"move": [400, 300]}, {"key": "Super"}]})", 5s);
    ASSERT_TRUE(given) << given.error().message;
    bool open = false;
    for (int k = 0; k < 50 && !open; ++k) {
        std::this_thread::sleep_for(100ms);
        auto got = (*node)->call(xdesk_state, {}, 5s);
        if (!got || got->empty()) continue;
        const auto now = Json::parse(got->front(), nullptr, false);
        if (now.is_object()) open = now["overlays"].value("launcher", false);
    }
    EXPECT_TRUE(open) << "the key reached the desktop: its launcher is open";
    // A window comes (here: a display of this test's, asking to be the
    // primary, as the Luban OS window does): the desktop takes its size; it
    // goes, and the desktop goes on on its virtual display (hotplug, §4.3).
    auto window = std::make_shared<openhal::emu::VirtualDisplay>(openhal::display::Mode{1024, 640, 1.0}, true);
    const auto window_path = openhal::display::device_path((*node)->root(), 50);
    std::optional<xbus::Stream> window_stream;
    std::mutex wm;
    ASSERT_TRUE((*node)->offer_stream(window_path, {.kind = openbus::EndpointKind::stream, .interfaces = {std::string(openhal::display::kInterface)}},
                                      [&](xbus::Stream st) {
                                          { std::lock_guard l(wm); window_stream = st; }
                                          std::thread([window, st]() mutable {
                                              while (auto chunk = st.read(5s)) {
                                                  const auto back = window->feed(chunk->bytes);
                                                  if (!back.empty() && !st.write(back)) break;
                                              }
                                          }).detach();
                                      }));
    auto attached = luban::vm::desktop(h, "box", {{"op", "attach"}, {"path", window_path}, {"role", "primary"}, {"kind", "window"}});
    ASSERT_TRUE(attached) << attached.error();
    const auto output_of = [&](Json& now) {
        auto got = (*node)->call(xdesk_state, {}, 5s);
        if (!got || got->empty()) return false;
        now = Json::parse(got->front(), nullptr, false);
        return now.is_object();
    };
    Json now;
    bool primary = false;
    for (int k = 0; k < 100 && !primary; ++k) {
        std::this_thread::sleep_for(100ms);
        primary = output_of(now) && now["displays"].size() == 2 && now["output"]["size"] == Json::array({1024, 640});
    }
    EXPECT_TRUE(primary) << "the window is the primary, the desktop its size\n" << now["displays"].dump() << now["output"].dump();
    for (int k = 0; k < 50 && window->frames() == 0; ++k) std::this_thread::sleep_for(100ms);
    EXPECT_GT(window->frames(), 0u) << "the window is drawn on";
    EXPECT_EQ(luban::vm::desktop(h, "box", {{"op", "status"}})->value("displays", Json::array()).size(), 2u);
    // It goes.
    { std::lock_guard l(wm); if (window_stream) window_stream->close(); }
    bool back = false;
    for (int k = 0; k < 100 && !back; ++k) {
        std::this_thread::sleep_for(100ms);
        back = output_of(now) && now["displays"].size() == 1 && now["output"]["size"] == Json::array({800, 600});
    }
    EXPECT_TRUE(back) << "the desktop goes on on its virtual display\n" << now["displays"].dump() << now["output"].dump();
    EXPECT_TRUE(luban::vm::desktop(h, "box", {{"op", "status"}})->value("running", false));
    // What a program it started said is kept, and its end: the crash is on
    // the bus with its signal and its log (design 2026-10-24 §6.1, J71).
    const auto apps_path = luban::vm::system_bus_root("box") + "/service/xdesk/apps";
    Json crashed;
    for (int k = 0; k < 100 && crashed.is_null(); ++k) {
        std::this_thread::sleep_for(100ms);
        auto got = (*node)->call(apps_path, {}, 5s);
        if (!got || got->empty()) continue;
        for (const auto& a : Json::parse(got->front(), nullptr, false))
            if (a.value("id", std::string{}) == "sh" && !a.value("running", true)) crashed = a;
    }
    ASSERT_FALSE(crashed.is_null()) << "the program that crashed is listed";
    EXPECT_EQ(crashed["code"], 128 + 11) << crashed.dump();
    EXPECT_NE(tk::read_file(crashed["log"].get<std::string>()).find("hello-from-an-app"), std::string::npos) << crashed.dump();
    // Gathered without copying it out by hand: luban logs.
    {
        auto told = tk::run({.argv = {luban->string(), "logs", "box", "--app", "sh"}, .env = env, .timeout = 60s});
        EXPECT_EQ(told.exit_code, 0) << told.transcript();
        EXPECT_NE(told.out.find("hello-from-an-app"), std::string::npos) << told.transcript();
        const auto bundle = root / "box-logs.tar";
        auto bundled = tk::run({.argv = {luban->string(), "logs", "box", "--bundle", bundle.string(), "--with-screenshot"}, .env = env, .timeout = 60s});
        EXPECT_EQ(bundled.exit_code, 0) << bundled.transcript();
        const auto tar = tk::read_file(bundle);
        for (const auto* member : {"status.json", "desktop/session.log", "screenshot.png"})
            EXPECT_NE(tar.find(member), std::string::npos) << member << "\n" << bundled.transcript();
        EXPECT_NE(tar.find("hello-from-an-app"), std::string::npos);
    }
    // Ended through its session: it says why.
    std::string events;
    auto sub = (*node)->subscribe(luban::vm::system_bus_root("box") + "/topic/desktop/events", [&](const xbus::Message& m) { events += m.body + "\n"; });
    (void)sub;
    auto ended = luban::vm::desktop(h, "box", {{"op", "end"}, {"by", "the test"}});
    ASSERT_TRUE(ended) << ended.error();
    const auto r = supervisor.get();
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_NE(events.find("\"ended\""), std::string::npos) << events;
    EXPECT_NE(events.find("the test"), std::string::npos) << events;
    EXPECT_FALSE(luban::vm::desktop(h, "box", {{"op", "status"}})) << "gone with it";
}
