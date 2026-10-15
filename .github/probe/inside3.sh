#!/usr/bin/env bash
# Round 3: O3b (dropping capabilities for the user's processes after
# session-init keeps CAP_SYS_ADMIN), O6 (a machine: what /usr/bin/vim
# resolves to, which subos is "this one", a regular user's install), O7 (a
# desktop system's /usr/share: launcher entries, cursor theme, terminfo).
# Prints observations; never asserts.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
XV="${XV:-2026.10.15.4}"
T="$RUNNER_TEMP/p"; mkdir -p "$T/logs"; cd "$T"
S="${GITHUB_STEP_SUMMARY:-/dev/null}"
say() { echo "$*"; echo "$*" >> "$S"; }
block() {
    local title="$1"; shift; local out rc
    out="$("$@" 2>&1)"; rc=$?
    echo "== $title (exit $rc)"; echo "$out"
    { echo; echo "**$title** (exit $rc)"; echo '```'; echo "$out" | tail -n "${LINES_:-40}"; echo '```'; } >> "$S"
}
sudo sysctl -q -w kernel.apparmor_restrict_unprivileged_userns=0
curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-x86_64.tar.gz"
mkdir -p inst && tar -xzf xl.tgz -C inst
(cd inst/*/ && ./bin/xlings self install </dev/null) > logs/install.log 2>&1
export PATH="$HOME/.xlings/bin:$PATH"
LB="$HOME/.xlings/bin/luban"
BW="$(ls "$HOME"/.xlings/data/xpkgs/xim-x-bwrap/*/bin/bwrap 2>/dev/null | head -1)"
[[ -x "$BW" ]] || { xlings install -y xim:bwrap >/dev/null 2>&1; BW="$(ls "$HOME"/.xlings/data/xpkgs/xim-x-bwrap/*/bin/bwrap | head -1)"; }

say "### O3b: session-init keeps CAP_SYS_ADMIN; the user's processes must not"
cat > "$T/drop.py" <<'PY'
import ctypes, os
libc = ctypes.CDLL(None, use_errno=True)
def status(tag):
    caps = [l.strip() for l in open("/proc/self/status") if l.startswith(("CapEff", "CapPrm", "CapBnd", "CapAmb"))]
    print(tag, " ".join(caps))
status("session-init:")
ok = os.system("mount --bind /tmp /mnt/a 2>/dev/null && umount /mnt/a") == 0
print("session-init can mount:", ok)
dropped = all(libc.prctl(24, c, 0, 0, 0) == 0 for c in range(0, 41))   # PR_CAPBSET_DROP
print("bounding set dropped:", dropped, "errno", ctypes.get_errno())
libc.prctl(47, 4, 0, 0, 0)                                              # PR_CAP_AMBIENT_CLEAR_ALL
SECBIT_NOROOT, SECBIT_NOROOT_LOCKED = 1 << 0, 1 << 1
print("securebits NOROOT:", libc.prctl(28, SECBIT_NOROOT | SECBIT_NOROOT_LOCKED, 0, 0, 0) == 0, "errno", ctypes.get_errno())  # PR_SET_SECUREBITS
pid = os.fork()
if pid == 0:
    os.execvp("sh", ["sh", "-c", "grep -E 'CapEff|CapPrm|CapBnd' /proc/self/status | tr '\\n' ' '; echo; mount --bind /tmp /mnt/a 2>&1 && echo 'child mount: ALLOWED' || echo 'child mount: refused'"])
os.waitpid(pid, 0)
PY
block "O3b with CAP_SYS_ADMIN only" "$BW" --unshare-user --uid 0 --gid 0 --unshare-pid --cap-add CAP_SYS_ADMIN \
    --ro-bind / / --dev /dev --proc /proc --tmpfs /mnt --dir /mnt/a --bind "$T" "$T" -- python3 "$T/drop.py"
block "O3b with CAP_SYS_ADMIN and CAP_SETPCAP" "$BW" --unshare-user --uid 0 --gid 0 --unshare-pid --cap-add CAP_SYS_ADMIN --cap-add CAP_SETPCAP \
    --ro-bind / / --dev /dev --proc /proc --tmpfs /mnt --dir /mnt/a --bind "$T" "$T" -- python3 "$T/drop.py"

say "### making core and desktop"
LINES_=8 block "luban new pcore --edition core -y" timeout 2400 env XLINGS_NON_INTERACTIVE=1 "$LB" new pcore --edition core -y
LINES_=8 block "luban new pdesk --edition desktop -y" timeout 2400 env XLINGS_NON_INTERACTIVE=1 "$LB" new pdesk --edition desktop -y

say "### O7: a desktop system's /usr/share"
LINES_=80 block "pdesk (sandbox): /usr/share, XDG_DATA_DIRS, launcher entries, cursor themes, terminfo" timeout 300 "$LB" run pdesk -- /bin/sh -c '
echo "XDG_DATA_DIRS=$XDG_DATA_DIRS"; echo "--- /usr/share:"; ls /usr/share
echo "--- applications dirs:"; for d in $(echo "${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" | tr ":" " ") "$HOME/.local/share"; do ls -d "$d/applications" 2>/dev/null && ls "$d/applications" | head; done
echo "--- .desktop files anywhere in /xlings payloads:"; find /xlings/data/xpkgs -name "*.desktop" 2>/dev/null | head -20
echo "--- cursor themes:"; ls -d /usr/share/icons/*/cursors ~/.icons/*/cursors 2>/dev/null; find /xlings/data/xpkgs -type d -name cursors 2>/dev/null | head
echo "--- terminfo:"; ls -d /usr/share/terminfo /etc/terminfo 2>/dev/null; find /xlings/data/xpkgs -maxdepth 5 -type d -name terminfo 2>/dev/null | head
echo "--- foot:"; command -v foot; ls -la "$(command -v foot)" 2>/dev/null'
block "pdesk session files (what starts, its environment)" bash -c "G=\$(readlink -f \$(find / -path '*/subos/pdesk/root' 2>/dev/null | head -1)); echo \$G; cat \$(dirname \$(dirname \$G))/rootfs/etc/luban/* 2>/dev/null | head -20; find \$(dirname \$(dirname \$G))/rootfs/etc -maxdepth 3 -name '*session*' -o -maxdepth 3 -name '*xdesk*' 2>/dev/null | head; ls \$G/usr/share/luban 2>/dev/null; cat \$G/usr/share/luban/settings.json 2>/dev/null | head -30"

say "### O6: a machine"
sudo chmod 666 /dev/kvm 2>/dev/null || true
block "luban config pcore --mode machine" timeout 300 "$LB" config pcore --mode machine
LINES_=60 block "machine: vim, the active subos, a regular user's install" timeout 1500 env XLINGS_NON_INTERACTIVE=1 "$LB" run -y pcore -- /bin/sh -c '
echo "kernel: $(uname -r)"; echo "XLINGS_ACTIVE_SUBOS=$XLINGS_ACTIVE_SUBOS"; ls -la /usr/bin/vim
vim --version 2>&1 | head -3
xlings subos list 2>&1 | head -6
id
xlings install -y xim:jq 2>&1 | tail -3'
LINES_=60 block "machine as its login user: vim and install" timeout 900 "$LB" run -y pcore --user root -- /bin/sh -c 'adduser -D probeuser 2>/dev/null || useradd -m probeuser; su probeuser -c "id; vim --version 2>&1 | head -2; xlings install -y xim:jq 2>&1 | tail -3"'
"$LB" stop pcore >/dev/null 2>&1 || true
