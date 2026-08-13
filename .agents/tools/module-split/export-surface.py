#!/usr/bin/env python3
"""Compare the exported-name surface of the interface units before and after the
split.  Nothing about this refactor may add or remove an export -- that is the
strongest invariant checkable without running the program.

It has to be depth-aware.  A line-anchored regex over an `export namespace`
body also matches the LOCAL VARIABLES inside the function bodies that live
there, so before the split it "sees" 900 more names than after -- an artefact of
the bodies moving, not a lost export.  So the namespace body is scanned with the
same item scanner the splitter uses, and only top-level declarators count.
"""
import re, glob, subprocess, sys
sys.path.insert(0, '.agents/tools/module-split')
from split import scrub, scan_items, mask_operator, NS_HEAD

KW = {'const', 'static', 'inline', 'constexpr', 'consteval', 'extern', 'virtual',
      'explicit', 'friend', 'operator', 'noexcept', 'return', 'template',
      'typedef', 'mutable', 'volatile', 'namespace', 'export'}


def item_name(head: str) -> set[str]:
    h = mask_operator(scrub(head)).strip().rstrip(';').strip()
    if not h:
        return set()
    t = re.match(r'(?:export\s+)?(?:struct|class|union|enum(?:\s+class)?)\s+(\w+)', h)
    if t:
        return {t.group(1)}
    a = re.match(r'(?:export\s+)?using\s+(\w+)\s*=', h)
    if a:
        return {a.group(1)}
    u = re.match(r'(?:export\s+)?using\s+[\w:]*?::(\w+)\s*$', h)
    if u:
        return {u.group(1)}
    if re.match(r'(?:export\s+)?(?:import|module)\b', h):
        return set()
    o = h.find('(')
    region = h[:o] if o >= 0 else h
    ids = [i for i in re.findall(r'\b([A-Za-z_]\w*)\b', region) if i not in KW]
    if not ids:
        return set()
    # for a function the declarator-id is the identifier just before '('
    if o >= 0:
        m = list(re.finditer(r'([\w~]+)\s*$', region))
        if m and m[-1].group(1) not in KW:
            return {m[-1].group(1)}
    return {ids[-1]}


def collect(text: str, inherited_export=False) -> set[str]:
    m = re.search(r'^export module [\w.]+(?::\w+)?;', text, re.M)
    body = text[m.end():] if m else text
    return walk(body, inherited_export)


def walk(body: str, inherited: bool) -> set[str]:
    names = set()
    for it in scan_items(body):
        if it.kind in ('preproc', 'blank', 'access'):
            continue
        exported = inherited or re.match(r'\s*export\b', it.head) is not None
        if it.body and NS_HEAD.match(it.head):
            names |= walk(it.body[1:-1], exported)
            continue
        if exported:
            names |= item_name(it.head)
    return names


files = [f for f in subprocess.run(
    ['git', 'ls-tree', '-r', '--name-only', 'b1563fe', 'src/'],
    capture_output=True, text=True).stdout.split() if f.endswith('.cppm')]

before, after = set(), set()
for f in files:
    t = subprocess.run(['git', 'show', f'b1563fe:{f}'],
                       capture_output=True, text=True).stdout
    before |= collect(t)
for f in glob.glob('src/**/*.cppm', recursive=True):
    after |= collect(open(f).read())

print(f'exported identifiers  before={len(before)}  after={len(after)}')
lost, gained = sorted(before - after), sorted(after - before)
print(f'lost={len(lost)}  gained={len(gained)}')
if lost:
    print('  LOST  :', lost[:40])
if gained:
    print('  GAINED:', gained[:40])
