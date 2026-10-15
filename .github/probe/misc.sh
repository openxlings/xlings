#!/usr/bin/env bash
# Round 5: O10 (virtio-fs in the machine kernel), O11 (a core system as a
# machine: vim and the current subos). Prints observations; never asserts.
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
curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-x86_64.tar.gz"
mkdir -p inst && tar -xzf xl.tgz -C inst && (cd inst/*/ && ./bin/xlings self install </dev/null) > logs/install.log 2>&1
export PATH="$HOME/.xlings/bin:$PATH"

say "### O10: virtio-fs in the machine kernel"
xlings install -y xim:linux-kernel-virt > logs/kernel.log 2>&1
K="$(ls -d "$HOME"/.xlings/data/xpkgs/xim-x-linux-kernel-virt/* 2>/dev/null | head -1)"
cat > "$T/kernel.sh" <<'SH'
K="$1"
find "$K" -maxdepth 4 | head -20
for f in $(find "$K" -name 'config*' -o -name '.config' | head -2); do echo "config: $f"; grep -E 'CONFIG_(VIRTIO_FS|FUSE_FS|DAX)=' "$f"; done
echo "modules:"; find "$K" -name 'virtiofs.ko*' -o -name 'fuse.ko*' | head
for f in $(find "$K" -name 'modules.builtin' | head -1); do echo "builtin: $f"; grep -E 'virtiofs|/fuse' "$f"; done
img="$(find "$K" -name 'vmlinuz*' | head -1)"; echo "image: $img"
if [ -n "$img" ]; then
    out="$(mktemp)"
    # The image is compressed: the extract-vmlinux way (gzip / zstd / xz)
    for magic in '\037\213\010' '(\265/\375' '\3757zXZ\000'; do
        pos=$(LC_ALL=C grep -aobP "$(printf "$magic" | od -An -tx1 | sed 's/ /\\x/g')" "$img" | head -1 | cut -d: -f1)
        [ -n "$pos" ] || continue
        tail -c +$((pos + 1)) "$img" | (gzip -dc 2>/dev/null || zstd -dc 2>/dev/null || xz -dc 2>/dev/null) > "$out" 2>/dev/null
        [ -s "$out" ] && break
    done
    if [ -s "$out" ]; then strings "$out" | grep -m5 -E 'virtiofs|virtio_fs|fuse' || echo "no virtiofs/fuse strings in the decompressed image"; else echo "could not decompress"; fi
fi
SH
block "linux-kernel-virt: virtio-fs" bash "$T/kernel.sh" "$K"

say "### O11: a core system as a machine"
sudo sysctl -q -w kernel.apparmor_restrict_unprivileged_userns=0
sudo chmod 666 /dev/kvm 2>/dev/null || true
LB="$HOME/.xlings/bin/luban"
LINES_=6 block "luban new pcore --edition core -y" timeout 2400 env XLINGS_NON_INTERACTIVE=1 "$LB" new pcore --edition core -y
block "luban config pcore --mode machine" "$LB" config pcore --mode machine
LINES_=80 block "luban enter pcore (machine): vim, current subos" python3 "$HERE/pty_run.py" --out "$T/logs/machine.bin" --wait 900 --tail 15 \
    --step '(Proceed|\[Y/n\]|y/N|[#$] $) ::: y\r' \
    --step '[#$] $ ::: uname -r; echo ACTIVE=$XLINGS_ACTIVE_SUBOS; ls -la /usr/bin/vim; vim --version | head -2; xlings subos list | head -5; cat /etc/xlings/root.json; echo PROBE-DONE\r' \
    --step 'PROBE-DONE ::: poweroff\r' \
    -- env TERM=xterm-256color "$LB" enter pcore
