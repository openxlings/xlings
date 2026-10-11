#!/usr/bin/env bash
# Luban images (Luban design §A9, §B3.5; the aarch64 design §4): a luban-tiny
# environment made with `luban`, exported as a live ISO, a drive and a qcow2,
# each booted by qemu to stage-0 and its init -- on x86_64 by BIOS and by
# UEFI, the ISO from a CD and as a drive (its hybrid GPT); on aarch64 by UEFI
# (virt), every image written in-process. Then `luban try --proxy`: the
# machine's only network is the proxy.
#
# The kernel: x86_64 installs xim:linux-kernel into the root (the generic
# profile's); aarch64 exports with --boot virt. The firmware is
# xim:edk2-firmware, installed into the test's home as a user would.
#
# xtest: covers=LUBAN-ISO,LUBAN-DRIVE,LUBAN-TRY,LUBAN-BOOT-PROFILE,LUBAN-ISO-UEFI,LUBAN-ISO-HYBRID,LUBAN-QCOW2 requires=linux,qemu,bwrap,network
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/rootfs_lib.sh"

BIN="$(find_xlings_bin)"
LUBAN="$(dirname "$BIN")/luban"
[[ -x "$LUBAN" ]] || LUBAN="$(dirname "$BIN")/luban-tool/luban"
[[ -x "$LUBAN" ]] || fail "no luban beside $BIN"
RUNTIME_DIR="$(runtime_home_dir luban_image)"
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"
rootfs_home "$RUNTIME_DIR/home"
H="$RH"
cp "$LUBAN" "$H/bin/luban"
cp "$LUBAN" "$H/bin/luban-init"
# The index under test, when CI names one (an xim-pkgindex PR not yet
# published: tools/ci/xlings_tools.sh checked it out), instead of the published.
if [[ -d "${XLINGS_TOOLS_HOME:-}/index-under-test" ]]; then
  python3 -c '
import json, os, sys
cfg = json.load(open(sys.argv[1])) if os.path.exists(sys.argv[1]) else {}
cfg["index_repos"] = [{"name": "xim", "url": sys.argv[2]}]
json.dump(cfg, open(sys.argv[1], "w"), indent=2)
' "$H/.xlings.json" "$XLINGS_TOOLS_HOME/index-under-test"
fi
L() { ( cd /tmp && env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE XLINGS_HOME="$H" DISPLAY= WAYLAND_DISPLAY= "$H/bin/luban" "$@" ); }

# limine: the index's (prebuilt boot files and a static tool), installed by
# the edition's boot profile when the first image is made.
log "luban new t tiny"
n=0
until L new t tiny >"$RUNTIME_DIR/new.log" 2>&1; do
  n=$((n + 1)); [[ $n -ge 3 ]] && { tail -30 "$RUNTIME_DIR/new.log"; fail "luban new t tiny"; }
  X subos remove -y t >/dev/null 2>&1 || true; sleep 5
done
grep -q "subos created: t" "$RUNTIME_DIR/new.log" || fail "no creation report: $(tail -5 "$RUNTIME_DIR/new.log")"
arch="$(uname -m)"
BOOT=()   # what export is told about the kernel
if [[ "$arch" == x86_64 ]]; then
  # A machine image boots a kernel; the edition does not carry one (Luban
  # design §A6) -- installed into the root, as a user would.
  retry X install -y --subos t xim:linux-kernel >"$RUNTIME_DIR/kernel.log" 2>&1 \
    || { tail -20 "$RUNTIME_DIR/kernel.log"; fail "install the kernel into the root"; }
  QEMU=(qemu-system-x86_64)
else
  # aarch64: the virt boot profile (the generic one is x86_64's).
  BOOT=(--boot virt)
  QEMU=(qemu-system-aarch64 -M virt)
fi
# The UEFI firmware, from the index (into this home: `luban try` finds it there).
retry X install -y xim:edk2-firmware >"$RUNTIME_DIR/firmware.log" 2>&1 \
  || { tail -20 "$RUNTIME_DIR/firmware.log"; fail "install edk2-firmware"; }
OVMF="$(ls "$H"/data/xpkgs/xim-x-edk2-firmware/*/share/edk2/edk2-"$arch"-code.fd | head -1)"
[[ -f "$OVMF" ]] || fail "no UEFI firmware for $arch"
# How a machine is handed it: virt takes the code image as its -bios; a PC
# maps it as flash (its size is not the BIOS ROM's), with a variable store
# beside it -- a copy of the empty one qemu-system ships.
UEFI=(-bios "$OVMF")
if [[ "$arch" == x86_64 ]]; then
  vars="$(dirname "$(readlink -f "$(command -v qemu-system-x86_64)")")/../share/qemu/edk2-i386-vars.fd"
  [[ -f "$vars" ]] || fail "no UEFI variable store beside qemu-system-x86_64 ($vars)"
  cp "$vars" "$RUNTIME_DIR/vars.fd"
  UEFI=(-drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF" -drive "if=pflash,format=raw,unit=1,file=$RUNTIME_DIR/vars.fd")
fi
# Runs a command (its own session) until its output shows the console
# prompt -- a booted system waits there forever -- then stops all of it.
until_console() {   # $1 log, then the command
  local log="$1"; shift
  : > "$log"
  setsid "$@" > "$log" 2>&1 < /dev/null &
  local pid=$! t=0
  until tr -d '\r' < "$log" | grep -aq "Please press Enter to activate this console"; do
    sleep 2; t=$((t + 2))
    kill -0 "$pid" 2>/dev/null && (( t < 600 )) || break
  done
  kill -- "-$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}
boot() {   # $1 log name, then qemu's media arguments
  local name="$1"; shift
  local accel=()
  if [[ -w /dev/kvm ]]; then accel=(-enable-kvm -cpu host); elif [[ "$arch" != x86_64 ]]; then accel=(-cpu max); fi
  until_console "$RUNTIME_DIR/$name.log" "${QEMU[@]}" "${accel[@]}" -m 2048 -smp 2 -nographic -no-reboot -nic none "$@"
  tr -d '\r' < "$RUNTIME_DIR/$name.log" | grep -aq "luban-init: booting 'default'" \
    || { tr -d '\r' < "$RUNTIME_DIR/$name.log" | tail -25; fail "$name: did not reach stage-0"; }
  tr -d '\r' < "$RUNTIME_DIR/$name.log" | grep -aq "Please press Enter to activate this console" \
    || { tr -d '\r' < "$RUNTIME_DIR/$name.log" | tail -15; fail "$name: did not reach its init"; }
  log "  $name: stage-0 and init"
}

CD=()   # qemu's arguments for $1 as a CD on this machine (no IDE on virt: SCSI)
cdrom() {
  if [[ "$arch" == x86_64 ]]; then CD=(-cdrom "$1")
  else CD=(-device virtio-scsi-pci,id=scsi -device scsi-cd,drive=cd -drive "if=none,id=cd,media=cdrom,readonly=on,format=raw,file=$1"); fi
}

log "luban export t t.iso, booted (written in-process: El Torito BIOS+UEFI, hybrid GPT)"
L export t "$RUNTIME_DIR/t.iso" "${BOOT[@]}" > "$RUNTIME_DIR/iso.log" 2>&1 || { cat "$RUNTIME_DIR/iso.log"; fail "export iso"; }
file "$RUNTIME_DIR/t.iso" | grep -q "ISO 9660" || fail "not an ISO: $(file "$RUNTIME_DIR/t.iso")"
if [[ "$arch" == x86_64 ]]; then
  boot iso-bios -cdrom "$RUNTIME_DIR/t.iso" -boot d
  boot iso-as-drive-bios -drive "file=$RUNTIME_DIR/t.iso,format=raw,if=virtio,readonly=on"
fi
cdrom "$RUNTIME_DIR/t.iso"
boot iso-uefi "${UEFI[@]}" "${CD[@]}"
boot iso-as-drive-uefi "${UEFI[@]}" -drive "file=$RUNTIME_DIR/t.iso,format=raw,if=virtio,readonly=on"

if [[ "$arch" == x86_64 ]]; then
log "luban export t p.iso --boot <profile>: the profile is installed into the root, its kernel and command line boot"
# A boot profile (Luban OS design part 2 §2.4) from an index of this test's:
# the kernel already in the root, and a command line of its own.
release="$(basename "$(dirname "$(ls "$H"/subos/t/root/usr/lib/modules/*/vmlinuz | head -1)")")"
FIX="$RUNTIME_DIR/fixture-index"
mkdir -p "$FIX/pkgs/l"
echo 'xim_indexrepos = {}' > "$FIX/xim-indexrepos.lua"
cat > "$FIX/pkgs/l/luban-boot-test.lua" <<EOF
package = { spec = '1', name = 'luban-boot-test', archs = {'x86_64', 'aarch64'}, xpm = { linux = { ['1.0.0'] = {} } } }
import('xim.libxpkg.pkginfo')
function install()
    local dir = path.join(pkginfo.install_dir(), 'share', 'luban')
    os.mkdir(dir)
    io.writefile(path.join(dir, 'boot.json'),
        '{"profile":"test","release":"$release","cmdline":["console=ttyS0","luban.profile=test"]}')
    return true
end
EOF
python3 -c '
import json, sys
cfg = json.load(open(sys.argv[1]))
repos = cfg.setdefault("index_repos", [])   # the default index stays (Config keeps it)
repos.append({"name": "fixture", "url": sys.argv[2], "source": "git"})
json.dump(cfg, open(sys.argv[1], "w"))
' "$H/.xlings.json" "$FIX"
L export t "$RUNTIME_DIR/p.iso" --boot fixture:luban-boot-test > "$RUNTIME_DIR/profile.log" 2>&1 \
  || { cat "$RUNTIME_DIR/profile.log"; fail "export --boot"; }
grep -aq "luban.profile=test" "$RUNTIME_DIR/p.iso" || fail "the profile's command line is not in the image"
if X subos export t --iso "$RUNTIME_DIR/both.iso" --boot generic --kernel /dev/null >/dev/null 2>&1; then
  fail "--boot and --kernel together were accepted"
fi
boot iso-profile -cdrom "$RUNTIME_DIR/p.iso" -boot d
fi

log "luban export t <dir>/ (a directory is written with its slash)"
L export t "$RUNTIME_DIR/rootdir/" > "$RUNTIME_DIR/dir.log" 2>&1 || { cat "$RUNTIME_DIR/dir.log"; fail "export dir/"; }
# Its /usr may be a link that resolves inside the root, not on this host.
[[ ( -d "$RUNTIME_DIR/rootdir/usr" || -L "$RUNTIME_DIR/rootdir/usr" ) && -d "$RUNTIME_DIR/rootdir/etc" ]] \
  || { ls -la "$RUNTIME_DIR/rootdir" | head; fail "export dir/ wrote no root"; }
[[ ! -e "$RUNTIME_DIR/rootdir/rootdir" ]] || fail "export dir/ nested the root inside itself"

log "luban export t t.img (a drive), booted"
L export t "$RUNTIME_DIR/t.img" --size 1G "${BOOT[@]}" > "$RUNTIME_DIR/drive.log" 2>&1 || { cat "$RUNTIME_DIR/drive.log"; fail "export drive"; }
# Its partition table, read without a machine's tool: the EFI system partition.
python3 -c '
import struct, sys, uuid
d = open(sys.argv[1], "rb"); d.seek(512); h = d.read(92)
assert h[:8] == b"EFI PART"
table, n, size = struct.unpack("<QII", h[72:88]); d.seek(table * 512)
types = {str(uuid.UUID(bytes_le=d.read(size)[:16])) for _ in range(n)}
sys.exit(0 if "c12a7328-f81f-11d2-ba4b-00a0c93ec93b" in types else 1)
' "$RUNTIME_DIR/t.img" || fail "no EFI system partition in the drive's GPT"
if [[ "$arch" == x86_64 ]]; then boot drive-bios -drive "file=$RUNTIME_DIR/t.img,format=raw,if=virtio"; fi
boot drive-uefi "${UEFI[@]}" -drive "file=$RUNTIME_DIR/t.img,format=raw,if=virtio"

log "luban export t t.qcow2 (in-process), booted"
L export t "$RUNTIME_DIR/t.qcow2" --size 1G "${BOOT[@]}" > "$RUNTIME_DIR/qcow2.log" 2>&1 || { cat "$RUNTIME_DIR/qcow2.log"; fail "export qcow2"; }
[[ "$(head -c 4 "$RUNTIME_DIR/t.qcow2" | od -An -tx1 | tr -d ' \n')" == 514649fb ]] || fail "not a qcow2"
boot qcow2-uefi "${UEFI[@]}" -drive "file=$RUNTIME_DIR/t.qcow2,format=qcow2,if=virtio"

log "luban __pipe: a private machine's connection reaches its proxy, both ways"
relayed="$(python3 - "$H/bin/luban" <<'PY'
import socket, subprocess, sys, threading
srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(1)
def echo():
    c, _ = srv.accept()
    while (d := c.recv(4096)): c.sendall(d.upper())
    c.close()
threading.Thread(target=echo, daemon=True).start()
a, b = socket.socketpair()   # what qemu's guestfwd cmd: hands over
p = subprocess.Popen([sys.argv[1], "__pipe", "127.0.0.1", str(srv.getsockname()[1])], stdin=b, stdout=b)
b.close(); a.sendall(b"through the proxy"); a.shutdown(socket.SHUT_WR); a.settimeout(10)
out = b""
while (d := a.recv(4096)): out += d
p.wait(timeout=10); print(out.decode(), p.returncode)
PY
)"
[[ "$relayed" == "THROUGH THE PROXY 0" ]] || fail "the relay: '$relayed'"

log "luban try t.iso --proxy: the proxy is its only network (and it boots with the proxy down)"
until_console "$RUNTIME_DIR/try.log" env XLINGS_HOME="$H" DISPLAY= WAYLAND_DISPLAY= "$H/bin/luban" try "$RUNTIME_DIR/t.iso" \
  --proxy socks5h://127.0.0.1:9 --memory 2048
out="$(tr -d '\r' < "$RUNTIME_DIR/try.log")"
grep -q "its only network is the proxy" <<<"$out" || fail "try --proxy: $(tail -5 <<<"$out")"
grep -aq "luban-init: booting" <<<"$out" || fail "try: did not boot: $(tail -10 <<<"$out")"
log "PASS ($arch): ISO (CD and drive), drive and qcow2 boot ($( [[ "$arch" == x86_64 ]] && echo "BIOS and UEFI" || echo UEFI)); luban try --proxy"
