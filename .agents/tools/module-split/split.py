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
        # an access specifier is its own item — it ends with ':' and would
        # otherwise swallow everything up to the next ';'
        am = re.match(r'(public|private|protected)\s*:', sc[i:])
        if am:
            j = i + am.end()
            items.append(Item(text[lead_start:i], text[i:j], None, '', 'access'))
            i = lead_start = j
            continue
        # an item: accumulate until ';' or '{' at paren depth 0
        start = i
        pdepth = 0
        body = None
        seen_params = False     # a ')' has closed at depth 0
        in_init = False         # inside a constructor's member-init list
        while i < n:
            c = sc[i]
            if c in '([':
                pdepth += 1
            elif c in ')]':
                pdepth -= 1
                if pdepth == 0 and c == ')':
                    seen_params = True
            elif (pdepth == 0 and c == ':' and seen_params
                  and sc[i:i+2] != '::' and (i == 0 or sc[i-1] != ':')):
                in_init = True
            elif pdepth == 0 and c == ';':
                i += 1
                break
            elif pdepth == 0 and c == '{':
                # In a member-init list an initialiser's brace follows an
                # IDENTIFIER (`: registry_ { registry }`) while the function
                # body's brace follows ')' or '}'.  Without this the first
                # initialiser gets mistaken for the body and the declaration
                # comes out as `Ctor(args); {}`.
                if in_init:
                    k = i - 1
                    while k >= 0 and sc[k].isspace():
                        k -= 1
                    if k >= 0 and (sc[k].isalnum() or sc[k] == '_'):
                        d = 0
                        while i < n:
                            if sc[i] == '{':
                                d += 1
                            elif sc[i] == '}':
                                d -= 1
                                if d == 0:
                                    i += 1
                                    break
                            i += 1
                        continue
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


# `operator=`, `operator==`, `operator<=>`, `operator[]`, `operator()`, a
# conversion operator...  The '=' and the parentheses in these are part of the
# NAME, so they must not be read as an initialiser or a parameter list.
OPERATOR_NAME = re.compile(
    r'\boperator\s*(?:new\b|delete\b|""\s*\w+|\(\s*\)|\[\s*\]|'
    r'[+\-*/%^&|~!<>=,]+)')


def mask_operator(s: str) -> str:
    return OPERATOR_NAME.sub(lambda m: 'operator_' + '_' * (len(m.group(0)) - 9), s)


def is_function_head(h: str) -> bool:
    """A function declarator: has a parameter list that is not part of an
    initialiser (`=` before the '(' means it is a variable)."""
    s = mask_operator(scrub(h))
    p = s.find('(')
    if p < 0:
        return False
    before = s[:p]
    if '=' in before:
        return False
    # `foo bar[3]` style, or a cast expression -> not a function head
    return re.search(r'[\w>~\]]\s*$', before) is not None


def has_deduced_return(h: str) -> bool:
    """`auto f(...) { ... }` with no trailing return type: the return type is
    deduced FROM THE BODY, so a caller in another unit cannot see it.  With a
    trailing `-> T` the declaration is complete and the body can move."""
    s = mask_operator(scrub(h))
    if '->' in s:
        return False
    return re.match(r'^\s*(?:export\s+)?(?:static\s+)?(?:\[\[[^\]]*\]\]\s*)*'
                    r'(?:constexpr\s+|inline\s+)*'
                    r'(?:auto|decltype\s*\(\s*auto\s*\))\b', s) is not None


def has_auto_parameter(h: str) -> bool:
    """`void f(auto x)` is an abbreviated function template."""
    s = mask_operator(scrub(h))
    o = s.find('(')
    if o < 0:
        return False
    depth, i = 0, o
    while i < len(s):
        if s[i] == '(': depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0: break
        i += 1
    return re.search(r'\bauto\b', s[o:i]) is not None


def is_qualified_definition(h: str) -> bool:
    """`T C::f(...)` or `T ns::f(...)` — a definition written with a qualified
    name.  An out-of-line member cannot be *declared* at namespace scope, so
    phase 1 leaves every qualified definition where it is."""
    s = mask_operator(scrub(h))
    p = s.find('(')
    decl = s[:p] if p >= 0 else s
    return '::' in decl.split()[-1] if decl.split() else False


def classify(it: Item, in_class: bool) -> str:
    """Return 'stay' | 'namespace' | 'both' | 'split-fn' | 'split-var'."""
    if it.kind in ('blank', 'access'):
        return 'stay'
    if it.kind == 'preproc':
        return 'both'                      # conditionals must bracket both units
    h = head_norm(it.head)
    if not h:
        return 'stay'
    # a module/import declaration that did not land in the preamble block
    if re.match(r'^(?:export\s+)?(?:import|module)\b', h):
        return 'stay'
    if NS_HEAD.match(it.head):
        if not it.body:
            return 'stay'
        # An ANONYMOUS namespace has internal linkage: its members cannot be
        # declared in one unit and defined in another, and the implementation
        # unit cannot see the interface's copy.  It moves whole.
        return 'namespace' if head_norm(it.head).rstrip() != 'namespace' \
            else 'move-impl'
    if TYPE_HEAD.match(it.head):
        return 'stay'                      # phase 1: types stay whole
    if CONSTEXPR.search(h) or STAY_HEAD.search(h):
        return 'stay'
    if re.search(r'\binline\b', h):
        return 'stay'
    # Function or variable is decided FIRST: the rules below read the text
    # before the parameter list, and in a variable that text is an initialiser
    # expression, where a `const` or a `::` means nothing about the declaration.
    if is_function_head(it.head):
        if it.body is None:
            return 'stay'                  # already only a declaration
        if re.search(r'=\s*(?:default|delete)\s*;?\s*$', h):
            return 'stay'
        if is_qualified_definition(it.head):
            return 'stay'                  # phase 1: no out-of-line members
        if has_deduced_return(it.head) or has_auto_parameter(it.head):
            return 'stay'
        return 'split-fn'
    # A variable definition — with an initialiser (`T x {..};`, `T x = ..;`)
    # or default-constructed (`T x;`).  Both are definitions and both move.
    # It needs a type AND a name: a lone identifier is a macro invocation
    # (`NLOHMANN_JSON_NAMESPACE_END;`), not a declaration.
    bare = h.rstrip(';').rstrip()
    lhs = mask_operator(scrub(bare)).split('=')[0]
    # `const` at namespace scope has internal linkage, so its definition cannot
    # move to another unit without changing that linkage.
    if re.search(r'\bconst\b', lhs):
        return 'stay'
    if re.match(r'^(?:export\s+)?(?:static\s+)?[\w:<>,\s*&\[\]]+\w\s*(?:$|=|\[)',
                bare) and len(re.sub(r'\s*[*&]\s*', ' ', lhs).split()) >= 2:
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

class Cand:
    """A module-private entity whose declaration may or may not be needed in
    the interface."""
    __slots__ = ('ns', 'decl', 'name', 'impl_with_defaults', 'impl_no_defaults')

    def __init__(self, ns, decl, name, impl_with_defaults, impl_no_defaults):
        self.ns = ns
        self.decl = decl
        self.name = name
        self.impl_with_defaults = impl_with_defaults
        self.impl_no_defaults = impl_no_defaults


class Split:
    def __init__(self):
        self.iface: list[str] = []
        self.impl: list[str] = []
        self.cands: list[Cand] = []
        self.moved = 0
        self.moved_lines = 0


# A non-exported entity is wrapped in these while the two texts are assembled,
# so one later pass can ask whether anything LEFT in the interface still names
# it -- and fix up BOTH sides together.  These bytes cannot occur in C++ source.
#
# Both sides are needed because the decision changes the definition too: if the
# interface keeps the declaration, the declaration carries the default arguments
# and the definition must not repeat them; if the declaration is dropped, the
# definition is the only declaration and must carry them itself.
IFACE_MARK, IMPL_MARK, MARK_END = '\x01', '\x02', '\x03'


def is_exported(head: str) -> bool:
    return re.match(r'^\s*export\b', head) is not None


def process_level(items: list[Item], out: Split, indent: str, in_class=False,
                  ns: str = '', exported: bool = False):
    for it in items:
        cls = classify(it, in_class)
        if cls == 'both':
            out.iface.append(it.lead + it.head)
            out.impl.append(it.lead + it.head)
        elif cls == 'namespace':
            inner = it.body[1:-1]
            sub = Split()
            # one shared candidate list per file, so the ids that pair an
            # interface marker with its implementation marker stay unique across
            # nesting levels
            sub.cands = out.cands
            nm = re.sub(r'^\s*(?:export\s+)?namespace\s*', '',
                        it.head).strip().rstrip('{').strip()
            sub_ns = f'{ns}::{nm}' if ns and nm else (nm or ns)
            process_level(scan_items(inner), sub, indent, ns=sub_ns,
                          exported=exported or is_exported(it.head))
            out.iface.append(it.lead + it.head + '{' + ''.join(sub.iface)
                             + '}' + it.tail)
            if any(x.strip() for x in sub.impl):
                body = '\n\n'.join(x.strip('\n') for x in sub.impl if x.strip())
                out.impl.append(strip_export(it.head).lstrip() + '{\n\n'
                                + body + '\n\n}' + it.tail)
            out.moved += sub.moved
            out.moved_lines += sub.moved_lines
        elif cls == 'move-impl':
            out.impl.append(it.lead.strip('\n') + '\n' + it.head + it.body + it.tail
                            if it.lead.strip() else it.head + it.body + it.tail)
            out.moved += 1
            out.moved_lines += (it.body or '').count('\n')
        elif cls == 'stay':
            out.iface.append(it.raw)
        elif cls == 'split-fn':
            decl = it.lead + decl_from_def(it.head)
            plain = strip_static(strip_export(it.head))
            no_def = strip_defaults(plain).lstrip('\n') + it.body + it.tail
            if exported or is_exported(it.head):
                out.iface.append(decl)
                out.impl.append(no_def)
            else:
                cid = str(len(out.cands))
                out.cands.append(Cand(ns, decl, declared_name(it.head),
                                      plain.lstrip('\n') + it.body + it.tail,
                                      no_def))
                out.iface.append(IFACE_MARK + cid + MARK_END)
                out.impl.append(IMPL_MARK + cid + MARK_END)
            out.moved += 1
            out.moved_lines += it.body.count('\n')
        elif cls == 'split-var':
            h = strip_static(strip_export(it.head))
            bare = h.strip().rstrip(';').rstrip() if it.body is None \
                else h.rstrip()
            # the declaration carries the type and name, never the initialiser
            eq = scrub(bare).find('=')
            decl = (bare[:eq].rstrip() if eq >= 0 else bare).strip()
            ext = it.lead + 'extern ' + decl + ';'
            defn = bare + (it.body or '') + it.tail
            defn = defn.strip() if defn.rstrip().endswith(';') \
                else defn.strip() + ';'
            if exported or is_exported(it.head):
                out.iface.append(ext)
                out.impl.append(defn)
            else:
                cid = str(len(out.cands))
                out.cands.append(Cand(ns, ext, declared_name(bare), defn, defn))
                out.iface.append(IFACE_MARK + cid + MARK_END)
                out.impl.append(IMPL_MARK + cid + MARK_END)
            out.moved += 1
            out.moved_lines += (it.body or '').count('\n')
        else:
            out.iface.append(it.raw)


IFACE_RE = re.compile(IFACE_MARK + r'(\d+)' + MARK_END)
IMPL_RE = re.compile(IMPL_MARK + r'(\d+)' + MARK_END)


def declared_name(decl: str) -> str:
    """The name a declaration introduces."""
    m = mask_operator(scrub(decl))
    o = m.find('(')
    region = m[:o] if o >= 0 else m.rstrip().rstrip(';')
    hits = list(re.finditer(r'[\w~]+', region))
    return region[hits[-1].start():hits[-1].end()] if hits else ''


def resolve_candidates(iface: str, impl: str, cands: list):
    """A NON-exported declaration belongs in the interface only if something
    still IN the interface names it -- an exported template, an inline or
    constexpr body, a class member body, a default argument.  Everything else is
    a module-private helper: keeping its declaration in the interface would put
    it in the BMI, so changing its signature would recompile every importer for
    no reason.

    Both texts are fixed up here because the two are coupled -- see the comment
    on IFACE_MARK."""
    if not cands:
        return IFACE_RE.sub('', iface), IMPL_RE.sub('', impl)
    # what the interface still says with every candidate declaration removed
    residue = IFACE_RE.sub('', iface)
    keep = set()
    for i, c in enumerate(cands):
        # `detail_::binding_error_(...)` is a reference to binding_error_.  A
        # lookbehind that rejected ':' would miss every qualified call, which is
        # the common form.  Over-counting only keeps a declaration that could
        # have moved; under-counting breaks the build.
        if c.name and re.search(r'(?<!\w)' + re.escape(c.name) + r'\b', residue):
            keep.add(i)
    iface = IFACE_RE.sub(
        lambda m: cands[int(m.group(1))].decl if int(m.group(1)) in keep else '',
        iface)
    impl = IMPL_RE.sub(
        lambda m: (cands[int(m.group(1))].impl_no_defaults
                   if int(m.group(1)) in keep
                   else cands[int(m.group(1))].impl_with_defaults),
        impl)
    return iface, impl


PREAMBLE = re.compile(
    r'\A(?P<gmf>.*?)^export module (?P<mod>[\w.]+)(?P<part>:\w+)?;',
    re.S | re.M)

# `import x.y;`, `import :part;`, `export import :part;`
IMPORT_LINE = re.compile(r'^(?:export\s+)?import\s+[\w.:]+\s*;', re.M)


def split_file(path: str, qualify_partition_import=False):
    src = open(path).read()
    m = PREAMBLE.search(src)
    if not m:
        return None, f'{path}: no `export module` declaration'
    gmf, mod, part = m.group('gmf'), m.group('mod'), m.group('part') or ''
    rest = src[m.end():]
    # the import block that immediately follows the module declaration
    im = re.match(
        r'\A((?:\s*(?:(?:export\s+)?import\s+[\w.:]+\s*;|//[^\n]*|/\*.*?\*/))*\s*)',
        rest, re.S)
    imports_blk = im.group(1) if im else ''
    body = rest[len(imports_blk):]
    # An implementation unit cannot re-export, so `export import` becomes a
    # plain import there.  Partition imports are dropped: the primary interface
    # already does `export import :p;`, an implementation unit implicitly
    # imports that primary, and the ordering edge reaches the partition BMI
    # through it.  Keeping them would only add mcpp's cosmetic
    # "module ':p' imported but not provided" warning on every build.
    imports = [re.sub(r'^export\s+', '', x)
               for x in IMPORT_LINE.findall(imports_blk)
               if not re.match(r'^(?:export\s+)?import\s+:', x)]

    out = Split()
    process_level(scan_items(body), out, '')
    if out.moved == 0:
        return None, f'{path}: nothing to move'

    iface = gmf + f'export module {mod}{part};' + imports_blk + ''.join(out.iface)
    # The implementation of a PARTITION's declarations belongs to an
    # implementation unit of the PRIMARY module (`module M;`) — `module M:p;`
    # would be another partition and would still produce a BMI.  A module may
    # have any number of implementation units.
    impl = (gmf if gmf.strip() else '') + f'module {mod};\n'
    if part and qualify_partition_import and f'import {part};' not in imports:
        imports.insert(0, f'import {part};')
    if imports:
        impl += '\n' + '\n'.join(imports) + '\n'
    # A dropped declaration is not re-emitted as a forward declaration: source
    # order is preserved, so a definition still precedes its callers exactly as
    # it did in the combined file -- and a namespace-scope declaration block is
    # not free.  One mentioning `std::vector<xvm::SubosRef>` made gcc 16.1.0
    # segfault in doctor.cpp, the known shape where a std container over a
    # module-attached type is first instantiated from namespace scope.
    impl += '\n' + '\n\n'.join(x.strip('\n') for x in out.impl if x.strip())
    iface, impl = resolve_candidates(iface, impl, out.cands)
    return (iface, impl.rstrip() + '\n', out.moved, out.moved_lines,
            mod + part), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--partition-import', action='store_true',
                    help="emit `import :part;` in a partition's implementation "
                         "unit (needed only when it defines entities the "
                         "partition does not export)")
    a = ap.parse_args()
    files = sorted(glob.glob('src/**/*.cppm', recursive=True)) if a.all else a.files
    tot_fn = tot_lines = tot_files = 0
    for f in files:
        res, err = split_file(f, a.partition_import)
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
