#!/usr/bin/env bash
# A root booted by a kernel (design part 2 §8): stage-0, the boot entries,
# and one declaration giving the same tree wherever it is presented.
#
# The disk is `subos export --rootfs` plus two SubOS of its own (copies of
# its default: `trial`, and `broken`, whose generation lost its init), made
# into ext4 as root inside a user namespace. The kernel is the root's own
# (/usr/lib/modules/<ver>/vmlinuz, from xim:linux-kernel). A state file in
# /root carries the scenario across boots; /etc/rc.local (machine state)
# prints what each boot was and powers off.
#
#   boot 1: the trial is `broken` -- stage-0 skips it (no init), drops the
#           trial, boots `default`; then `subos boot trial --once`;
#   boot 2: `trial`, once; inside it, `subos boot default --now`: init
#           re-execs stage-0 and user space is `default` without a new kernel;
#   boot 3: not confirmed, the trial is gone: `default` again.
# And the tree of /usr is the same in the bwrap instance, the container and
# the booted machine.
#
# xtest: covers=BOOT-STAGE0,BOOT-ONCE-FALLBACK,BOOT-NOW,EXPORT-DISK,ROOT-SAME-TREE,LUBAN-TINY,PERF-STAGE0 requires=linux,qemu,bwrap,docker,network
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/rootfs_lib.sh"

BIN="$(find_xlings_bin)"
RUNTIME_DIR="$(runtime_home_dir rootfs_boot)"
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"
IMG=xlings-e2e-luban-boot
trap 'docker rmi -f "$IMG" >/dev/null 2>&1 || true' EXIT

rootfs_home "$RUNTIME_DIR/home"
H="$RH"
rootfs_new tiny subos:luban-tiny "$RUNTIME_DIR/new.log" || fail "subos new --from subos:luban-tiny"
# The kernel is the machine's, not the edition's (Luban design §A6): installed
# into the root to boot it (an edition from before that already has it).
retry X install -y --subos tiny xim:linux-kernel >"$RUNTIME_DIR/kernel.log" 2>&1 \
  || { tail -20 "$RUNTIME_DIR/kernel.log"; fail "install the kernel into the root"; }
S="$H/subos/tiny"
KERNEL="$(ls "$S"/root/usr/lib/modules/*/vmlinuz | head -1)"
[[ -f "$KERNEL" ]] || fail "the root has no kernel"

log "the tree, in a bwrap instance"
tree_instance="$(X subos exec tiny -- /bin/sh -c "$tree_lines_script" | md5sum | cut -d' ' -f1)"

cat > "$S/rootfs/etc/rc.local" <<'EOF'
#!/bin/sh
# The scenario's driver: what this boot is, then the next step.
state=$(cat /root/.stage 2>/dev/null || echo 0)
boot=$(xlings subos boot 2>/dev/null | sed -n 's/^this boot \([^ ]*\) via \([^ ,]*\).*/\1:\2/p')
tree=$(cd /usr && for f in bin/* lib/*; do printf "%s %s\n" "$f" "$(readlink "$f")"; done \
       | sed -E "s|/subos/[^/]+/|/subos/*/|g" | md5sum | cut -d' ' -f1)
elapsed=$(grep '"event":"boot"' "$XLINGS_HOME/logs/boot.ndjson" | tail -1 | sed -n 's/.*"stage0_elapsed_us":\([0-9][0-9]*\).*/\1/p')
echo "LUBAN-BOOT stage=$state boot=$boot tree=$tree stage0_us=$elapsed"
case "$state" in
  0) echo 1 > /root/.stage; xlings subos boot trial --once >/dev/null 2>&1 ;;
  1) echo 2 > /root/.stage; sync
     # After this script (and init's sysinit) has finished: init then takes
     # the signal, re-execs stage-0, and the next user space is default's.
     ( sleep 2; xlings subos boot default --now ) </dev/null >/dev/console 2>&1 &
     exit 0 ;;
  2) echo 3 > /root/.stage ;;
  *) ;;
esac
sync
poweroff -f
EOF
chmod +x "$S/rootfs/etc/rc.local"

log "export --rootfs, two more SubOS, ext4"
X subos export tiny --rootfs "$RUNTIME_DIR/root" >"$RUNTIME_DIR/export.log" 2>&1 \
  || { cat "$RUNTIME_DIR/export.log"; fail "export --rootfs"; }
R="$RUNTIME_DIR/root"
IH="$R$H"
for n in trial broken; do
  cp -a "$IH/subos/default" "$IH/subos/$n"
  mkdir -p "$IH/config/subos/$n"
  cp "$IH/config/subos/default/instance.json" "$IH/config/subos/$n/"
  # Its own generation: the copied pointer names root.gen inside the copy.
done
gen="$(readlink "$IH/subos/broken/root")"
rm -f "$IH/subos/broken/$gen/usr/bin/init"
# boot 1's trial: a SubOS with no init.
printf '{"default":"default","fallback":"default","once":"broken","tries":{}}\n' > "$IH/boot.json"
X subos export tiny --disk "$RUNTIME_DIR/probe.img" >/dev/null 2>&1 || true   # (exercises --disk itself)
[[ -s "$RUNTIME_DIR/probe.img" ]] || fail "export --disk wrote nothing"
bwrap --unshare-user --uid 0 --gid 0 --dev-bind / / -- \
  "$(command -v mkfs.ext4 || ls /usr/sbin/mkfs.ext4 /sbin/mkfs.ext4 2>/dev/null | head -1)" -q -F -L luban -d "$R" "$RUNTIME_DIR/disk.img" 1G \
  || fail "mkfs.ext4"

qemu_boot() {
  local accel=()
  [[ -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
  timeout 600 qemu-system-x86_64 "${accel[@]}" -m 1024 -smp 2 -nographic -no-reboot -nic none \
    -kernel "$KERNEL" -drive "file=$RUNTIME_DIR/disk.img,format=raw,if=virtio" \
    -append "root=/dev/vda rw console=ttyS0 panic=-1 init=$H/boot/xlings-init" \
    > "$RUNTIME_DIR/boot-$1.log" 2>&1 || true
  tr -d '\r' < "$RUNTIME_DIR/boot-$1.log" | grep -E "xlings-init:|LUBAN-BOOT" || true
}

log "boot 1: a trial with no init"
b1="$(qemu_boot 1)"; echo "$b1" | sed 's/^/      | /'
grep -q "'broken' has no init" <<<"$b1" || fail "boot 1: stage-0 did not skip the broken trial"
grep -q "LUBAN-BOOT stage=0 boot=default:default" <<<"$b1" || fail "boot 1: default was not booted"
tree_vm="$(sed -n 's/.*LUBAN-BOOT stage=0 .* tree=\([0-9a-f]*\).*/\1/p' <<<"$b1" | head -1)"

log "boot 2: the trial, once; then --now back to default"
b2="$(qemu_boot 2)"; echo "$b2" | sed 's/^/      | /'
grep -q "LUBAN-BOOT stage=1 boot=trial:once" <<<"$b2" || fail "boot 2: the trial did not boot once"
grep -q "LUBAN-BOOT stage=2 boot=default:once" <<<"$b2" || fail "boot 2: --now did not hand / to default"
[[ "$(grep -c 'Linux version' "$RUNTIME_DIR/boot-2.log")" -le 1 ]] || fail "boot 2: --now restarted the kernel"

log "boot 3: the trial was not made the default"
b3="$(qemu_boot 3)"; echo "$b3" | sed 's/^/      | /'
grep -q "LUBAN-BOOT stage=3 boot=default:default" <<<"$b3" || fail "boot 3: not default"

log "stage-0 budget: startup through preparing exec init <= 200 ms"
samples="$(printf '%s\n' "$b1" "$b2" "$b3" | sed -n 's/.*LUBAN-BOOT .* stage0_us=\([0-9][0-9]*\).*/\1/p')"
[[ "$(wc -l <<<"$samples")" -eq 4 ]] || fail "every boot, including --now, must report stage-0 timing"
while IFS= read -r elapsed; do
  [[ "$elapsed" =~ ^[0-9]+$ && "$elapsed" -le 200000 ]] \
    || fail "stage-0 exceeded 200 ms: ${elapsed} us"
done <<<"$samples"

log "the same tree: instance, container, machine"
X subos export tiny --tar "$RUNTIME_DIR/tiny.tar.gz" >/dev/null 2>&1 || fail "export --tar"
docker import "$RUNTIME_DIR/tiny.tar.gz" "$IMG" >/dev/null
tree_container="$(docker run --rm --network none "$IMG" /bin/sh -c "$tree_lines_script" | md5sum | cut -d' ' -f1)"
log "  instance $tree_instance  container $tree_container  machine $tree_vm"
[[ -n "$tree_vm" && "$tree_instance" == "$tree_container" && "$tree_container" == "$tree_vm" ]] \
  || fail "one declaration, three trees"

log "PASS: the machine boots the SubOS boot.json chooses, and switches without a reboot"
