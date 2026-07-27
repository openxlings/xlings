#!/usr/bin/env python3
"""Flag index recipes that provider-scoped registration would now reject.

Registration validates a package's whole set of xvm operations as one batch
and fails closed. Three of those checks turn recipe defects that used to pass
silently into install failures:

  SelfBinding      an entry bound to itself
  RootNotInBatch   a binding naming a target the recipe never registers
  DuplicateNode    the same target and version registered twice

A recipe hitting any of them cannot be installed at all, so the index has to
be clean before the release ships. This is a static scan -- it reads the
xvm.add / xvm.setup calls rather than executing the hooks, so it cannot see
names built at runtime. Those are reported separately as unresolved rather
than silently treated as clean: an audit that quietly skips what it cannot
parse reads as "nothing to fix" when it means "did not look".

Usage:  pkgindex_registration_audit.py <index-dir>
Exit:   0 clean, 1 findings, 2 usage error
"""
import re
import sys
from pathlib import Path

# xvm.add("name", { ... })  /  xvm.add('name')
ADD = re.compile(r"""xvm\.add\s*\(\s*(["'])(?P<name>[^"']+)\1(?P<rest>.*?)\)""", re.S)
# xvm.setup("name", { ... })
SETUP = re.compile(r"""xvm\.setup\s*\(\s*(["'])(?P<name>[^"']+)\1""")
BINDING = re.compile(r"""binding\s*=\s*(["'])(?P<value>[^"']+)\1""")
# A binding whose target is built at runtime: binding = name .. "@" .. version
BINDING_DYNAMIC = re.compile(r"""binding\s*=\s*[^"',}]*\.\.""")


def audit_recipe(path: Path):
    """Return (findings, unresolved) for one recipe."""
    text = path.read_text(encoding="utf-8", errors="replace")
    findings, unresolved = [], []

    # xvm.setup builds its own consistent batch (root + members bound to it),
    # so it cannot produce these defects. Recipes using it need no scan.
    if SETUP.search(text):
        return findings, unresolved

    registered, first_seen = [], {}
    for match in ADD.finditer(text):
        name = match.group("name")
        rest = match.group("rest")
        if name in first_seen:
            # Recipes routinely register one name differently per platform:
            #   if is_host("windows") then xvm.add("git", ...) else xvm.add("git", ...) end
            # Only one branch runs, so that is not a duplicate. Treating it as
            # one would fill the report with noise and train people to ignore
            # it. Without evaluating the Lua we cannot prove exclusivity, so
            # an else/elseif between the two occurrences downgrades this to
            # unresolved rather than clearing or rejecting it.
            between = text[first_seen[name]:match.start()]
            if re.search(r"\b(else|elseif)\b", between):
                unresolved.append(
                    f"'{name}' registered twice across an if/else — "
                    f"exclusive branches, or a real duplicate?")
            else:
                findings.append(("DuplicateNode", f"'{name}' registered twice"))
        else:
            first_seen[name] = match.end()

        binding = BINDING.search(rest)
        if binding:
            target = binding.group("value").split("@", 1)[0]
            registered.append((name, target))
        elif BINDING_DYNAMIC.search(rest):
            unresolved.append(f"'{name}' binds to a runtime-built target")
        else:
            registered.append((name, None))

    names = {name for name, _ in registered}
    for name, target in registered:
        if target is None:
            continue
        if target == name:
            findings.append(("SelfBinding", f"'{name}' binds to itself"))
        elif target not in names:
            findings.append(
                ("RootNotInBatch",
                 f"'{name}' binds to '{target}', which the recipe never registers"))
    return findings, unresolved


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(sys.argv[1]) / "pkgs"
    if not root.is_dir():
        print(f"FATAL: no pkgs/ under {sys.argv[1]}", file=sys.stderr)
        return 2

    recipes = sorted(root.rglob("*.lua"))
    rejected, unresolved_total, with_xvm = 0, 0, 0

    for recipe in recipes:
        text = recipe.read_text(encoding="utf-8", errors="replace")
        if "xvm.add" not in text and "xvm.setup" not in text:
            continue
        with_xvm += 1
        findings, unresolved = audit_recipe(recipe)
        rel = recipe.relative_to(root.parent)
        for kind, detail in findings:
            print(f"REJECT  {rel}: {kind} — {detail}")
            rejected += 1
        for detail in unresolved:
            print(f"unresolved  {rel}: {detail}")
            unresolved_total += 1

    print()
    print(f"scanned      {len(recipes)} recipes, {with_xvm} register with xvm")
    print(f"rejected     {rejected}")
    print(f"unresolved   {unresolved_total}  (not provably clean by a static "
          f"scan — read each one before treating the index as clean)")
    return 1 if rejected else 0


if __name__ == "__main__":
    sys.exit(main())
