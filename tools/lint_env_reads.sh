#!/usr/bin/env bash
# Lint: the Luban OS app reads a system only through the service.
#
# A system's root is not a directory the window can walk: its /usr is a mount
# point on the host and its links name paths this host may not have. What the
# app shows of a system -- its desktops, applications, settings, os-release --
# comes from luban.service (describe, Environment, enter_plan), which reads
# where the links lead (xlings.subos.located). The settings page once walked
# usr/share/wayland-sessions itself and showed no desktop (design 2026-10-18
# §10, E11). A line in luban/host/luban-os that takes a system's /usr or root
# (usr_of, root_of) needs an `env-read-ok: <why>` marker.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MARK='env-read-ok:'
bad=0
while IFS= read -r hit; do
  case "$hit" in *"$MARK"*) ;; *)
    echo "::error::the Luban OS app reads a system's tree itself (ask luban.service): $hit"
    bad=$((bad + 1)) ;;
  esac
done < <(grep -rnE '\b(usr_of|root_of)\(' luban/host/luban-os/src || true)

if [ "$bad" -gt 0 ]; then
  echo "env reads lint: $bad line(s) in luban/host/luban-os read a system's tree; use luban.service, or mark $MARK <why>"
  exit 1
fi
echo "env reads lint: PASS (the Luban OS app reads systems through the service)"
