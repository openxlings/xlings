// xdesk under the Luban OS window, for the tests that drive it (design
// 2026-10-19 §17): the binary, a client in one colour, a session of three.
#pragma once

namespace luban_os_e2e {

// xdesk (luban/xdesk/app), built beside the app; XDESK_BIN names another.
inline std::optional<fs::path> xdesk_binary() {
    if (const char* forced = std::getenv("XDESK_BIN"); forced && *forced) return fs::path(forced);
    const auto app = app_binary();
    if (!app) return std::nullopt;
    for (const auto& c : {app->parent_path() / "xdesk", app->parent_path().parent_path() / "xdesk-app" / "xdesk"})
        if (fs::is_regular_file(c)) return c;
    return std::nullopt;
}


// A Wayland client in one colour (xdg-shell over the plain wire, its pixels
// in shared memory), so where xdesk puts a window is read off the picture;
// with --log it also takes text (text-input-v3) and the clipboard, and says
// what it got.
inline fs::path colour_client(const tk::Home& home) {
    const auto file = home.root() / "client.py";
    tk::write_file(file, R"PY(# A Wayland client in one colour: xdg-shell over the plain wire, its pixels
# in shared memory. Closes when asked. argv: RRGGBB [--log FILE] [--copy TEXT] [--cursor 1]
#   --log   text-input-v3 on: what is committed, and the selection it is
#           offered, written to FILE ("commit <text>", "paste <text>"); and
#           the pointer's presses ("button <code> at <x> <y>", its coordinates)
#   --copy  then its own selection: TEXT
#   --cursor  its own pointer over it: 8x8 in its colour, hotspot (3, 4)
#   --app-id  its application id
#   --cells WxH  draws to whole cells of WxH pixels unless told it is tiled,
#           maximized or fullscreen -- as a terminal does (foot)
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
cells = tuple(int(v) for v in opts["--cells"].split("x")) if "--cells" in opts else None
compositor, shm, wm = bind("wl_compositor", 4), bind("wl_shm", 1), bind("xdg_wm_base", 2 if cells else 1)
surface = new(); send(compositor, 0, struct.pack("<I", surface))
xdg_surface = new(); send(wm, 2, struct.pack("<II", xdg_surface, surface))
toplevel = new(); send(xdg_surface, 1, struct.pack("<I", toplevel))
send(toplevel, 2, string(sys.argv[1]))                      # set_title
if "--app-id" in opts: send(toplevel, 3, string(opts["--app-id"]))   # set_app_id
keyboard = text = device = source = pointer = cursor_surface = None
seat = bind("wl_seat", 5) if log or cursor else None
if cursor or log:
    pointer = new(); send(seat, 0, struct.pack("<I", pointer))              # get_pointer
if cursor:
    cursor_surface = new(); send(compositor, 0, struct.pack("<I", cursor_surface))
if log:
    keyboard = new(); send(seat, 1, struct.pack("<I", keyboard))            # get_keyboard
    inputs = bind("zwp_text_input_manager_v3", 1)                         # ids in the order they are sent
    text = new(); send(inputs, 1, struct.pack("<II", text, seat))
    manager = bind("wl_data_device_manager", 3)
    device = new(); send(manager, 1, struct.pack("<II", device, seat))     # get_data_device
send(surface, 6)                                             # commit
size = (0, 0); drawn = None; offers = {}; pasted = False; enter_serial = 0; last_motion = (0, 0)
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
        n, = struct.unpack("<I", body[8:12])
        states = struct.unpack("<%dI" % (n // 4), body[12:12 + n])
        if cells and not any(st in (1, 2, 5, 6, 7, 8) for st in states):
            size = (max(cells[0], size[0] - size[0] % cells[0]), max(cells[1], size[1] - size[1] % cells[1]))
    elif obj == toplevel and op == 1:
        break                                                # close
    elif obj == xdg_surface and op == 0:
        send(xdg_surface, 4, body[:4])                       # ack_configure
        if size != drawn: draw(*size); drawn = size
    elif obj == wm and op == 0:
        send(wm, 3, body[:4])                                # pong
    elif obj == pointer and op == 0 and cursor:              # enter: its own pointer
        serial, = struct.unpack("<I", body[:4])
        send(pointer, 0, struct.pack("<IIii", serial, cursor_surface, 3, 4))   # set_cursor
        draw(8, 8, cursor_surface)
    elif obj == pointer and op == 0 and log:                 # enter: where, in its own coordinates
        x, y = struct.unpack("<ii", body[8:16])
        last_motion = (x / 256.0, y / 256.0)
    elif obj == pointer and op == 2 and log:                 # motion: where, in its own coordinates
        x, y = struct.unpack("<ii", body[4:12])
        last_motion = (x / 256.0, y / 256.0)
    elif obj == pointer and op == 3 and log:                 # button
        _, _, b, state = struct.unpack("<IIII", body[:16])
        if state == 1: log.write("button %d at %.0f %.0f\n" % (b, *last_motion))
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

inline tk::RunResult xdesk_with(const tk::Home& home, std::string_view profile, std::string_view script) {
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

inline constexpr std::array<int, 3> kRed { 0xc0, 0x40, 0x40 }, kGreen { 0x40, 0xc0, 0x40 }, kBlue { 0x40, 0x40, 0xc0 };

// A window's colour as drawn: within a few steps of it per channel (a fade
// that has not quite ended on a slow machine darkened 192 to 189).
inline bool near_colour(const std::array<int, 3>& got, const std::array<int, 3>& want, int steps = 6) {
    for (int i = 0; i < 3; ++i)
        if (got[i] < want[i] - steps || got[i] > want[i] + steps) return false;
    return true;
}

// Where a colour is on a picture: its bounding box (x0, y0, x1, y1), -1s for none.
inline std::array<int, 4> box_of(const Image& img, const std::array<int, 3>& colour) {
    std::array<int, 4> b{-1, -1, -1, -1};
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
            if (img.at(x, y) == colour) {
                if (b[0] < 0 || x < b[0]) b[0] = x;
                if (b[1] < 0 || y < b[1]) b[1] = y;
                b[2] = std::max(b[2], x);
                b[3] = std::max(b[3], y);
            }
    return b;
}


}  // namespace luban_os_e2e
