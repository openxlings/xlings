#!/usr/bin/env bash
# CI's tools come from xlings, not from the runner's package manager (the
# aarch64 design §3.4, HOST-DEPS-T0): the published xlings installs them into
# a home of their own, and each program is linked into /usr/local/bin -- the
# path a machine's copy has in xlings' tool table, so a test home finds it as
# any user's machine would offer it.
#
#   tools/ci/xlings_tools.sh <package>...
#
# XLINGS_TOOLS_HOME     the tools' home (default $RUNNER_TEMP/xlings-tools)
# XLINGS_TOOLS_INDEX_REF  an xim-pkgindex branch to install from (a PR's,
#                       before it is published); empty: the published index
# Exports XLINGS_TOOLS_HOME to later steps ($GITHUB_ENV). bwrap is copied, not
# linked: the copy is root's and not setuid, as a distribution's is.
set -euo pipefail
home="${XLINGS_TOOLS_HOME:-${RUNNER_TEMP:-/tmp}/xlings-tools}"
export XLINGS_HOME="$home" XLINGS_NON_INTERACTIVE=1
X="$home/bin/xlings"
if [[ ! -x "$X" ]]; then
  arch="$(uname -m)"
  tag="$(curl -fsSL https://api.github.com/repos/openxlings/xlings/releases/latest | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)"
  v="${tag#v}"
  w="$(mktemp -d)"
  curl -fsSL --retry 3 -o "$w/x.tar.gz" "https://github.com/openxlings/xlings/releases/download/$tag/xlings-$v-linux-$arch.tar.gz"
  curl -fsSL --retry 3 -o "$w/x.tar.gz.sha256" "https://github.com/openxlings/xlings/releases/download/$tag/xlings-$v-linux-$arch.tar.gz.sha256"
  [[ "$(sha256sum "$w/x.tar.gz" | cut -d' ' -f1)" == "$(cut -d' ' -f1 "$w/x.tar.gz.sha256")" ]] || { echo "xlings $v: digest mismatch" >&2; exit 1; }
  tar -xzf "$w/x.tar.gz" -C "$w"
  (cd "$w"/xlings-*/ && ./bin/xlings self install </dev/null >/dev/null)
  if [[ -n "${XLINGS_TOOLS_INDEX_REF:-}" ]]; then
    git clone -q --depth 1 --branch "$XLINGS_TOOLS_INDEX_REF" https://github.com/openxlings/xim-pkgindex.git "$home/index-under-test"
    python3 - "$home/.xlings.json" "$home/index-under-test" <<'PY2'
import json, sys
cfg = json.load(open(sys.argv[1]))
cfg["index_repos"] = [{"name": "xim", "url": sys.argv[2]}]
json.dump(cfg, open(sys.argv[1], "w"), indent=2)
PY2
  fi
fi
for pkg in "$@"; do
  "$X" install -y "xim:$pkg" </dev/null >"$home/install-$pkg.log" 2>&1 || { tail -30 "$home/install-$pkg.log"; echo "xlings install $pkg failed" >&2; exit 1; }
  for dir in "$home"/data/xpkgs/xim-x-"$pkg"/*/bin "$home"/data/xpkgs/xim-x-"$pkg"/*/sbin; do
    [[ -d "$dir" ]] || continue
    for prog in "$dir"/*; do
      [[ -f "$prog" && -x "$prog" ]] || continue
      name="$(basename "$prog")"
      if [[ "$name" == bwrap ]]; then
        sudo install -o root -g root -m 0755 "$prog" "/usr/local/bin/$name"
      else
        sudo ln -sf "$prog" "/usr/local/bin/$name"
      fi
    done
  done
  echo "xlings tools: $pkg ($(ls -d "$home"/data/xpkgs/xim-x-"$pkg"/* | xargs -n1 basename | tr '\n' ' '))"
done
[[ -n "${GITHUB_ENV:-}" ]] && echo "XLINGS_TOOLS_HOME=$home" >> "$GITHUB_ENV"
exit 0
