#!/usr/bin/env python3
"""Size the interface/implementation split: how much of each .cppm is a
namespace-scope function body (movable) vs inside a class body (needs
out-of-line member syntax) vs must-stay (template/constexpr/type)."""
import re, glob, sys, collections

def strip_for_scan(s):
    """Blank out string/char literals and comments so brace matching is sane.
    Keeps byte offsets identical."""
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
        elif c in '"\'':
            q = c; j = i + 1
            # raw strings R"(...)"
            if q == '"' and i > 0 and s[i-1] == 'R':
                m = re.match(r'"([^(]*)\(', s[i:])
                if m:
                    delim = m.group(1)
                    end = s.find(')' + delim + '"', i)
                    j = n if end < 0 else end + len(delim) + 2
                    for k in range(i, j):
                        if s[k] != '\n': out[k] = ' '
                    i = j; continue
            while j < n:
                if s[j] == '\\': j += 2; continue
                if s[j] == q: j += 1; break
                if s[j] == '\n': break
                j += 1
            for k in range(i, j):
                if s[k] != '\n': out[k] = ' '
            i = j
        else:
            i += 1
    return ''.join(out)

tot = collections.Counter()
per_file = []
for f in sorted(glob.glob('src/**/*.cppm', recursive=True)):
    src = open(f).read()
    scan = strip_for_scan(src)
    lines = src.count('\n')
    # find class/struct/union bodies at any depth: `struct X ... {` ... matching `}`
    cls_lines = 0
    for m in re.finditer(r'\b(?:struct|class|union)\s+(\w+)[^;{]*\{', scan):
        # skip forward declarations (handled by the `{` requirement)
        start = scan.index('{', m.start())
        depth, i = 0, start
        while i < len(scan):
            if scan[i] == '{': depth += 1
            elif scan[i] == '}':
                depth -= 1
                if depth == 0: break
            i += 1
        cls_lines += src.count('\n', start, i)
    tot['lines'] += lines
    tot['class_body_lines'] += cls_lines
    per_file.append((lines, cls_lines, f))

print(f"total .cppm lines      : {tot['lines']}")
print(f"inside class/struct    : {tot['class_body_lines']}  "
      f"({100*tot['class_body_lines']/tot['lines']:.1f}%)")
print(f"outside class/struct   : {tot['lines']-tot['class_body_lines']}  "
      f"({100*(tot['lines']-tot['class_body_lines'])/tot['lines']:.1f}%)")
print()
print("files with the most class-body code:")
per_file.sort(key=lambda r: -r[1])
for lines, cls, f in per_file[:12]:
    print(f"  {cls:5}/{lines:5} ({100*cls/max(lines,1):4.0f}%)  {f}")

# namespace-scope `static` free functions (internal linkage -> cannot be
# declared in one unit and defined in another)
print()
stat_fns = []
for f in sorted(glob.glob('src/**/*.cppm', recursive=True)):
    scan = strip_for_scan(open(f).read())
    for m in re.finditer(r'^([ \t]*)static\s+(?!.*\b(?:constexpr|inline)\b)([\w:<>,& *]+?)\s+(\w+)\s*\([^;]*?\)\s*(?:const\s*)?\{', scan, re.M):
        stat_fns.append((f, m.group(3), len(m.group(1))))
print(f"namespace/class-scope `static ... (...) {{` definitions: {len(stat_fns)}")
for f, name, ind in stat_fns[:15]:
    print(f"  indent={ind:2} {name:28} {f}")
