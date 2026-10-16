#!/usr/bin/env bash
# A bare distribution made into a user's machine before xlings: what
# quick_install needs (curl, tar, gzip, certificates), sudo (the one-time
# setup asks for it), and the tools that make a user (useradd, runuser).
# Nothing else: what the scenarios need beyond it comes from xlings (python:
# run.sh installs it). The aarch64 design §3.4.
set -euo pipefail
as_root() { if [[ $(id -u) -eq 0 ]]; then "$@"; else sudo "$@"; fi; }
if command -v apt-get >/dev/null; then
    as_root env DEBIAN_FRONTEND=noninteractive apt-get -qq update >/dev/null
    as_root env DEBIAN_FRONTEND=noninteractive apt-get -qq install -y curl ca-certificates sudo util-linux passwd tar gzip procps >/dev/null  # host-dep-ok: a bare distribution, prepared to install xlings
elif command -v dnf >/dev/null; then
    as_root dnf -y -q install --allowerasing curl sudo util-linux shadow-utils tar gzip findutils procps-ng >/dev/null  # host-dep-ok: a bare distribution, prepared to install xlings
elif command -v apk >/dev/null; then
    as_root apk add -q bash curl ca-certificates sudo util-linux shadow tar gzip findutils procps >/dev/null  # host-dep-ok: a bare distribution, prepared to install xlings
elif command -v pacman >/dev/null; then
    as_root pacman -Sy --noconfirm --needed curl sudo util-linux tar gzip findutils procps-ng >/dev/null  # host-dep-ok: a bare distribution, prepared to install xlings
elif command -v zypper >/dev/null; then
    as_root zypper -n -q install curl sudo util-linux shadow tar gzip findutils procps >/dev/null  # host-dep-ok: a bare distribution, prepared to install xlings
else
    echo "bare.sh: no package manager this script knows" >&2; exit 2
fi
