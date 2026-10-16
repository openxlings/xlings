#!/usr/bin/env bash
# Luban images (Luban design §A9, §B3.5; the aarch64 design §4): a luban-tiny
# environment made with `luban`, exported as a live ISO, a drive and a qcow2,
# each booted by qemu to stage-0 and its init -- on x86_64 by BIOS and by
# UEFI, the ISO from a CD and as a drive (its hybrid GPT); on aarch64 by UEFI
# (virt), every image written in-process. Then the environment as a machine:
# its only network is the proxy; then its desktop (xim:labwc-desktop) shown
# to a client of the machine as the Luban OS window is, and typed into.
#
# The kernel: x86_64 installs xim:linux-kernel into the root (the generic
# profile's); aarch64 exports with --boot virt. The firmware is
# xim:edk2-firmware, installed into the test's home as a user would.
#
# xtest: covers=LUBAN-ISO,LUBAN-DRIVE,LUBAN-TRY,LUBAN-BOOT-PROFILE,LUBAN-ISO-UEFI,LUBAN-ISO-HYBRID,LUBAN-QCOW2,LUBAN-MACHINE-DESKTOP requires=linux,qemu,bwrap,network
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
  # An index already synced stays until it is gone (tools/ci/xlings_tools.sh).
  rm -rf "$H/data/xim-pkgindex"
  X update >"$RUNTIME_DIR/update.log" 2>&1 || { tail -20 "$RUNTIME_DIR/update.log"; fail "xlings update (the index under test)"; }
  log "index under test: $(readlink -f "$H/data/xim-pkgindex")"
fi
L() { ( cd /tmp && env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE XLINGS_HOME="$H" DISPLAY= WAYLAND_DISPLAY= "$H/bin/luban" "$@" ); }
# Where a Luban system's own files are: in the system domain's private home
# when this host built it there (part 3 §3.4), else in the home itself.
sysdir() { local d="$H/domains/xlings/private/subos/$1"; [[ -d "$d" ]] && echo "$d" || echo "$H/subos/$1"; }

# limine: the index's (prebuilt boot files and a static tool), installed by
# the edition's boot profile when the first image is made.
log "luban new t --edition tiny"
n=0
until L new t --edition tiny >"$RUNTIME_DIR/new.log" 2>&1; do
  n=$((n + 1)); [[ $n -ge 3 ]] && { tail -30 "$RUNTIME_DIR/new.log"; fail "luban new t --edition tiny"; }
  X subos remove -y t >/dev/null 2>&1 || true; sleep 5
done
grep -q "t is ready" "$RUNTIME_DIR/new.log" || fail "no creation report: $(tail -5 "$RUNTIME_DIR/new.log")"
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
# The UEFI firmware, from the index (into this home: `luban enter <image>` finds it there).
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
  cp "$vars" "$RUNTIME_DIR/vars.empty.fd"
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
  # Each boot from an empty variable store: one boot's entries (a CD that is
  # gone) are not the next machine's.
  [[ -f "$RUNTIME_DIR/vars.empty.fd" ]] && cp "$RUNTIME_DIR/vars.empty.fd" "$RUNTIME_DIR/vars.fd"
  local accel=()
  if [[ -w /dev/kvm ]]; then accel=(-enable-kvm -cpu host); elif [[ "$arch" != x86_64 ]]; then accel=(-cpu max); fi
  until_console "$RUNTIME_DIR/$name.log" "${QEMU[@]}" "${accel[@]}" -m 2048 -smp 2 -nographic -no-reboot -nic none "$@"
  tr -d '\r' < "$RUNTIME_DIR/$name.log" | grep -aq "luban-init: booting 'default'" \
    || { tr -d '\r' < "$RUNTIME_DIR/$name.log" | tail -60; fail "$name: did not reach stage-0"; }
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
release="$(basename "$(dirname "$(root_kernel "$(sysdir t)")")")"
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
if X subos __export t --iso "$RUNTIME_DIR/both.iso" --boot generic --kernel /dev/null >/dev/null 2>&1; then
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

log "luban enter t.iso: an image boots as it is, its console on the terminal"
until_console "$RUNTIME_DIR/image.log" env XLINGS_HOME="$H" DISPLAY= WAYLAND_DISPLAY= "$H/bin/luban" enter "$RUNTIME_DIR/t.iso"
out="$(tr -d '\r' < "$RUNTIME_DIR/image.log")"
grep -aq "luban-init: booting" <<<"$out" || fail "enter t.iso: did not boot: $(tail -10 <<<"$out")"
grep -aq "qemu" <<<"$(grep -a "booting t.iso" <<<"$out")" && fail "enter t.iso names its monitor: $(grep -a booting <<<"$out")"

log "luban enter t --mode machine: the environment itself, its own kernel, a session with its shell"
q="$(command -v "${QEMU[0]}" || true)"
log "  qemu: ${q:-none} -> $(readlink -f "$q" 2>/dev/null); 9p: $("${QEMU[0]}" -M none -device virtio-9p-pci,help 2>&1 | head -1)"
L config t --proxy socks5h://127.0.0.1:9 >/dev/null 2>&1 || fail "config --proxy"
# The machine's own logs are kept with the test's, whatever happens next.
keep_machine_logs() { for f in "$H"/logs/machines/t/*.log; do [[ -f "$f" ]] && cp "$f" "$RUNTIME_DIR/machine-$(basename "$f")"; done; true; }
trap keep_machine_logs EXIT
machine="$( { (sleep 120; printf 'echo MACHINE-$(uname -r) > /root/machine.txt; cat /root/machine.txt; exit\n'; sleep 10) 2>/dev/null || true; } \
  | timeout 600 script -qfec "env XLINGS_HOME=$H DISPLAY= WAYLAND_DISPLAY= $H/bin/luban enter t --mode machine --temp" /dev/null 2>&1 | tr -d '\r' || true)"
printf '%s\n' "$machine" > "$RUNTIME_DIR/machine-session.log"
grep -q "t is up" <<<"$machine" || fail "machine: did not come up: $(tail -20 <<<"$machine"; tail -20 "$H"/logs/machines/t/console.log 2>/dev/null)"
grep -q "only its proxy" <<<"$machine" || fail "machine: its network is not only the proxy: $(grep -a starting <<<"$machine")"
grep -q "MACHINE-" <<<"$machine" || fail "machine: the session ran nothing: $(tail -20 <<<"$machine")"
[[ ! -e "$(sysdir t)/rootfs/root/machine.txt" ]] || fail "machine --temp: what it wrote stayed"
[[ ! -e "$H/run/machines/t/scratch" ]] || fail "machine --temp: its copy stayed"
log "the desktop: labwc-desktop in t, its session bridged by opensal-wlr to a client of the machine, as the Luban OS window is"
CLIENT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/luban_desktop_client.py"
retry X install -y --subos t xim:labwc-desktop >"$RUNTIME_DIR/labwc.log" 2>&1 \
  || { tail -20 "$RUNTIME_DIR/labwc.log"; fail "install labwc-desktop into t"; }
L config t --mode machine --on-close background >/dev/null 2>&1 || fail "config t --mode machine --on-close background"
{ (sleep 120; printf 'exit\n'; sleep 5) 2>/dev/null || true; } \
  | timeout 300 script -qfec "env XLINGS_HOME=$H DISPLAY= WAYLAND_DISPLAY= $H/bin/luban enter t" /dev/null >"$RUNTIME_DIR/desktop-boot.log" 2>&1 || true
[[ -S "$H/run/machines/t/luban.sock" ]] || fail "desktop: t is not running: $(tail -10 "$RUNTIME_DIR/desktop-boot.log")"
desk="$(timeout 300 python3 "$CLIENT" "$H/run/machines/t" \
  '{Alt+Return}echo DESKTOP-$(uname -m) > /root/desktop.txt\n' "$RUNTIME_DIR/desktop.ppm" || true)"
cp "$(sysdir t)/rootfs/var/log/luban/desktop.log" "$RUNTIME_DIR/machine-desktop.log" 2>/dev/null || true
grep -q '"drew": true' <<<"$desk" || fail "desktop: it never drew: $desk $(tail -20 "$RUNTIME_DIR/machine-desktop.log" 2>/dev/null)"
[[ "$(cat "$(sysdir t)/rootfs/root/desktop.txt" 2>/dev/null)" == "DESKTOP-$arch" ]] \
  || fail "desktop: what was typed into its terminal did not run ($desk)"
L stop t >/dev/null 2>&1 || true

log "the same desktop in a sandbox: luban __desktop under xlings subos use --cmd, as the window runs it"
L config t --mode sandbox >/dev/null 2>&1 || fail "config t --mode sandbox"
# The luban the sandbox sees: a system in the prefix domain sees its own
# home (/xlings) and the luban that domain carries, not this home's.
inner_luban="$H/bin/luban"
[[ -d "$H/domains/xlings/private/subos/t" ]] && inner_luban=/xlings/bin/luban
sbox="$(cd /tmp && timeout 300 python3 "$CLIENT" --exec \
  '{Alt+Return}echo SANDBOX-$(uname -m) > /root/sandbox.txt\n' "$RUNTIME_DIR/sandbox.ppm" -- \
  env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE XLINGS_HOME="$H" "$H/bin/xlings" subos use t \
  --cmd "exec '$inner_luban' __desktop - --size 1024x640" 2>"$RUNTIME_DIR/sandbox-desktop.log" || true)"
grep -q '"drew": true' <<<"$sbox" || fail "sandbox desktop: it never drew: $sbox $(tail -20 "$RUNTIME_DIR/sandbox-desktop.log")"
[[ "$(cat "$(sysdir t)/rootfs/root/sandbox.txt" 2>/dev/null)" == "SANDBOX-$arch" ]] \
  || fail "sandbox desktop: what was typed into its terminal did not run ($sbox)"
log "PASS ($arch): ISO (CD and drive), drive and qcow2 boot ($( [[ "$arch" == x86_64 ]] && echo "BIOS and UEFI" || echo UEFI)); an image entered; the environment as a machine, its proxy the only network; its desktop shown and typed into, as a machine and in a sandbox"
