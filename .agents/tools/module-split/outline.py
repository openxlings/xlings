#!/usr/bin/env python3
"""Phase 2 of the interface/implementation split: move class member function
BODIES out of the class definition in the .cppm into out-of-line definitions in
the module's .cpp.

Phase 1 (split.py) moved namespace-scope functions.  What is left in the
interface is the 15% of the code that sits inside a class body, where a body is
implicitly inline and therefore still part of the BMI — so editing it still
recompiles every importer.  `Config` is the case that matters: 93% class body
and 34 direct importers.

Out-of-lining a member is more than moving text:

  * the declarator-id becomes `C::name`, and everything AFTER it is looked up in
    class scope — but the RETURN TYPE comes before it and is not, so a return
    type naming a nested type has to be qualified
  * `static` and `virtual` are declaration-only specifiers and must be dropped
  * a default argument may appear only in the in-class declaration
  * a constructor's member-init list travels with the definition, not the
    declaration

Usage:  outline.py --write src/core/config.cppm ...
        outline.py --write --all
"""
from __future__ import annotations
import argparse, glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from split import (scan_items, scrub, mask_operator, head_norm, Item,
                   strip_defaults, has_deduced_return, has_auto_parameter,
                   ensure_cstdio, CONSTEXPR, TYPE_HEAD, NS_HEAD)

CLASS_HEAD = re.compile(
    r'^\s*(?:export\s+)?(?P<kw>struct|class)\s+(?P<name>\w+)'
    r'(?P<rest>[^;{]*)$')

# Members that must keep their body in the class because moving it makes
# gcc@16.1.0 crash.  Each entry is (path suffix, class, member) and needs a
# reason — an unexplained exclusion is indistinguishable from superstition.
ICE_SKIP = {
    # gcc 16.1.0 segfaults in cc1plus while compiling doctor.cpp once
    # `Counts::issues()` is defined out-of-line.  The crash is reported against
    # `DoctorState st;` at doctor.cpp:55 -- a different type, in a different
    # function -- so the message names the wrong entity entirely.  It is a
    # compiler bug, not a code error, and it leaves a truncated .gcm behind that
    # makes unrelated targets fail next build, so this 2-line accessor stays
    # inline.
    ('core/xself/doctor.cppm', 'Counts', 'issues'),

    # `static Config& instance_() { static Config inst; return inst; }` -- the
    # singleton accessor.  Its function-local static is where the module-attached
    # `Config` (std containers over module types inside) is first completed.
    # Move that body to the implementation unit and the first-instantiation point
    # moves with it, after which gcc 16.1.0 segfaults compiling an UNRELATED
    # translation unit -- doctor.cpp, at `DoctorState st;`, a different type in a
    # different module.  Found by bisecting config.cppm's 85 movable members;
    # #34 is the boundary and the other 84 are fine.
    ('core/config.cppm', 'Config', 'instance_'),
}


def ice_skipped(path: str, cls: str, member: str) -> bool:
    return any(path.replace(os.sep, '/').endswith(p) and cls == c and member == m
               for p, c, m in ICE_SKIP)


def directive_condition(line: str):
    """Normalise an #if-family directive to a boolean expression string, or
    None if the line does not open a conditional."""
    m = re.match(r'\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)', line, re.S)
    if not m:
        return None
    kw, rest = m.group(1), m.group(2).strip().rstrip('\\').strip()
    if kw == 'ifdef':
        return ('open', f'defined({rest})')
    if kw == 'ifndef':
        return ('open', f'!defined({rest})')
    if kw == 'if':
        return ('open', f'({rest})')
    if kw == 'elif':
        return ('elif', f'({rest})')
    if kw == 'else':
        return ('else', None)
    return ('close', None)


class GuardStack:
    """Tracks the #if nesting so an outlined definition can be re-wrapped in
    the same condition.  A class inside `#if defined(_WIN32)` whose members are
    appended to the .cpp unguarded would compile its Windows bodies on Linux —
    which is exactly what happened before this existed."""

    def __init__(self):
        self.stack = []          # list of [current_condition, seen_conditions]

    def feed(self, line: str):
        d = directive_condition(line)
        if not d:
            return
        kind, cond = d
        if kind == 'open':
            self.stack.append([cond, [cond]])
        elif kind == 'elif' and self.stack:
            prev = self.stack[-1][1]
            self.stack[-1][0] = ' && '.join(
                [f'!({p})' for p in prev] + [cond])
            self.stack[-1][1].append(cond)
        elif kind == 'else' and self.stack:
            prev = self.stack[-1][1]
            self.stack[-1][0] = ' && '.join(f'!({p})' for p in prev)
        elif kind == 'close' and self.stack:
            self.stack.pop()

    def current(self):
        conds = [s[0] for s in self.stack if s[0]]
        return ' && '.join(conds) if conds else None


def nested_type_names(body: str) -> set[str]:
    """Types declared inside this class body — a return type naming one of
    them has to be qualified in an out-of-line definition."""
    sc = scrub(body)
    names = set(re.findall(r'\b(?:struct|class|enum(?:\s+class)?|union)\s+(\w+)',
                           sc))
    names |= set(re.findall(r'\busing\s+(\w+)\s*=', sc))
    return names


def split_member_init(head: str):
    """Return (declarator_part, member_init_part).  The member-init list starts
    at a ':' that is not '::' and sits after the closing ')' of the params."""
    s = mask_operator(scrub(head))
    o = s.find('(')
    if o < 0:
        return head, ''
    depth, i = 0, o
    while i < len(s):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    j = i
    while j < len(s):
        if s[j] == ':' and s[j:j+2] != '::' and (j == 0 or s[j-1] != ':'):
            return head[:j], head[j:]
        j += 1
    return head, ''


def strip_virt_specifiers(head: str) -> str:
    """`override` and `final` are declaration-only: a virt-specifier outside a
    class definition is an error.  They sit after the parameter list, so
    strip_defaults never sees them."""
    s = mask_operator(scrub(head))
    o = s.find('(')
    if o < 0:
        return head
    depth, i = 0, o
    while i < len(s):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    return head[:i] + re.sub(r'\s*\b(?:override|final)\b', '', head[i:])


def qualify(head: str, cls: str, nested: set[str], fn: str) -> str:
    """`T name(args)` -> `C::T C::name(args)` (return type qualified only when
    it names a nested type)."""
    s = mask_operator(scrub(head))
    # Locate the declarator-id: the identifier right before the parameter list.
    # Search the MASKED text — `operator=` ends in '=' and would not match as an
    # identifier — but slice the original, which mask_operator keeps the same
    # length as.
    o = s.find('(')
    m = None
    for m in re.finditer(r'[\w~]+\s*$', s[:o]):
        pass
    if not m:
        return head
    ret = head[:m.start()]
    name = head[m.start():o]
    # drop declaration-only specifiers
    ret = re.sub(r'(^|\s)(?:static|virtual|explicit)\s+', r'\1', ret)
    # `export` never appears on a member, but be safe
    ret = re.sub(r'(^|\s)export\s+', r'\1', ret)
    # qualify a nested return type
    ret_stripped = ret.strip()
    for t in nested:
        ret_stripped = re.sub(r'(?<![\w:])' + re.escape(t) + r'(?![\w:])',
                              f'{cls}::{t}', ret_stripped)
    lead_ws = ret[:len(ret) - len(ret.lstrip())]
    sep = ' ' if ret_stripped else ''
    return f'{lead_ws}{ret_stripped}{sep}{cls}::{name.strip()}' + head[o:]


def dedent(text: str, n: int) -> str:
    """Shift every line but the first left by up to n spaces.  A member body
    carries the class's indentation; left alone, 3,845 lines of out-of-line
    definitions come out indented one level too deep with their closing brace
    hanging in mid-air."""
    if n <= 0:
        return text
    head, sep, rest = text.partition('\n')
    if not sep:
        return text
    out = []
    for line in rest.split('\n'):
        k = 0
        while k < n and k < len(line) and line[k] == ' ':
            k += 1
        out.append(line[k:])
    return head + '\n' + '\n'.join(out)


def indent_of(lead: str, head: str) -> int:
    """How far the member declaration is indented inside the class."""
    m = re.search(r'[ \t]*$', lead.split('\n')[-1])
    base = len(m.group(0).expandtabs(4)) if m else 0
    if base:
        return base
    m = re.match(r'[ \t]*', head)
    return len(m.group(0).expandtabs(4))


def member_name(decl: str) -> str:
    """The declarator-id of a member function head."""
    m = mask_operator(scrub(decl))
    o = m.find('(')
    hits = list(re.finditer(r'[\w~]+\s*$', m[:o] if o >= 0 else m))
    return decl[hits[-1].start():o].strip() if hits else ''


def outline_class(cls_item: Item, ns: str, outer: str, out_impl: list,
                  stats: dict, guards: 'GuardStack', path: str) -> str:
    """Rewrite one class body; append (namespace, definition) pairs to
    out_impl.  Returns the rewritten class item text."""
    m = CLASS_HEAD.match(cls_item.head.rstrip())
    if not m:
        return cls_item.raw
    cname = m.group('name')
    if 'template' in cls_item.head:
        # a template class's members cannot be defined in another unit without
        # explicit instantiation
        return cls_item.raw
    body = cls_item.body[1:-1]
    nested = nested_type_names(body)
    # The definitions are emitted INSIDE the enclosing namespace, so the
    # declarator-id only needs the class path.  Qualifying it with the
    # namespace instead would leave the RETURN TYPE unqualified at global
    # scope, where the class's own name does not resolve.
    qual = f'{outer}::{cname}' if outer else cname
    pieces = []
    for it in scan_items(body):
        keep = True
        if it.kind == 'preproc':
            guards.feed(it.head)
        if it.kind == 'def' and it.body and CLASS_HEAD.match(it.head.rstrip()):
            pieces.append(outline_class(it, ns, qual, out_impl, stats,
                                        guards, path))
            continue
        if it.kind == 'def' and it.body and not TYPE_HEAD.match(it.head) \
                and not NS_HEAD.match(it.head):
            h = head_norm(it.head)
            decl, init = split_member_init(it.head)
            movable = (
                '(' in scrub(mask_operator(decl))
                and not CONSTEXPR.search(h)
                and 'template' not in h
                and not re.search(r'=\s*(?:default|delete)\s*;?\s*$', h)
                and not has_deduced_return(decl)
                and not has_auto_parameter(decl)
                and 'friend' not in h
                and '::' not in scrub(mask_operator(decl)).split('(')[0].split()[-1]
                and not ice_skipped(path, cname, member_name(decl))
                and (LIMIT[0] is None or stats['members'] < LIMIT[0])
            )
            if movable:
                # in-class: declaration only, default arguments kept
                pieces.append(it.lead + decl.rstrip() + ';')
                ind = indent_of(it.lead, it.head)
                out_impl.append((ns, guards.current(),
                    qualify(strip_virt_specifiers(strip_defaults(decl)),
                            qual, nested, cname).strip() + ' '
                    + dedent(init + it.body + it.tail, ind).lstrip(' ')))
                stats['members'] += 1
                stats['lines'] += it.body.count('\n')
                keep = False
        if keep:
            pieces.append(it.raw)
    return cls_item.lead + cls_item.head + '{' + ''.join(pieces) + '}' + cls_item.tail


def walk(items: list[Item], ns: str, out_impl: list, stats: dict,
         guards: 'GuardStack', path: str) -> str:
    parts = []
    for it in items:
        if it.kind == 'preproc':
            guards.feed(it.head)
        if it.kind == 'def' and it.body and NS_HEAD.match(it.head):
            nm = re.sub(r'^\s*(?:export\s+)?namespace\s*', '',
                        it.head).strip().rstrip('{').strip()
            sub = f'{ns}::{nm}' if ns and nm else (nm or ns)
            inner = walk(scan_items(it.body[1:-1]), sub, out_impl, stats,
                         guards, path)
            parts.append(it.lead + it.head + '{' + inner + '}' + it.tail)
        elif it.kind == 'def' and it.body and CLASS_HEAD.match(it.head.rstrip()):
            parts.append(outline_class(it, ns, '', out_impl, stats, guards, path))
        else:
            parts.append(it.raw)
    return ''.join(parts)


def group_by_namespace(out_impl: list) -> str:
    """Emit the definitions inside their own namespace block, in the order the
    classes appeared, so an unqualified return type still resolves — and inside
    the #if that guarded the class, so platform-only bodies stay
    platform-only."""
    chunks, key, buf = [], None, []
    for ns, guard, text in out_impl:
        if (ns, guard) != key:
            if buf:
                chunks.append((key, buf))
            key, buf = (ns, guard), []
        buf.append(text)
    if buf:
        chunks.append((key, buf))
    parts = []
    for (ns, guard), texts in chunks:
        body = '\n\n'.join(x.strip('\n') for x in texts)
        if ns:
            body = f'namespace {ns} {{\n\n{body}\n\n}} // namespace {ns}'
        if guard:
            body = f'#if {guard}\n\n{body}\n\n#endif'
        parts.append(body)
    return '\n\n'.join(parts)


BANNER = ('// \u2500\u2500 out-of-line class members '
          '\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500'
          '\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500'
          '\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500'
          '\u2500\u2500\u2500\u2500')

LIMIT = [None]      # bisection aid: outline at most this many members per file


def process(path: str, write: bool):
    cpp = os.path.splitext(path)[0] + '.cpp'
    src = open(path).read()
    m = re.search(r'^export module (?P<mod>[\w.]+)(?P<part>:\w+)?;', src, re.M)
    if not m:
        return 0, 0, f'{path}: no `export module`'
    head_end = m.end()
    im = re.match(
        r'\A((?:\s*(?:(?:export\s+)?import\s+[\w.:]+\s*;|//[^\n]*|/\*.*?\*/))*\s*)',
        src[head_end:], re.S)
    pre_len = head_end + (im.end(1) if im else 0)
    preamble, body = src[:pre_len], src[pre_len:]

    out_impl: list[str] = []
    stats = {'members': 0, 'lines': 0}
    new_body = walk(scan_items(body), '', out_impl, stats, GuardStack(), path)
    if not stats['members']:
        return 0, 0, f'{path}: no outlinable members'
    if write:
        open(path, 'w').write(preamble + new_body)
        # Re-running must replace the generated section, not stack another copy
        # on top of it.  Appending blind produced three definitions of
        # `TaskManager::TaskManager` in task.cpp across three regenerations --
        # and only in the files this tool creates itself, because those are the
        # ones split.py does not rewrite from scratch.
        if os.path.exists(cpp):
            prev = open(cpp).read()
            cut = prev.find(BANNER)
            if cut >= 0:
                open(cpp, 'w').write(prev[:cut].rstrip() + '\n')
        if not os.path.exists(cpp):
            # This module had nothing at namespace scope to move, so phase 1
            # produced no implementation unit.  Build its preamble the same way
            # split.py does: the global module fragment verbatim, `module M;`
            # for the primary (a partition's implementation belongs to the
            # primary module), and the interface's non-partition imports.
            gmf = src[:src.index('export module')]
            imports = [re.sub(r'^export\s+', '', x) for x in
                       re.findall(r'^(?:export\s+)?import\s+[\w.:]+\s*;',
                                  preamble, re.M)
                       if not re.match(r'^(?:export\s+)?import\s+:', x)]
            head = (gmf if gmf.strip() else '') + f'module {m.group("mod")};\n'
            if imports:
                head += '\n' + '\n'.join(imports) + '\n'
            open(cpp, 'w').write(head)
        with open(cpp, 'a') as fh:
            fh.write('\n\n' + BANNER + '\n\n')
            fh.write(group_by_namespace(out_impl) + '\n')
        # An outlined body can be the first thing in this unit to name `stdout`.
        # Read before opening for write: `open(w).write(open(r).read())` truncates
        # the file before the read runs, and every unit that took this path came
        # out empty -- which surfaces as undefined symbols at link time, not as a
        # compile error in the file that was emptied.
        text = open(cpp).read()
        open(cpp, 'w').write(ensure_cstdio(text))
    return stats['members'], stats['lines'], None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--limit', type=int, default=None,
                    help='outline at most N members per file (bisection aid)')
    a = ap.parse_args()
    LIMIT[0] = a.limit
    files = sorted(glob.glob('src/**/*.cppm', recursive=True)) if a.all else a.files
    tm = tl = tf = 0
    for f in files:
        n, ln, err = process(f, a.write)
        if err:
            print(f'SKIP {err}')
            continue
        tf += 1; tm += n; tl += ln
        print(f'{f}: {n} members, {ln} body lines outlined')
    print(f'\n{tf} files, {tm} members, {tl} body lines outlined')


if __name__ == '__main__':
    main()
