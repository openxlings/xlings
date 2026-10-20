#!/usr/bin/env bash
# Lint: the package layers import downward only (SubOS design part 3 §8).
#
#   luban/     a SubOS that is a machine     may import modules/, never src/
#   modules/   shared by xlings and Luban    may import neither luban/ nor src/
#   src/       the xlings frontend           may import modules/, never luban/
#
# Luban is one implementation over xlings: luban drives xlings, xlings never
# links Luban. What both read (boot.json, a root's /etc, image formats) is a
# module under modules/.
#
# A module name says where it lives: `luban.*` is under luban/, `xlings.core.*`
# (and the frontend's other top-level names) under src/. An import that points
# upward is a build that compiles today and cannot be split tomorrow -- the
# SubOS core reaching for the package manager is exactly what `Ports` exists
# to prevent, and nothing else would notice until the split.
# xtest: covers=LAYOUT-DEPS,LAYOUT-SPECS,LUBAN-ARCH-DEPS requires=
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

bad=0
report() { echo "::error file=$1::$2"; bad=$((bad + 1)); }

# `import x;` / `export import x;` at the start of a line, comments excluded.
imports() {
  grep -rnE --include='*.cppm' --include='*.cpp' '^[[:space:]]*(export[[:space:]]+)?import[[:space:]]+[A-Za-z0-9_.:]+' "$@" 2>/dev/null \
    | sed -E 's/^([^:]+):([0-9]+):[[:space:]]*(export[[:space:]]+)?import[[:space:]]+([A-Za-z0-9_.]+).*/\1:\2 \4/' || true
}

# The frontend's module roots: everything src/ exports.
frontend_roots=$(grep -rhoE --include='*.cppm' '^export module [A-Za-z_.]+' src 2>/dev/null \
                 | awk '{print $3}' | sort -u)
is_frontend() {
  local m="$1"
  case "$m" in xlings.core|xlings.core.*) return 0 ;; esac
  grep -qxF "$m" <<<"$frontend_roots"
}

while read -r where module; do
  [ -n "${module:-}" ] || continue
  case "$module" in luban|luban.*) report "${where%%:*}" "modules/ imports $module ($where): modules/ is shared and may not depend on luban/" ;; esac
  if is_frontend "$module"; then
    report "${where%%:*}" "modules/ imports the frontend module $module ($where): pass what is needed through Ports"
  fi
done < <(imports modules)

if [ -d luban ]; then
  while read -r where module; do
    [ -n "${module:-}" ] || continue
    if is_frontend "$module"; then
      report "${where%%:*}" "luban/ imports the frontend module $module ($where): luban/ depends on modules/ only"
    fi
  done < <(imports luban)
fi

while read -r where module; do
  [ -n "${module:-}" ] || continue
  case "$module" in luban|luban.*) report "${where%%:*}" "src/ imports $module ($where): xlings never links Luban; move what both need to modules/" ;; esac
done < <(imports src)

# luban/ holds four kinds of package (Luban OS part 4 §4; design 2026-10-19
# §28): specs/, kits/ and xdesk/ are independent projects kept here until they
# move to their own repositories, os/ is Luban OS itself.
#
#   luban/specs/<name>  a specification: the standard library and its own
#                       modules (<name>, <name>.*). Specifications never import
#                       one another; implementations connect them.
#   luban/kits/<name>   an implementation or a reusable component: the standard
#                       library, specifications, other kits and their C
#                       libraries -- never xlings or Luban.
#   luban/xdesk/<part>  the xdesk desktop: as a kit -- never xlings or Luban.
#   luban/os/service    the OS depends on no presentation: not the Luban OS
#                       app's library (luban.desktop.*), not the desktop (xdesk.*).
spec_roots=""
if [ -d luban/specs ]; then
  for d in luban/specs/*/; do spec_roots="$spec_roots $(basename "$d")"; done
  for d in luban/specs/*/; do
    own="$(basename "$d")"
    while read -r where module; do
      [ -z "${module:-}" ] && continue
      case "$module" in std|std.*|"$own"|"$own".*) continue ;; esac
      report "${where%%:*}" "the specification $own imports $module ($where): a specification imports the standard library and itself only"
    done < <(imports "$d")
  done
fi
is_spec() { local m="${1%%.*}" r; for r in $spec_roots; do [ "$m" = "$r" ] && return 0; done; return 1; }
for project in luban/kits luban/xdesk; do
  [ -d "$project" ] || continue
  while read -r where module; do
    [ -z "${module:-}" ] && continue
    case "$module" in
      std|std.*|xdesk|xdesk.*) continue ;;
      # Their C libraries' modules (mcpp packages: wlroots, freedesktop.*, compat.*).
      wlroots|wlroots.*|freedesktop.*|compat.*) continue ;;
    esac
    is_spec "$module" && continue
    case "$module" in
      xlings|xlings.*|luban|luban.*) report "${where%%:*}" "$project imports $module ($where): it is reusable on its own -- the standard library, specifications, kits and their C libraries, never xlings or Luban" ;;
    esac
  done < <(imports "$project")
done
if [ -d luban/os/service ]; then
  while read -r where module; do
    case "${module:-}" in
      luban.desktop|luban.desktop.*|xdesk|xdesk.*) report "${where%%:*}" "luban/os/service imports $module ($where): the OS depends on no presentation" ;;
    esac
  done < <(imports luban/os/service)
fi

# What of xlings each Luban package may import: luban/DEPENDENCIES.toml
# (design 2026-10-20 WuXian §16.2) -- its layers, base and shared; xlings'
# domain only where [migrating] lists it, and nowhere new.
if [ -f luban/DEPENDENCIES.toml ]; then
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    report "${line%%::*}" "${line#*::}"
  done < <(imports luban | python3 -c '
import fnmatch, sys, tomllib
table = tomllib.load(open("luban/DEPENDENCIES.toml", "rb"))
layers, packages = table["layers"], table["packages"]
migrating = {m["module"]: m for m in table.get("migrating", [])}
def matches(module, patterns):
    return any(module == p or (p.endswith(".*") and (module == p[:-2] or module.startswith(p[:-1]))) for p in patterns)
for row in sys.stdin:
    parts = row.split()
    if len(parts) != 2:
        continue
    where, module = parts
    if not module.startswith("xlings"):
        continue
    path = where.split(":")[0]
    owners = [d for d in packages if path.startswith(d + "/")]
    if not owners:
        continue
    package = max(owners, key=len)
    if any(matches(module, layers[l]) for l in packages[package]):
        continue
    m = migrating.get(module)
    if m and any(path.startswith(w + "/") for w in m["where"]):
        continue
    if m:
        print(f"{path}::{package} imports {module} ({where}): it is being moved behind {m["to"]}; no new use outside {", ".join(m["where"])}")
    else:
        print(f"{path}::{package} imports {module} ({where}), which luban/DEPENDENCIES.toml does not allow there: a base or shared module only, the xlings domain through a Port capability")
')
fi

if [ "$bad" -gt 0 ]; then
  echo "layer dependency lint: FAIL ($bad import(s) across a boundary)"
  exit 1
fi
echo "layer dependency lint: PASS (luban -> modules, modules -/-> luban, src; src -/-> luban; specs -> std and themselves; kits, xdesk -/-> xlings, luban; the OS service -/-> presentation; Luban's xlings imports as luban/DEPENDENCIES.toml allows)"
