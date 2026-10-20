#!/usr/bin/env python3
"""The Luban OS window's key frames on one page, to look at.

The headless tests (luban/tests/e2e/test_luban_os.cpp) check what can be checked --
every view reached by keys and pointer, every control on screen, apart and big
enough at three sizes in two languages, a desktop drawn and given its input --
and keep a screenshot of each key frame. What they cannot judge (proportion,
hierarchy, density, whether it looks finished) is judged by a person, here:

    mcpp test test_luban_os
    python3 luban/tools/luban_os_keyframes.py [dir]     # default target/xtest-artifacts/luban-os

writes <dir>/index.html: the frames grouped by view, each size and language
side by side. CI keeps the same directory as the `luban-os-screenshots`
artifact of every Linux run.
"""
import html
import re
import sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else "target/xtest-artifacts/luban-os")
frames = sorted(root.glob("*.bmp"))
if not frames:
    sys.exit(f"no frames in {root}: run `mcpp test test_luban_os` first")

groups: dict[str, list[Path]] = {}
for f in frames:
    m = re.match(r"(\d+x\d+)-(en|zh)-(.+)\.bmp$", f.name)
    groups.setdefault(m.group(3) if m else "flows", []).append(f)

def size_key(f: Path):
    m = re.match(r"(\d+)x(\d+)-(en|zh)", f.name)
    return (int(m.group(1)), m.group(3)) if m else (0, f.name)

parts = ["<!doctype html><meta charset=utf-8><title>Luban OS key frames</title>",
         "<style>body{background:#0d0e11;color:#cfd3da;font:14px sans-serif;margin:24px}"
         "h2{font-weight:500;margin:28px 0 10px}figure{display:inline-block;margin:0 14px 14px 0;vertical-align:top}"
         "img{border:1px solid #2a2d33;max-width:560px;display:block}figcaption{color:#8a909a;font-size:12px;margin-top:4px}</style>",
         "<h1>Luban OS key frames</h1>"]
for view in sorted(groups):
    parts.append(f"<h2>{html.escape(view)}</h2>")
    for f in sorted(groups[view], key=size_key):
        parts.append(f'<figure><img src="{html.escape(f.name)}" loading=lazy><figcaption>{html.escape(f.stem)}</figcaption></figure>')
(root / "index.html").write_text("\n".join(parts) + "\n", encoding="utf-8")
print(f"{root / 'index.html'}: {len(frames)} frames in {len(groups)} groups")
