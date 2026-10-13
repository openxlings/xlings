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
# xtest: covers=LAYOUT-DEPS requires=
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

bad=0
report() { echo "::error file=$1::$2"; bad=$((bad + 1)); }

# `import x;` / `export import x;` at the start of a line, comments excluded.
imports() {
  grep -rnE --include='*.cppm' --include='*.cpp' '^[[:space:]]*(export[[:space:]]+)?import[[:space:]]+[A-Za-z_.:]+' "$@" 2>/dev/null \
    | sed -E 's/^([^:]+):([0-9]+):[[:space:]]*(export[[:space:]]+)?import[[:space:]]+([A-Za-z_.]+).*/\1:\2 \4/' || true
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

# The desktop component families (Luban OS desktop part 2 §2.0): opensal and
# xdesk are used by other systems on their own, so they import the standard
# library, each other and their C libraries -- never xlings or Luban.
for family in luban/desktop/opensal luban/desktop/xdesk; do
  [ -d "$family" ] || continue
  while read -r where module; do
    [ -z "$module" ] && continue
    case "$module" in
      std|std.*|opensal|opensal.*|xdesk|xdesk.*) ;;
      # Their C libraries' modules (mcpp packages: wlroots, freedesktop.*, compat.*).
      wlroots|wlroots.*|freedesktop.*|compat.*) ;;
      *) report "${where%%:*}" "$family imports $module ($where): opensal and xdesk are reusable on their own -- the standard library, each other and their C libraries, never xlings or Luban" ;;
    esac
  done < <(imports "$family")
done

if [ "$bad" -gt 0 ]; then
  echo "layer dependency lint: FAIL ($bad import(s) across a boundary)"
  exit 1
fi
echo "layer dependency lint: PASS (luban -> modules, modules -/-> luban, src; src -/-> luban; opensal, xdesk -> std, each other, their C libraries)"
