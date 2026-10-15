#!/usr/bin/env python3
"""Run a command on a pty, type a script into it, and record every byte.

    pty_run.py --out <file> [--cols 120 --rows 40] [--step "<wait-regex> ::: <text>"]... -- <argv...>

Each step waits (up to --wait seconds) until the output so far matches the
regex, then types the text (\\r and \\x1b escapes are honoured). Prints a
summary of terminal sequences seen. Never asserts.
"""
import argparse, fcntl, os, pty, re, select, struct, sys, termios, time

p = argparse.ArgumentParser()
p.add_argument("--out", required=True)
p.add_argument("--cols", type=int, default=120)
p.add_argument("--rows", type=int, default=40)
p.add_argument("--wait", type=float, default=60)
p.add_argument("--step", action="append", default=[])
p.add_argument("--tail", type=float, default=3)
p.add_argument("argv", nargs=argparse.REMAINDER)
a = p.parse_args()
argv = a.argv[1:] if a.argv and a.argv[0] == "--" else a.argv

pid, fd = pty.fork()
if pid == 0:
    os.execvp(argv[0], argv)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", a.rows, a.cols, 0, 0))
buf = bytearray()

def pump(timeout):
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                return False
            if not d:
                return False
            buf.extend(d)
    return True

alive = True
for step in a.step:
    pat, _, text = step.partition(" ::: ")
    text = text.encode().decode("unicode_escape").encode("latin-1")
    deadline = time.time() + a.wait
    start = len(buf)
    while alive and time.time() < deadline:
        if re.search(pat.encode(), bytes(buf[max(0, start - 2048):])):
            break
        alive = pump(0.3)
    time.sleep(0.3)
    if alive:
        os.write(fd, text)
        alive = pump(0.5)
if alive:
    pump(a.tail)
try:
    os.kill(pid, 9)
except ProcessLookupError:
    pass
open(a.out, "wb").write(bytes(buf))
b = bytes(buf)
seq = {
    "alt-screen enter \\e[?1049h": b"\x1b[?1049h",
    "alt-screen leave \\e[?1049l": b"\x1b[?1049l",
    "alt-screen enter \\e[?47h": b"\x1b[?47h",
    "alt-screen leave \\e[?47l": b"\x1b[?47l",
    "smcup \\e7\\e[?47h": b"\x1b7\x1b[?47h",
}
print(f"bytes: {len(b)}")
for k, v in seq.items():
    print(f"  {k}: {'yes' if v in b else 'no'}")
for w in [b"can't access tty", b"no job control", b"tcgetpgrp", b"Inappropriate ioctl",
          b"cannot set terminal process group", b"Not a tty"]:
    if w in b:
        print(f"  warning text seen: {w.decode()}")
text = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][0-9A-Za-z]|\x1b[=>78]|\r", b"", b).decode("utf-8", "replace")
print("---- text (last 40 lines) ----")
print("\n".join(text.splitlines()[-40:]))
