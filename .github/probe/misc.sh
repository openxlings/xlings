#!/usr/bin/env bash
# O9 cursor themes, O10 virtiofs, O11 a core system as a machine (vim and
# the current subos). Prints observations; never asserts.
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
sudo apt-get -qq update >/dev/null

say "### O9: cursor themes"
sudo apt-get -qq install -y adwaita-icon-theme breeze-cursor-theme >/dev/null 2>&1
block "sizes, files, nominal sizes, licences" python3 - <<'PY'
import os, struct, glob
def nominal(path):
    try:
        b = open(path, "rb").read()
        if b[:4] != b"Xcur": return []
        n = struct.unpack_from("<I", b, 12)[0]
        return sorted({struct.unpack_from("<III", b, 16 + 12 * i)[1] for i in range(n)})
    except Exception:
        return []
for name, d in [("Adwaita", "/usr/share/icons/Adwaita/cursors"), ("Breeze", "/usr/share/icons/breeze_cursors/cursors")]:
    files = glob.glob(d + "/*")
    real = [f for f in files if not os.path.islink(f)]
    size = sum(os.path.getsize(f) for f in real)
    sizes = nominal(os.path.join(d, "left_ptr")) or nominal(os.path.join(d, "default"))
    print(f"{name}: {d} · {len(files)} names ({len(real)} files) · {size/1e6:.2f} MB · nominal sizes {sizes}")
PY
block "licences" bash -c "grep -m3 -iE 'license|licence' /usr/share/doc/adwaita-icon-theme/copyright; echo ---; grep -m3 -iE 'license|licence' /usr/share/doc/breeze-cursor-theme/copyright"

say "### O10: virtiofs"
sudo apt-get -qq install -y virtiofsd >/dev/null 2>&1
VFS="$(command -v virtiofsd || ls /usr/libexec/virtiofsd /usr/lib/qemu/virtiofsd 2>/dev/null | head -1)"
mkdir -p "$T/share"
block "virtiofsd unprivileged (--sandbox none)" bash -c "'$VFS' --version 2>&1 | head -1; ('$VFS' --socket-path='$T/vfs.sock' --shared-dir='$T/share' --sandbox none & echo \$! > '$T/vfs.pid'); sleep 2; ls -la '$T/vfs.sock' && echo 'listening as $(id -un)'; kill \$(cat '$T/vfs.pid') 2>/dev/null"
block "virtiofsd unprivileged (--sandbox namespace)" bash -c "('$VFS' --socket-path='$T/vfs2.sock' --shared-dir='$T/share' --sandbox namespace & echo \$! > '$T/vfs2.pid'); sleep 2; ls -la '$T/vfs2.sock' && echo listening; kill \$(cat '$T/vfs2.pid') 2>/dev/null"
curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-x86_64.tar.gz"
mkdir -p inst && tar -xzf xl.tgz -C inst && (cd inst/*/ && ./bin/xlings self install </dev/null) > logs/install.log 2>&1
export PATH="$HOME/.xlings/bin:$PATH"
xlings install -y xim:linux-kernel-virt > logs/kernel.log 2>&1
K="$(ls -d "$HOME"/.xlings/data/xpkgs/xim-x-linux-kernel-virt/* 2>/dev/null | head -1)"
block "linux-kernel-virt: virtio-fs and FUSE in its config" bash -c "ls '$K' '$K'/boot 2>/dev/null | head; cfg=\$(ls '$K'/boot/config* '$K'/config* 2>/dev/null | head -1); if [ -n \"\$cfg\" ]; then grep -E 'CONFIG_(VIRTIO_FS|FUSE_FS|DAX)=' \"\$cfg\"; else img=\$(ls '$K'/boot/vmlinuz* | head -1); echo \"no config file; searching \$img\"; strings \"\$img\" | grep -m3 -E 'virtiofs|virtio_fs'; fi"

say "### O11: a core system as a machine"
sudo sysctl -q -w kernel.apparmor_restrict_unprivileged_userns=0
sudo chmod 666 /dev/kvm 2>/dev/null || true
LB="$HOME/.xlings/bin/luban"
LINES_=6 block "luban new pcore --edition core -y" timeout 2400 env XLINGS_NON_INTERACTIVE=1 "$LB" new pcore --edition core -y
block "luban config pcore --mode machine" "$LB" config pcore --mode machine
LINES_=80 block "luban enter pcore (machine): vim, current subos" python3 "$HERE/pty_run.py" --out "$T/logs/machine.bin" --wait 600 --tail 10 \
    --step '(Proceed|\[Y/n\]|y/N)|y\r' \
    --step '[#$] $|uname -r; echo ACTIVE=$XLINGS_ACTIVE_SUBOS; ls -la /usr/bin/vim; vim --version | head -2; xlings subos list | head -5; cat /etc/xlings/root.json 2>&1; echo PROBE-DONE\r' \
    --step 'PROBE-DONE|poweroff\r' \
    -- env TERM=xterm-256color "$LB" enter pcore
