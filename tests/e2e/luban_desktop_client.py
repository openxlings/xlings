#!/usr/bin/env python3
"""A running machine's desktop, driven the way the Luban OS window drives it:
the machine's client socket (luban.vm.protocol: [type][channel][length]
frames), a channel opened with "display", and opensal.wire inside its data
frames -- this side the provider (the size, the input), the guest's
opensal-wlr the consumer (the pixels).

    luban_desktop_client.py <run dir of the machine> <keys> <out.ppm>
    luban_desktop_client.py --exec <keys> <out.ppm> -- <command...>

<keys> is typed once the desktop has drawn: characters, \\n for Return,
{Alt+Return}-style chords. With --exec the desktop is a command whose standard
input and output are opensal.wire (a sandbox's `luban __desktop`), as the
window runs it. Prints what it saw as one JSON line; exits 1 when the desktop
never drew."""
import json
import os
import socket
import struct
import subprocess
import sys
import time

EXEC = sys.argv[1] == "--exec"
RUN, KEYS, OUT = (None, sys.argv[2], sys.argv[3]) if EXEC else (sys.argv[1], sys.argv[2], sys.argv[3])
CHANNEL = 2
W, H = 1024, 640

if EXEC:
    proc = subprocess.Popen(sys.argv[sys.argv.index("--") + 1:], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    os.set_blocking(proc.stdout.fileno(), False)
else:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(RUN + "/luban.sock")
    s.setblocking(False)


def frame(kind, payload=b""):
    payload = payload.encode() if isinstance(payload, str) else payload
    if EXEC:
        if kind == "D":
            proc.stdin.write(payload)
            proc.stdin.flush()
        elif kind == "C":
            proc.stdin.close()
        return
    s.sendall(kind.encode() + struct.pack(">II", CHANNEL, len(payload)) + payload)


def wire(kind, payload):
    payload = payload.encode() if isinstance(payload, str) else payload
    frame("D", kind.encode() + struct.pack(">I", len(payload)) + payload)


def receive():
    """The machine's frames, or (with --exec) the command's output as one data frame; b"" at the end."""
    if EXEC:
        d = proc.stdout.read(1 << 20)
        if d is None:
            raise BlockingIOError
        return b"D" + struct.pack(">II", CHANNEL, len(d)) + d if d else b""
    return s.recv(1 << 20)


if not EXEC:
    frame("O", json.dumps({"display": {"session": "", "width": W, "height": H}}))
vm_buf, wire_buf = b"", b""
seen = {"h": 0, "s": 0, "f": 0}
size, fb, ended = [0, 0], bytearray(), None


def pump(seconds):
    global vm_buf, wire_buf, ended
    end = time.time() + seconds
    while time.time() < end and ended is None:
        try:
            d = receive()
            if not d:
                ended = "the desktop's stream ended"
                return
            vm_buf += d
        except BlockingIOError:
            time.sleep(0.02)
            continue
        while len(vm_buf) >= 9:
            kind, ch, n = chr(vm_buf[0]), *struct.unpack(">II", vm_buf[1:9])
            if len(vm_buf) < 9 + n:
                break
            body, vm_buf = vm_buf[9:9 + n], vm_buf[9 + n:]
            if ch != CHANNEL:
                continue
            if kind == "X":
                ended = body.decode(errors="replace")
                return
            if kind != "D":
                continue
            wire_buf += body
            while len(wire_buf) >= 5:
                n2 = struct.unpack(">I", wire_buf[1:5])[0]
                if len(wire_buf) < 5 + n2:
                    break
                k, pl, wire_buf = chr(wire_buf[0]), wire_buf[5:5 + n2], wire_buf[5 + n2:]
                seen[k] = seen.get(k, 0) + 1
                if k == "h":
                    wire("H", "%d %d %d 1" % (1 | 2 | 4 | 8, W, H))
                elif k == "s":
                    size[:] = map(int, pl.decode().split())
                    fb[:] = bytes(size[0] * size[1] * 4)
                elif k == "f":
                    nl = pl.index(b"\n")
                    x, y, w, h, stride = map(int, pl[:nl].split())
                    rows = pl[nl + 1:]
                    for r in range(h):
                        o = ((y + r) * size[0] + x) * 4
                        fb[o:o + w * 4] = rows[r * stride:r * stride + w * 4]


def key(sym, mods=0, down=None):
    for d in ((1, 0) if down is None else (down,)):
        wire("E", "key %d %d %d 0" % (d, sym, mods))


def type_keys(text):
    names = {"Return": 0xff0d, "Tab": 0xff09, "Escape": 0xff1b}
    mods_of = {"Alt": (4, 0xffe9), "Ctrl": (2, 0xffe3), "Shift": (1, 0xffe1), "Super": (8, 0xffeb)}
    i = 0
    while i < len(text):
        if text[i] == "{":
            j = text.index("}", i)
            *mods, name = text[i + 1:j].split("+")
            bits = sum(mods_of[m][0] for m in mods)
            for m in mods:
                key(mods_of[m][1], bits, 1)
            key(names.get(name, ord(name[0]) if len(name) == 1 else 0), bits)
            for m in mods:
                key(mods_of[m][1], 0, 0)
            pump(1.5)
            i = j + 1
        elif text.startswith("\\n", i):
            key(0xff0d)
            i += 2
        else:
            c = text[i]
            key(ord(c), 1 if c.isupper() else 0)
            i += 1


# The desktop has drawn once it has sent a frame and gone quiet a moment.
deadline = time.time() + 120
while time.time() < deadline and seen["f"] == 0 and ended is None:
    pump(0.5)
pump(3)
drew = seen["f"] > 0
if drew:
    type_keys(KEYS)
    pump(3)
    with open(OUT, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (size[0], size[1]))
        f.write(bytes(b for i in range(0, len(fb), 4) for b in (fb[i + 2], fb[i + 1], fb[i])))
frame("C")
if EXEC:
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
print(json.dumps({"drew": drew, "size": size, "messages": seen, "ended": ended}))
sys.exit(0 if drew else 1)
