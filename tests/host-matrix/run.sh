#!/usr/bin/env bash
# The host matrix on THIS machine (Luban OS design part 2 §5).
#
#   tests/host-matrix/run.sh --tarball <xlings release .tar.gz> --evidence <dir>
#                            [--index <xim-pkgindex checkout>] [--scenarios "a b"] [--mirror GLOBAL|CN]
#
# Runs as the machine's administrator (root, or a user sudo lets through).
# It makes an ORDINARY user -- no sudo at all, which is what a user's
# machine and an agent have -- installs the build under test for that user
# the way quick_install does (`self install` from the release tarball), and
# runs each scenario as that user. A scenario that needs the one-time
# isolation setup exits 3 having recorded the command it was told; the
# administrator runs it here and the scenario runs again. The evidence (one
# NDJSON per scenario, host.json, summary.md) goes to --evidence.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tarball="" evidence="" index="" mirror="${HM_MIRROR:-GLOBAL}"
scenarios="install doctor presets proxy edition agent upgrade terminal"
user="${HM_USER:-hm}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tarball) tarball="$(realpath "$2")"; shift 2 ;;
        --evidence) evidence="$2"; shift 2 ;;
        --index) index="$(realpath "$2")"; shift 2 ;;
        --scenarios) scenarios="$2"; shift 2 ;;
        --mirror) mirror="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -f "$tarball" && -n "$evidence" ]] || { echo "usage: run.sh --tarball <file> --evidence <dir> [--index <dir>]" >&2; exit 2; }
mkdir -p "$evidence"; evidence="$(realpath "$evidence")"

as_root() { if [[ $EUID -eq 0 ]]; then "$@"; else sudo "$@"; fi; }
userhome="/home/$user"
work="$userhome/hm"
as_user() {   # env assignments, then the command
    as_root runuser -u "$user" -- env -i HOME="$userhome" USER="$user" LOGNAME="$user" LANG=C.UTF-8 TERM=dumb \
        PATH="$userhome/.xlings/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" "$@"
}

echo "== host matrix: $(. /etc/os-release; echo "$PRETTY_NAME"), kernel $(uname -r), user $user (no sudo)"
id "$user" >/dev/null 2>&1 || as_root useradd -m -s /bin/bash "$user"
for g in sudo wheel admin; do as_root gpasswd -d "$user" "$g" >/dev/null 2>&1 || true; done
as_root rm -rf "$work"
as_root mkdir -p "$work/state" "$work/evidence" "$work/xlings"
as_root cp -r "$here" "$work/suite"
as_root cp "$tarball" "$work/xlings.tar.gz"
[[ -n "$index" ]] && as_root cp -r "$index" "$work/index"
as_root chown -R "$user:" "$work"
if as_user sudo -n true >/dev/null 2>&1; then echo "$user can sudo: not a user's machine" >&2; exit 2; fi

# The build under test, as quick_install installs it.
as_user bash -c "cd '$work/xlings' && tar -xzf ../xlings.tar.gz && cd */ && ./bin/xlings self install </dev/null" \
    > "$evidence/install.log" 2>&1 || { tail -30 "$evidence/install.log"; echo "self install failed" >&2; exit 1; }
as_user "$userhome/.xlings/bin/xlings" config --mirror "$mirror" >/dev/null
if [[ -n "$index" ]]; then
    # The index under test (a PR's checkout) instead of the published one.
    as_user python3 -c '
import json, sys
p = sys.argv[1]
cfg = json.load(open(p))
cfg["index_repos"] = [{"name": "xim", "url": sys.argv[2]}]
json.dump(cfg, open(p, "w"), indent=2)
' "$userhome/.xlings/.xlings.json" "$work/index"
fi
as_user env HM_EVIDENCE="$work/evidence" HM_STATE="$work/state" bash -c "source '$work/suite/lib.sh'; host_facts"

declare -A result
setup_done=""
for s in $scenarios; do
    echo "== $s"
    attempt() {
        as_user env HM_EVIDENCE="$work/evidence" HM_STATE="$work/state" ${setup_done:+HM_SETUP_DONE=1} \
            bash "$work/suite/scenarios/$s.sh"
    }
    set +e; attempt; rc=$?; set -e
    if [[ $rc -eq 3 ]]; then
        request="$(cat "$work/state/admin-request" 2>/dev/null || true)"
        echo "   one-time setup it asked for: $request"
        # As on a user's machine: the user runs the command, and it asks for
        # administrator rights through sudo (xlings.subos.elevation). The
        # user may use sudo for this step only -- the AppArmor restriction
        # does not apply to root, so the administrator running it would fix
        # nothing.
        as_root sh -c "echo '$user ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/hm-setup && chmod 440 /etc/sudoers.d/hm-setup"
        set +e
        as_user XLINGS_NON_INTERACTIVE=1 "$userhome/.xlings/bin/xlings" self doctor --isolation --fix -y \
            > "$evidence/setup.log" 2>&1
        setup_rc=$?
        set -e
        as_root rm -f /etc/sudoers.d/hm-setup "$work/state/admin-request"
        [[ $setup_rc -eq 0 ]] || { cat "$evidence/setup.log"; result[$s]=fail; continue; }
        setup_done=1
        set +e; attempt; rc=$?; set -e
    fi
    case $rc in
        0) result[$s]=pass ;;
        4) result[$s]=skip ;;
        *) result[$s]=fail ;;
    esac
done

as_root cp -r "$work/evidence/." "$evidence/"
as_root chown -R "$(id -u):$(id -g)" "$evidence"
{
    echo "| scenario | result |"
    echo "|---|---|"
    for s in $scenarios; do echo "| $s | ${result[$s]} |"; done
    echo
    echo "host: \`$(cat "$evidence/host.json")\`"
    [[ -n "$setup_done" ]] && echo "the one-time isolation setup was needed and done"
} > "$evidence/summary.md"
cat "$evidence/summary.md"
for s in $scenarios; do [[ "${result[$s]}" != fail ]] || exit 1; done
