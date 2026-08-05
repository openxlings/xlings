#!/usr/bin/env bash
# §6.4 的同源断言,先做成可执行判据(A1/A2/A5 用它)
# 对一个已安装的 ELF:INTERP 所在的 payload 目录,必须等于 RPATH 里同一 provider 的目录。
set -uo pipefail
BIN="${1:?usage: samesource.sh <elf>}"
interp=$(readelf -p .interp "$BIN" 2>/dev/null | grep -oE '/[^ ]+')
[[ -n "$interp" ]] || { echo "SKIP  no INTERP: $BIN"; exit 0; }
# payload 根 = .../xpkgs/<store>/<version>
root_of(){ sed -E 's#(.*/xpkgs/[^/]+/[^/]+)/.*#\1#' <<<"$1"; }
iroot=$(root_of "$interp")
provider=$(sed -E 's#.*/xpkgs/([^/]+)/([^/]+)$#\1#' <<<"$iroot")
rpath=$(objdump -p "$BIN" 2>/dev/null | grep -E 'RUNPATH|RPATH' | sed 's#.*PATH *##' | head -1)
same=""; other=""
IFS=: read -ra parts <<<"$rpath"
for p in "${parts[@]}"; do
  case "$p" in *"/xpkgs/$provider/"*)
    r=$(root_of "$p")
    [[ "$r" == "$iroot" ]] && same="$r" || other="$r" ;;
  esac
done
if [[ -n "$other" && -z "$same" ]]; then
  echo "FAIL  ${BIN##*/}"
  echo "        INTERP → $iroot"
  echo "        RPATH  → $other      ← 同一 provider,不同 payload"
  exit 1
fi
echo "OK    ${BIN##*/}  ($provider $(basename "$iroot"))"
