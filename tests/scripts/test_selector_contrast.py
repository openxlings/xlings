#!/usr/bin/env python3
"""The picker's own text must be readable against the terminal background.

THE DEFECT THIS LOCKS DOWN

`ui/selector.cpp` drew its frame, both separators and the key-hint line with
`theme::border()` -- #334155 on the default dark theme, against a #0D1117
background. That is a contrast ratio of about 1.6:1, where 4.5:1 is the WCAG
AA floor for body text. Six of the seven coloured elements in the widget were
that colour, so the whole thing sat in the background.

It was reported as "灰色太暗了" and it is measurable, which is why this is a
test and not a style note: the colours are emitted as truecolor SGR, so the
rendered output states its own foreground and the ratio can be computed.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]


def luminance(rgb):
    def channel(c):
        c /= 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (channel(x) for x in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(fg, bg):
    a, b = luminance(fg), luminance(bg)
    lo, hi = sorted((a, b))
    return (hi + 0.05) / (lo + 0.05)


# The default dark theme's surface, from modules/theme. Read rather than
# hardcoded so a theme change moves this test with it.
def dark_slot(name):
    src = (ROOT / "src" / "core" / "palette.cppm").read_text()
    dark = src.split("light_")[0]
    m = re.search(rf"{name}\s*\{{\s*(\d+),\s*(\d+),\s*(\d+)\s*\}}", dark)
    if not m:
        print(f"selector contrast: cannot find dark slot '{name}'", file=sys.stderr)
        sys.exit(1)
    return tuple(int(g) for g in m.groups())


surface = dark_slot("surface")
failures = []

# Every colour the picker paints its own chrome with must clear the floor.
# `border` is deliberately NOT in this list: it is a divider in panels, where
# a low-contrast hairline is correct. The rule is about text.
# Role name -> the colour name `palette.cppm` still spells it with. The two
# diverged when roles were introduced (theme::muted() reads palette::dim());
# mapping here rather than renaming keeps this test from becoming a reason to
# touch 119 call sites.
for role, colour in (("muted", "dim"), ("text", "text"), ("accent", "cyan")):
    ratio = contrast(dark_slot(colour), surface)
    if ratio < 4.5:
        failures.append(f"{role} ({colour}): {ratio:.2f}:1 against surface, need 4.5:1")

# And the picker must not be painting text with `border`.
# Code only. A comment explaining why `border` was removed must not count as
# still using it -- an assertion that a file does not CONTAIN a string is
# almost always too coarse, and here it would forbid recording the reason.
sel_lines = [
    l for l in (ROOT / "src" / "ui" / "selector.cpp").read_text().splitlines()
    if not l.lstrip().startswith("//")
]
if any("theme::border()" in l for l in sel_lines):
    failures.append(
        "selector.cpp still paints with theme::border(); it is #334155 on the "
        "default dark theme (~1.6:1) and was the reported problem")

# Each shipped theme states which picker it wants, and the value must be one
# the binary knows. A theme asking for a style that does not exist would render
# the default and look like the setting does nothing.
import json
KNOWN = {"inline", "plain", "framed"}
expected = {"mono": "plain", "high-contrast": "framed"}
for name, want in expected.items():
    f = ROOT / "config" / "themes" / f"{name}.json"
    if not f.is_file():
        failures.append(f"config/themes/{name}.json is missing")
        continue
    got = json.loads(f.read_text()).get("selector")
    if got not in KNOWN:
        failures.append(f"{name}.json asks for selector {got!r}, not one of {sorted(KNOWN)}")
    elif got != want:
        failures.append(f"{name}.json selector is {got!r}, expected {want!r}")

# And the three styles must all be reachable from the code, or a theme could
# name one that silently falls through to the default.
sel_src = (ROOT / "src" / "ui" / "selector.cpp").read_text()
for style in ("Plain", "Framed", "Inline"):
    if f"SelectorStyle::{style}" not in sel_src:
        failures.append(f"selector.cpp does not render SelectorStyle::{style}")

if failures:
    print("selector contrast: FAILED", file=sys.stderr)
    for f in failures:
        print(f"  {f}", file=sys.stderr)
    sys.exit(1)

print("selector contrast: ok (muted/text/accent clear 4.5:1; three styles "
      "reachable; mono=plain, high-contrast=framed)")
