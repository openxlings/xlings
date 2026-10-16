#!/usr/bin/env python3
"""A program on a pseudo-terminal, typed into as a person types, its every
byte kept -- the host matrix's terminal driver (design 2026-10-16 L §12.4).

    term.py --out FILE [--cols 120 --rows 40] [--wait 90]
           [--step "<regex> ::: <text>"]... [--expect REGEX]... [--reject REGEX]...
           -- argv...

Each step waits until what the terminal received since the previous step
matches <regex>, then types <text> (\\r, \\n, \\x03, \\x1b escapes honoured).
The run passes when every --expect matches what was received and no
--reject does; it prints what failed and the last lines shown. Exit 0 pass,
1 fail, 2 a step whose output never came.
"""
import argparse, fcntl, os, pty, re, select, signal, struct, sys, termios, time

p = argparse.ArgumentParser()
p.add_argument("--out", required=True)
p.add_argument("--cols", type=int, default=120)
p.add_argument("--rows", type=int, default=40)
p.add_argument("--wait", type=float, default=90)
p.add_argument("--step", action="append", default=[])
p.add_argument("--expect", action="append", default=[])
p.add_argument("--reject", action="append", default=[])
p.add_argument("--tail", type=float, default=3)
p.add_argument("argv", nargs=argparse.REMAINDER)
a = p.parse_args()
argv = a.argv[1:] if a.argv and a.argv[0] == "--" else a.argv
if not argv:
    sys.exit("term.py: no program")

pid, fd = pty.fork()
if pid == 0:
    os.environ.setdefault("TERM", "xterm-256color")
    try:
        os.execvp(argv[0], argv)
    finally:
        os._exit(127)
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


def plain(b):
    return re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)|\x1b\[[0-9;?<=>]*[A-Za-z@`]|\x1b[()][0-9A-Za-z]|\x1b[=>78NOM]|\r", b"", b).decode("utf-8", "replace")


alive, missed = True, None
mark = 0
for step in a.step:
    pat, _, text = step.partition(" ::: ")
    text = text.encode().decode("unicode_escape").encode("latin-1")
    deadline = time.time() + a.wait
    found = False
    while time.time() < deadline:
        if re.search(pat, plain(bytes(buf[mark:]))):
            found = True
            break
        if not alive:
            break
        alive = pump(0.3)
    if not found:
        missed = pat
        break
    mark = len(buf)
    time.sleep(0.2)
    try:
        os.write(fd, text)
    except OSError:
        alive = False
    alive = pump(0.4) and alive
if alive:
    pump(a.tail)
try:
    os.killpg(os.getpgid(pid), signal.SIGKILL)
except (ProcessLookupError, PermissionError):
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
with open(a.out, "wb") as f:
    f.write(bytes(buf))
shown = plain(bytes(buf))
failures = []
if missed is not None:
    failures.append(f"the terminal never showed /{missed}/")
for e in a.expect:
    if not re.search(e, shown):
        failures.append(f"not shown: /{e}/")
for r in a.reject:
    m = re.search(r, shown)
    if m:
        failures.append(f"shown, and must not be: {m.group(0)!r}")
print("\n".join(shown.splitlines()[-30:]))
for f in failures:
    print(f"term.py: {f}")
sys.exit(2 if missed is not None else 1 if failures else 0)
