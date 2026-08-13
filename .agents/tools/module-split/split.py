#!/usr/bin/env python3
"""Split a C++23 module interface unit that carries its own implementation
into a standard-conforming (.cppm interface, .cpp implementation) pair.

Phase 1 scope: namespace-scope entities.  Class/struct bodies are left whole
in the interface (phase 2 outlines their members).

Usage:
    split.py --dry-run  src/core/log.cppm ...
    split.py --write    src/core/log.cppm ...
    split.py --write --all
"""
from __future__ import annotations
import argparse, glob, os, re, sys

# ─────────────────────────── lexical scrubbing ───────────────────────────

def scrub(s: str) -> str:
    """Return s with comments and string/char literals blanked to spaces,
    preserving length and newlines so offsets stay valid."""
    out = list(s)
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '/' and i + 1 < n and s[i+1] == '/':
            j = s.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j): out[k] = ' '
            i = j
        elif c == '/' and i + 1 < n and s[i+1] == '*':
            j = s.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if s[k] != '\n': out[k] = ' '
            i = j
        elif c == '"' and i > 0 and s[i-1] == 'R':
            m = re.match(r'"([^(\s\\]*)\(', s[i:])
            if m:
                delim = m.group(1)
                end = s.find(')' + delim + '"', i)
                j = n if end < 0 else end + len(delim) + 2
                for k in range(i, j):
                    if s[k] != '\n': out[k] = ' '
                i = j
                continue
            i = _blank_quoted(s, out, i, '"', n)
        elif c in '"\'':
            i = _blank_quoted(s, out, i, c, n)
        else:
            i += 1
    return ''.join(out)


def _blank_quoted(s, out, i, q, n):
    j = i + 1
    while j < n:
        if s[j] == '\\':
            j += 2; continue
        if s[j] == q:
            j += 1; break
        if s[j] == '\n':
            break
        j += 1
    for k in range(i, j):
        if s[k] != '\n': out[k] = ' '
    return j


# ─────────────────────────── item scanning ───────────────────────────

class Item:
    __slots__ = ('lead', 'head', 'body', 'tail', 'kind')

    def __init__(self, lead, head, body, tail, kind):
        self.lead = lead      # comments / blank lines preceding the item
        self.head = head      # declarator text before the body brace
        self.body = body      # '{...}' including braces, or None
        self.tail = tail      # text between the body and its ';' (plus ';')
        self.kind = kind      # 'preproc' | 'decl' | 'def' | 'blank'

    @property
    def raw(self):
        return self.lead + self.head + (self.body or '') + self.tail


def scan_items(text: str) -> list[Item]:
    """Split one brace level into items.  Comments and blank lines immediately
    preceding an item become its `lead`."""
    sc = scrub(text)
    items: list[Item] = []
    i, n = 0, len(text)
    lead_start = 0
    while i < n:
        # skip whitespace/comments -> they accumulate into `lead`
        if sc[i].isspace():
            i += 1
            continue
        # preprocessor directive: whole logical line, verbatim
        if sc[i] == '#' and (i == 0 or text.rfind('\n', 0, i) == i - 1 - (i - 1 - text.rfind('\n', 0, i))
                             or text[:i].rsplit('\n', 1)[-1].strip() == ''):
            j = i
            while j < n:
                nl = sc.find('\n', j)
                if nl < 0:
                    j = n; break
                if text[nl-1] != '\\':
                    j = nl + 1; break
                j = nl + 1
            items.append(Item(text[lead_start:i], text[i:j], None, '', 'preproc'))
            i = lead_start = j
            continue
        # an item: accumulate until ';' or '{' at paren depth 0
        start = i
        pdepth = 0
        body = None
        while i < n:
            c = sc[i]
            if c in '([':
                pdepth += 1
            elif c in ')]':
                pdepth -= 1
            elif pdepth == 0 and c == ';':
                i += 1
                break
            elif pdepth == 0 and c == '{':
                bstart = i
                depth = 0
                while i < n:
                    if sc[i] == '{': depth += 1
                    elif sc[i] == '}':
                        depth -= 1
                        if depth == 0:
                            i += 1; break
                    i += 1
                body = text[bstart:i]
                break
            i += 1
        if body is None:
            head, tail = text[start:i], ''
            kind = 'decl'
        else:
            head = text[start:bstart]
            # A ';' may close the item — directly (`enum E { } ;`) or after a
            # declarator (`struct X { } inst;`).  Consume through it, but never
            # the whitespace beyond: that belongs to the NEXT item's lead,
            # which is what carries the file's line structure.
            j, tail = i, ''
            while j < n and sc[j].isspace():
                j += 1
            if j < n and sc[j] == ';':
                tail, i = text[i:j+1], j + 1
            elif j < n and re.match(r'[\w*&]', sc[j]):
                k = sc.find(';', j)
                if k >= 0 and '\n' not in sc[i:k]:
                    tail, i = text[i:k+1], k + 1
            kind = 'def'
        items.append(Item(text[lead_start:start], head, body, tail, kind))
        lead_start = i
    if lead_start < n:
        items.append(Item(text[lead_start:], '', None, '', 'blank'))
    return items


# ─────────────────────────── classification ───────────────────────────

STAY_HEAD = re.compile(
    r'\b(?:template|concept|static_assert|using|typedef|namespace|extern|friend|'
    r'requires|__attribute__)\b')
TYPE_HEAD = re.compile(r'^\s*(?:export\s+)?(?:struct|class|union|enum)\b')
NS_HEAD = re.compile(r'^\s*(?:export\s+)?namespace\b')
CONSTEXPR = re.compile(r'\b(?:constexpr|consteval|constinit)\b')


def head_norm(h: str) -> str:
    return ' '.join(scrub(h).split())


def is_function_head(h: str) -> bool:
    """A function declarator: has a parameter list that is not part of an
    initialiser (`=` before the '(' means it is a variable)."""
    s = scrub(h)
    p = s.find('(')
    if p < 0:
        return False
    before = s[:p]
    if '=' in before:
        return False
    # `foo bar[3]` style, or a cast expression -> not a function head
    return re.search(r'[\w>~\]]\s*$', before) is not None


def classify(it: Item, in_class: bool) -> str:
    """Return 'stay' | 'namespace' | 'both' | 'split-fn' | 'split-var'."""
    if it.kind == 'blank':
        return 'stay'
    if it.kind == 'preproc':
        return 'both'                      # conditionals must bracket both units
    h = head_norm(it.head)
    if not h:
        return 'stay'
    if NS_HEAD.match(it.head):
        return 'namespace' if it.body else 'stay'
    if TYPE_HEAD.match(it.head):
        return 'stay'                      # phase 1: types stay whole
    if CONSTEXPR.search(h) or STAY_HEAD.search(h):
        return 'stay'
    if re.search(r'\binline\b', h):
        return 'stay'
    # `const` at namespace scope has internal linkage, so its definition
    # cannot be moved to another unit without changing linkage.
    if re.search(r'\bconst\b', h.split('(')[0]):
        return 'stay'
    if is_function_head(it.head):
        if it.body is None:
            return 'stay'                  # already only a declaration
        if re.search(r'=\s*(?:default|delete)\s*;?\s*$', h):
            return 'stay'
        return 'split-fn'
    # A variable definition — with an initialiser (`T x {..};`, `T x = ..;`)
    # or default-constructed (`T x;`).  Both are definitions and both move.
    if re.match(r'^(?:export\s+)?(?:static\s+)?[\w:<>,\s*&\[\]]+\w\s*(?:$|=|\[)', h.rstrip(';')):
        return 'split-var'
    return 'stay'


# ─────────────────────────── head rewriting ───────────────────────────

def strip_export(h: str) -> str:
    return re.sub(r'(^|\s)export\s+', r'\1', h, count=1)


def strip_static(h: str) -> str:
    """Namespace-scope `static` becomes module linkage so the declaration and
    the definition can live in different units of the same module."""
    return re.sub(r'(^|\s)static\s+', r'\1', h, count=1)


def split_params(params: str) -> list[str]:
    out, depth, cur = [], 0, ''
    for c in params:
        if c in '([{<':
            depth += 1
        elif c in ')]}>':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += c
    if cur.strip():
        out.append(cur)
    return out


def strip_defaults(h: str) -> str:
    """Remove default arguments — they must appear only in the declaration."""
    sc = scrub(h)
    o = sc.find('(')
    if o < 0:
        return h
    depth, i = 0, o
    while i < len(sc):
        if sc[i] == '(': depth += 1
        elif sc[i] == ')':
            depth -= 1
            if depth == 0: break
        i += 1
    params_raw = h[o+1:i]
    if '=' not in scrub(params_raw):
        return h
    parts = []
    for p in split_params(params_raw):
        eq = scrub(p).find('=')
        parts.append(p[:eq].rstrip() if eq >= 0 else p)
    return h[:o+1] + ', '.join(x.strip() for x in parts) + h[i:]


def decl_from_def(h: str) -> str:
    """The interface declaration for a definition head."""
    return h.rstrip().rstrip() + ';'


# ─────────────────────────── emission ───────────────────────────

class Split:
    def __init__(self):
        self.iface: list[str] = []
        self.impl: list[str] = []
        self.moved = 0
        self.moved_lines = 0


def process_level(items: list[Item], out: Split, indent: str, in_class=False):
    for it in items:
        cls = classify(it, in_class)
        if cls == 'both':
            out.iface.append(it.lead + it.head)
            out.impl.append(it.lead + it.head)
        elif cls == 'namespace':
            inner = it.body[1:-1]
            sub = Split()
            process_level(scan_items(inner), sub, indent)
            out.iface.append(it.lead + it.head + '{' + ''.join(sub.iface)
                             + '}' + it.tail)
            if any(x.strip() for x in sub.impl):
                body = '\n\n'.join(x.strip('\n') for x in sub.impl if x.strip())
                out.impl.append(strip_export(it.head).lstrip() + '{\n\n'
                                + body + '\n\n}' + it.tail)
            out.moved += sub.moved
            out.moved_lines += sub.moved_lines
        elif cls == 'stay':
            out.iface.append(it.raw)
        elif cls == 'split-fn':
            out.iface.append(it.lead + decl_from_def(it.head))
            impl_head = strip_defaults(strip_static(strip_export(it.head)))
            out.impl.append(impl_head.lstrip('\n') + it.body + it.tail)
            out.moved += 1
            out.moved_lines += it.body.count('\n')
        elif cls == 'split-var':
            h = strip_static(strip_export(it.head))
            bare = h.strip().rstrip(';').rstrip() if it.body is None \
                else h.rstrip()
            # the declaration carries the type and name, never the initialiser
            eq = scrub(bare).find('=')
            decl = (bare[:eq].rstrip() if eq >= 0 else bare).strip()
            out.iface.append(it.lead + 'extern ' + decl + ';')
            defn = bare + (it.body or '') + it.tail
            out.impl.append(defn.strip() if defn.rstrip().endswith(';')
                            else defn.strip() + ';')
            out.moved += 1
            out.moved_lines += (it.body or '').count('\n')
        else:
            out.iface.append(it.raw)


PREAMBLE = re.compile(r'\A(?P<gmf>.*?)^export module (?P<mod>[\w.]+);',
                      re.S | re.M)


def split_file(path: str):
    src = open(path).read()
    m = PREAMBLE.search(src)
    if not m:
        return None, f'{path}: no `export module` declaration'
    gmf, mod = m.group('gmf'), m.group('mod')
    rest = src[m.end():]
    # the import block that immediately follows the module declaration
    im = re.match(r'\A((?:\s*(?:import [\w.:]+;|//[^\n]*|/\*.*?\*/))*\s*)',
                  rest, re.S)
    imports_blk = im.group(1) if im else ''
    body = rest[len(imports_blk):]
    imports = re.findall(r'^import [\w.:]+;', imports_blk, re.M)

    out = Split()
    process_level(scan_items(body), out, '')
    if out.moved == 0:
        return None, f'{path}: nothing to move'

    iface = gmf + f'export module {mod};' + imports_blk + ''.join(out.iface)
    impl = (gmf if gmf.strip() else '') + f'module {mod};\n'
    if imports:
        impl += '\n' + '\n'.join(imports) + '\n'
    impl += '\n' + '\n\n'.join(x.strip('\n') for x in out.impl if x.strip())
    return (iface, impl.rstrip() + '\n', out.moved, out.moved_lines, mod), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    files = sorted(glob.glob('src/**/*.cppm', recursive=True)) if a.all else a.files
    tot_fn = tot_lines = tot_files = 0
    for f in files:
        res, err = split_file(f)
        if err:
            print(f'SKIP {err}')
            continue
        iface, impl, moved, mlines, mod = res
        tot_fn += moved; tot_lines += mlines; tot_files += 1
        print(f'{f}: {moved} entities, {mlines} body lines -> '
              f'{os.path.splitext(f)[0]}.cpp')
        if a.write:
            open(f, 'w').write(iface)
            open(os.path.splitext(f)[0] + '.cpp', 'w').write(impl)
    print(f'\n{tot_files} files, {tot_fn} entities, {tot_lines} body lines moved')


if __name__ == '__main__':
    main()
