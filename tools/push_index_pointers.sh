#!/usr/bin/env bash
# Assemble the COMBINED index pointer (xim-index-pointers.json) from the per-index
# manifests in <dir> and push it into xlings-res/xim-index on GitHub and GitCode
# as a REPO FILE (served raw: raw.githubusercontent.com/.../main/<f> and
# raw.gitcode.com/.../raw/main/<f>), updated by git push.
#
# Why one combined file (not per-index): the client then needs ONE raw fetch for
# all indexes (main + subs), avoiding gitcode raw rate-limiting. Why a repo file
# (not a release asset): gitcode release assets can't be overwritten (gtc) or
# deleted (API 405); a repo file IS overwriteable via git push. Artifacts stay
# release assets (version-unique names, create-only).
#
# Usage: tools/push_index_pointers.sh <dir-with-*-latest.json>
# Auth:  XLINGS_RES_TOKEN (github), GITCODE_TOKEN (gitcode)
# Repos: GH_REPO_SLUG / GTC_REPO_SLUG (default xlings-res/xim-index)
set -euo pipefail

DIR="${1:?usage: push_index_pointers.sh <dir>}"
GH_REPO_SLUG="${GH_REPO_SLUG:-xlings-res/xim-index}"
GTC_REPO_SLUG="${GTC_REPO_SLUG:-xlings-res/xim-index}"

compgen -G "$DIR/*-latest.json" >/dev/null 2>&1 || { echo "[pointers] no *-latest.json in $DIR" >&2; exit 1; }

# Assemble combined pointer: filename xim-index-latest.json -> key "xim";
# xim-index-<name>-latest.json -> key "<name>".
COMBINED="$DIR/xim-index-pointers.json"
python3 - "$DIR" "$COMBINED" <<'PY'
import sys, json, glob, os, re
d, out = sys.argv[1], sys.argv[2]
indexes = {}
for f in sorted(glob.glob(os.path.join(d, "*-latest.json"))):
    name = os.path.basename(f)
    if name == "xim-index-pointers.json": continue
    m = re.match(r'xim-index-(?:(.+)-)?latest\.json$', name)
    if not m: continue
    key = m.group(1) or "xim"
    try: indexes[key] = json.load(open(f, encoding="utf-8"))
    except Exception as e: print(f"[pointers] skip {name}: {e}", file=sys.stderr)
json.dump({"format_version": 1, "indexes": indexes}, open(out, "w", encoding="utf-8"), indent=2)
print(f"[pointers] combined {len(indexes)} index(es): {', '.join(sorted(indexes))}")
PY

push_one() {  # <clone-url> <label>
  local url="$1" label="$2" tmp
  tmp="$(mktemp -d)"
  if ! git clone -q --depth 1 "$url" "$tmp" 2>/dev/null; then
    echo "[pointers] $label: clone failed (skip)"; rm -rf "$tmp"; return 0
  fi
  # Clean legacy per-index pointers + any probe files; keep only the combined one.
  rm -f "$tmp"/xim-index-*latest.json "$tmp"/ptest.* "$tmp"/ptestnoext "$tmp"/pov.json 2>/dev/null || true
  # MERGE our (re)built keys INTO the existing combined pointer, preserving
  # sibling indexes' keys. Each index repo (xim, awesome, scode, d2x) publishes
  # independently and must update ONLY its own key — a plain overwrite would drop
  # the others (regression seen when xim-pkgindex's per-repo CI published alone).
  python3 - "$tmp/xim-index-pointers.json" "$COMBINED" <<'PYMERGE'
import sys, json, os
dst, src = sys.argv[1], sys.argv[2]
base = {"format_version": 1, "indexes": {}}
if os.path.exists(dst):
    try: base = json.load(open(dst, encoding="utf-8"))
    except Exception: base = {"format_version": 1, "indexes": {}}
base.setdefault("indexes", {})
base["indexes"].update(json.load(open(src, encoding="utf-8")).get("indexes", {}))
base["format_version"] = 1
json.dump(base, open(dst, "w", encoding="utf-8"), indent=2)
PYMERGE
  git -C "$tmp" add -A
  if git -C "$tmp" diff --cached --quiet; then
    echo "[pointers] $label: no change"; rm -rf "$tmp"; return 0
  fi
  git -C "$tmp" -c user.email=ci@xlings.dev -c user.name=xlings-ci \
      commit -qm "chore: update combined index pointer"
  if git -C "$tmp" push -q origin HEAD:main 2>/dev/null; then
    echo "[pointers] $label: pushed"
  else
    echo "[pointers] $label: push failed (non-blocking)"
  fi
  rm -rf "$tmp"
}

if [[ -n "${XLINGS_RES_TOKEN:-}" ]]; then
  push_one "https://x-access-token:${XLINGS_RES_TOKEN}@github.com/${GH_REPO_SLUG}.git" "github"
else
  echo "[pointers] XLINGS_RES_TOKEN unset; skipping github"
fi
if [[ -n "${GITCODE_TOKEN:-}" ]]; then
  push_one "https://oauth2:${GITCODE_TOKEN}@gitcode.com/${GTC_REPO_SLUG}.git" "gitcode"
else
  echo "[pointers] GITCODE_TOKEN unset; skipping gitcode"
fi
