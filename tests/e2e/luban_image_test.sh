#!/usr/bin/env bash
# Luban images (Luban design §A9, §B3.5): a luban-tiny environment made with
# `luban`, exported as a live ISO and as a drive image, each booted by qemu to
# stage-0 and its init -- on BIOS, and on UEFI where OVMF is. Then
# `luban try --proxy`: the machine's only network is the proxy.
#
# limine comes from its release (the binary files and its tool built here);
# xorriso, when the machine has it, makes the ISO a BIOS+UEFI hybrid.
#
# xtest: covers=LUBAN-ISO,LUBAN-DRIVE,LUBAN-TRY,LUBAN-BOOT-PROFILE requires=linux,qemu,bwrap,network
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
L() { ( cd /tmp && env -u XLINGS_ACTIVE_SUBOS -u XLINGS_SUBOS_MODE XLINGS_HOME="$H" DISPLAY= WAYLAND_DISPLAY= "$H/bin/luban" "$@" ); }

log "limine (release binary files, its tool built here)"
LIMINE="$H/data/xpkgs/xim-x-limine/12.9.3"
mkdir -p "$LIMINE/share/limine" "$LIMINE/bin"
retry curl -fsSL -o "$RUNTIME_DIR/limine.tgz" \
  https://github.com/limine-bootloader/limine/releases/download/v12.9.3/limine-binary.tar.gz
tar -xzf "$RUNTIME_DIR/limine.tgz" -C "$RUNTIME_DIR"
src="$RUNTIME_DIR/limine-binary"
cp "$src"/limine-bios-cd.bin "$src"/limine-bios.sys "$src"/limine-uefi-cd.bin "$src"/BOOTX64.EFI "$LIMINE/share/limine/"
cc -O2 -std=gnu11 -static -o "$LIMINE/bin/limine" "$src/limine.c" 2>/dev/null \
  || cc -O2 -std=gnu11 -o "$LIMINE/bin/limine" "$src/limine.c"

log "luban new t tiny"
n=0
until L new t tiny >"$RUNTIME_DIR/new.log" 2>&1; do
  n=$((n + 1)); [[ $n -ge 3 ]] && { tail -30 "$RUNTIME_DIR/new.log"; fail "luban new t tiny"; }
  X subos remove -y t >/dev/null 2>&1 || true; sleep 5
done
grep -q "subos created: t" "$RUNTIME_DIR/new.log" || fail "no creation report: $(tail -5 "$RUNTIME_DIR/new.log")"
# A machine image boots a kernel; the edition does not carry one (Luban
# design §A6) -- installed into the root, as a user would.
retry X install -y --subos t xim:linux-kernel >"$RUNTIME_DIR/kernel.log" 2>&1 \
  || { tail -20 "$RUNTIME_DIR/kernel.log"; fail "install the kernel into the root"; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd; do [[ -f "$f" ]] && { OVMF="$f"; break; }; done
# Runs a command (its own session) until its output shows the console
# prompt -- a booted system waits there forever -- then stops all of it.
until_console() {   # $1 log, then the command
  local log="$1"; shift
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
  [[ -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
  until_console "$RUNTIME_DIR/$name.log" qemu-system-x86_64 "${accel[@]}" -m 2048 -smp 2 -nographic -no-reboot -nic none "$@"
  tr -d '\r' < "$RUNTIME_DIR/$name.log" | grep -aq "luban-init: booting 'default'" \
    || { tr -d '\r' < "$RUNTIME_DIR/$name.log" | tail -25; fail "$name: did not reach stage-0"; }
  tr -d '\r' < "$RUNTIME_DIR/$name.log" | grep -aq "Please press Enter to activate this console" \
    || { tr -d '\r' < "$RUNTIME_DIR/$name.log" | tail -15; fail "$name: did not reach its init"; }
  log "  $name: stage-0 and init"
}

log "luban export t t.iso, booted"
L export t "$RUNTIME_DIR/t.iso" > "$RUNTIME_DIR/iso.log" 2>&1 || { cat "$RUNTIME_DIR/iso.log"; fail "export iso"; }
file "$RUNTIME_DIR/t.iso" | grep -q "ISO 9660" || fail "not an ISO: $(file "$RUNTIME_DIR/t.iso")"
boot iso-bios -cdrom "$RUNTIME_DIR/t.iso" -boot d
if [[ -n "$OVMF" ]] && command -v xorriso >/dev/null; then boot iso-uefi -bios "$OVMF" -cdrom "$RUNTIME_DIR/t.iso"; fi

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

log "luban export t <dir>/ (a directory is written with its slash)"
L export t "$RUNTIME_DIR/rootdir/" > "$RUNTIME_DIR/dir.log" 2>&1 || { cat "$RUNTIME_DIR/dir.log"; fail "export dir/"; }
# Its /usr may be a link that resolves inside the root, not on this host.
[[ ( -d "$RUNTIME_DIR/rootdir/usr" || -L "$RUNTIME_DIR/rootdir/usr" ) && -d "$RUNTIME_DIR/rootdir/etc" ]] \
  || { ls -la "$RUNTIME_DIR/rootdir" | head; fail "export dir/ wrote no root"; }
[[ ! -e "$RUNTIME_DIR/rootdir/rootdir" ]] || fail "export dir/ nested the root inside itself"

log "luban export t t.img (a drive), booted"
L export t "$RUNTIME_DIR/t.img" --size 1G > "$RUNTIME_DIR/drive.log" 2>&1 || { cat "$RUNTIME_DIR/drive.log"; fail "export drive"; }
if command -v sfdisk >/dev/null; then
  sfdisk -d "$RUNTIME_DIR/t.img" | grep -q "type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B" || fail "no system partition"
fi
boot drive-bios -drive "file=$RUNTIME_DIR/t.img,format=raw,if=virtio"
if [[ -n "$OVMF" ]]; then boot drive-uefi -bios "$OVMF" -drive "file=$RUNTIME_DIR/t.img,format=raw,if=virtio"; fi

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
log "PASS: ISO and drive boot (BIOS$( [[ -n "$OVMF" ]] && echo ", UEFI")); luban try --proxy"
