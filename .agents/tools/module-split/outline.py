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
                   CONSTEXPR, TYPE_HEAD, NS_HEAD)

CLASS_HEAD = re.compile(
    r'^\s*(?:export\s+)?(?P<kw>struct|class)\s+(?P<name>\w+)'
    r'(?P<rest>[^;{]*)$')


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


def qualify(head: str, cls: str, nested: set[str], fn: str) -> str:
    """`T name(args)` -> `C::T C::name(args)` (return type qualified only when
    it names a nested type)."""
    s = mask_operator(scrub(head))
    # locate the declarator-id: the identifier right before the parameter list
    o = s.find('(')
    pre = head[:o]
    m = None
    for m in re.finditer(r'[\w~]+\s*$', pre):
        pass
    if not m:
        return head
    ret = pre[:m.start()]
    name = pre[m.start():]
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


def outline_class(cls_item: Item, ns: str, out_impl: list, stats: dict) -> str:
    """Rewrite one class body; append out-of-line definitions to out_impl.
    Returns the rewritten class item text."""
    m = CLASS_HEAD.match(cls_item.head.rstrip())
    if not m:
        return cls_item.raw
    cname = m.group('name')
    if ':' in m.group('rest') or 'template' in cls_item.head:
        # a base-class list is fine, but a template class is not: its members
        # cannot be defined in another unit without explicit instantiation
        if 'template' in cls_item.head:
            return cls_item.raw
    body = cls_item.body[1:-1]
    nested = nested_type_names(body)
    qual = f'{ns}::{cname}' if ns else cname
    pieces = []
    for it in scan_items(body):
        keep = True
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
            )
            if movable:
                # in-class: declaration only, default arguments kept
                pieces.append(it.lead + decl.rstrip() + ';')
                out_impl.append(
                    qualify(strip_defaults(decl), qual, nested, cname).strip()
                    + init + it.body + it.tail)
                stats['members'] += 1
                stats['lines'] += it.body.count('\n')
                keep = False
        if keep:
            pieces.append(it.raw)
    return cls_item.lead + cls_item.head + '{' + ''.join(pieces) + '}' + cls_item.tail


def walk(items: list[Item], ns: str, out_impl: list, stats: dict) -> str:
    parts = []
    for it in items:
        if it.kind == 'def' and it.body and NS_HEAD.match(it.head):
            nm = re.sub(r'^\s*(?:export\s+)?namespace\s*', '',
                        it.head).strip().rstrip('{').strip()
            sub = f'{ns}::{nm}' if ns and nm else (nm or ns)
            inner = walk(scan_items(it.body[1:-1]), sub, out_impl, stats)
            parts.append(it.lead + it.head + '{' + inner + '}' + it.tail)
        elif it.kind == 'def' and it.body and CLASS_HEAD.match(it.head.rstrip()):
            parts.append(outline_class(it, ns, out_impl, stats))
        else:
            parts.append(it.raw)
    return ''.join(parts)


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
    new_body = walk(scan_items(body), '', out_impl, stats)
    if not stats['members']:
        return 0, 0, f'{path}: no outlinable members'
    if write:
        open(path, 'w').write(preamble + new_body)
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
            fh.write('\n\n// ── out-of-line class members '
                     '─────────────────────────────────\n\n')
            fh.write('\n\n'.join(x.strip('\n') for x in out_impl) + '\n')
    return stats['members'], stats['lines'], None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()
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
