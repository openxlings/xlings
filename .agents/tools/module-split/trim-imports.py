#!/usr/bin/env python3
"""Phase 3 of the split: move the imports the bodies took with them.

split.py COPIES the interface's import list into the implementation unit. That is
correct for the implementation -- it needs them -- but it leaves the interface
importing modules only the moved bodies used. 249 of 562 interface import edges
(44%) ended up like that, and every one makes the interface's BMI depend on a
module it does not name, so an edit to that module recompiles this interface for
nothing. It caps the very benefit the split exists to deliver.

An import is kept when the interface still NAMES something the imported module
exports. That test needs the export sets, so this runs as a post-pass over the
finished tree rather than inside split.py, which sees one file at a time.

Never dropped:
  * `export import X;` -- a deliberate re-export, an aggregator's whole purpose
  * `import std;` and `import :partition;`
  * anything whose module cannot be resolved locally (mcpplibs, compat, ...)

Usage:  trim-imports.py [--write]
"""
from __future__ import annotations
import argparse, glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from split import scrub, scan_items, mask_operator, NS_HEAD

KW = {'const', 'static', 'inline', 'constexpr', 'consteval', 'extern', 'virtual',
      'explicit', 'friend', 'operator', 'noexcept', 'return', 'template',
      'typedef', 'mutable', 'volatile', 'namespace', 'export', 'struct', 'class',
      'union', 'enum', 'using', 'void', 'auto'}


def item_names(head: str) -> set[str]:
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
    if o >= 0:
        m = list(re.finditer(r'([\w~]+)\s*$', region))
        if m and m[-1].group(1) not in KW:
            return {m[-1].group(1)}
    ids = [i for i in re.findall(r'\b([A-Za-z_]\w*)\b', region) if i not in KW]
    return {ids[-1]} if ids else set()


def exported_names(text: str) -> set[str]:
    """Every name this interface unit exports, at any namespace depth."""
    names: set[str] = set()

    def walk(body: str, inherited: bool):
        for it in scan_items(body):
            if it.kind in ('preproc', 'blank', 'access'):
                continue
            exported = inherited or re.match(r'\s*export\b', it.head) is not None
            if it.body and NS_HEAD.match(it.head):
                walk(it.body[1:-1], exported)
                continue
            if exported:
                names.update(item_names(it.head))
                # a class also exports its member names, which callers write
                if it.body and re.match(r'\s*(?:export\s+)?(?:struct|class|union)\b',
                                        it.head):
                    for m in scan_items(it.body[1:-1]):
                        if m.kind not in ('preproc', 'blank', 'access'):
                            names.update(item_names(m.head))

    m = re.search(r'^export module [\w.]+(?::\w+)?;', text, re.M)
    walk(text[m.end():] if m else text, False)
    return {n for n in names if n}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()

    files = sorted(glob.glob('src/**/*.cppm', recursive=True))
    srcs = {f: open(f).read() for f in files}

    # module (including partitions) -> exported names
    exports: dict[str, set[str]] = {}
    primary_of: dict[str, str] = {}
    for f, s in srcs.items():
        m = re.search(r'^export module ([\w.]+)(:\w+)?;', s, re.M)
        if not m:
            continue
        full = m.group(1) + (m.group(2) or '')
        exports[full] = exported_names(s)
        primary_of[full] = m.group(1)
    # A module offers everything it RE-EXPORTS as well as what it declares:
    # its own partitions, and any `export import X;` of a whole module. Missing
    # the second kind made every aggregator (`xlings.runtime` re-exports
    # event/event_stream/capability and declares nothing itself) look like it
    # exported no names at all, so importers of it were trimmed and
    # `interface.cppm` lost `Event`, `EventStream` and `CancellationToken`.
    for full, names in list(exports.items()):
        p = primary_of[full]
        if p != full:
            exports.setdefault(p, set())
            exports[p] |= names

    reexports: dict[str, set[str]] = {}
    for f, src in srcs.items():
        m = re.search(r'^export module ([\w.]+)(:\w+)?;', src, re.M)
        if not m:
            continue
        full = m.group(1) + (m.group(2) or '')
        reexports[full] = set(re.findall(r'^export import\s+([\w.:]+)\s*;', src, re.M))

    def closure(mod, seen=None):
        seen = set() if seen is None else seen
        if mod in seen:
            return set()
        seen.add(mod)
        names = set(exports.get(mod, ()))
        for r in reexports.get(mod, ()):
            target = (primary_of.get(mod, mod) + r) if r.startswith(':') else r
            names |= closure(target, seen)
        return names

    exports = {m: closure(m) for m in list(exports)}

    total = dropped = 0
    report = []
    for f, s in srcs.items():
        m = re.search(r'^export module [\w.]+(?::\w+)?;', s, re.M)
        if not m:
            continue
        head, body = s[:m.end()], s[m.end():]
        lines = body.split('\n')
        # the text the interface actually is, minus its own import lines
        text = scrub('\n'.join(
            l for l in lines if not re.match(r'\s*(?:export\s+)?import\s', l)))
        out, drops = [], []
        for l in lines:
            im = re.match(r'\s*import\s+([\w.:]+)\s*;\s*$', l)
            if not im:
                out.append(l)
                continue
            mod = im.group(1)
            total += 1
            names = exports.get(mod)
            if mod == 'std' or mod.startswith(':') or names is None:
                out.append(l)                      # std, partition, or foreign
                continue
            # keep it if the interface names anything this module exports, or
            # mentions a segment of the module name (belt and braces)
            segs = [x for x in mod.split('.') if x != 'xlings']
            used = any(re.search(r'(?<![\w])' + re.escape(n) + r'(?![\w])', text)
                       for n in names) or \
                   any(re.search(r'(?<![\w])' + re.escape(sg) + r'(?![\w])', text, re.I)
                       for sg in segs)
            if used:
                out.append(l)
            else:
                drops.append(mod)
                dropped += 1
        if drops:
            report.append((f, drops))
            if a.write:
                open(f, 'w').write(head + '\n'.join(out))

    for f, drops in report:
        print(f'{f}: -{len(drops)}  {", ".join(drops)}')
    print(f'\n{len(report)} interfaces, {dropped}/{total} plain imports dropped '
          f'({100*dropped/max(total,1):.0f}%)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
