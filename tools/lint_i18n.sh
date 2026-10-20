#!/usr/bin/env bash
# A program's words are by key, in its catalogues (design 2026-10-19 §7):
# every key the Luban OS app and luban say is in its English catalogue, its Chinese
# one names no key English lacks and takes the same arguments, and no
# tr(en, zh) pair -- the words at the call -- comes back.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
import re, sys, pathlib
src = pathlib.Path("luban/host/luban-os/src")
entry = re.compile(r'\{\s*"([^"]+)",\s*"((?:[^"\\]|\\.)*)"\s*\}')
def table(f): return dict(entry.findall((src / "i18n" / f).read_text()))
en, zh = table("en.cpp"), table("zh.cpp")
bad = []
args = lambda t: len(re.findall(r'(?<!\{)\{[^{}]*\}', t))
used = set()
for f in src.rglob("*.cpp*"):
    if f.parent.name == "i18n": continue
    text = f.read_text()
    for m in re.finditer(r'(?<![A-Za-z0-9_:.])tr\(\s*"([^"]*)"\s*,\s*"', text):
        bad.append(f"{f}: tr(en, zh) -- the words belong in luban/host/luban-os/src/i18n by key")
    used |= set(re.findall(r'"(luban\.os\.[a-z0-9_.]+)"', text))
for k in sorted(used - en.keys()): bad.append(f"{k}: used, not in i18n/en.cpp")
for k in sorted(zh.keys() - en.keys()): bad.append(f"{k}: in i18n/zh.cpp, not in English")
for k in sorted(zh.keys() & en.keys()):
    if args(zh[k]) != args(en[k]): bad.append(f"{k}: Chinese takes {args(zh[k])} arguments, English {args(en[k])}")
for k, t in en.items():
    if not t: bad.append(f"{k}: empty in English")
# luban's own domain (luban/os: the CLI and the service): every key said is in
# its English catalogue (luban/os/service/src/i18n/words.cpp).
luban_en = dict(entry.findall(pathlib.Path("luban/os/service/src/i18n/words.cpp").read_text()))
said = set()
for f in pathlib.Path("luban/os").rglob("*.cpp*"):
    said |= set(re.findall(r'(?<![A-Za-z0-9_])trf?\(\s*"(luban\.(?!os\.)[a-z0-9_.]+)"', f.read_text()))
for k in sorted(said - luban_en.keys()): bad.append(f"{k}: said in luban/os, not in its words.cpp")
if bad:
    print("i18n lint: FAIL"); [print("  " + b) for b in bad]; sys.exit(1)
print(f"i18n lint: PASS (luban.os: {len(en)} words, {len(zh)} in Chinese; luban: {len(luban_en)} words)")
PY
