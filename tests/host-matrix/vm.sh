#!/usr/bin/env bash
# The host matrix on another distribution's kernel and LSM (Luban OS design
# part 2 §5.2, the K layer): its own cloud image booted under KVM, the same
# run.sh inside as that machine's administrator. A container shares the
# runner's kernel and AppArmor, so SELinux (Fedora), an unrestricted Debian
# and Arch's newest kernel are only reachable this way.
#
#   tests/host-matrix/vm.sh <fedora|debian-12|arch> --tarball <f> --evidence <dir>
#                           [--index <dir>] [--scenarios "a b"]
#
# Needs qemu-system-x86_64, qemu-img, cloud-localds (cloud-image-utils),
# ssh, and /dev/kvm. Images are cached in $HM_CACHE (default ~/.cache/hm-images)
# and checked against the distribution's published checksums.
set -euo pipefail

distro="$1"; shift
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tarball="" evidence="" index="" scenarios=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tarball) tarball="$(realpath "$2")"; shift 2 ;;
        --evidence) evidence="$2"; shift 2 ;;
        --index) index="$(realpath "$2")"; shift 2 ;;
        --scenarios) scenarios="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -f "$tarball" && -n "$evidence" ]] || { echo "usage: vm.sh <distro> --tarball <f> --evidence <dir>" >&2; exit 2; }
mkdir -p "$evidence"; evidence="$(realpath "$evidence")"
cache="${HM_CACHE:-$HOME/.cache/hm-images}"; mkdir -p "$cache"
work="$(mktemp -d)"
port=$((22000 + RANDOM % 1000))
log() { printf '[vm %s] %s\n' "$distro" "$*" >&2; }

case "$distro" in
    fedora)
        # The current release; its point-release names come from the listing
        # (an older release moves to the archive and its URL 404s).
        base=https://download.fedoraproject.org/pub/fedora/linux/releases/44/Cloud/x86_64/images
        listing="$(curl -fsSL --retry 3 "$base/")"
        image="$(grep -oE 'Fedora-Cloud-Base-Generic-[0-9]+-[0-9.]+\.x86_64\.qcow2' <<<"$listing" | sort -u | tail -1)"
        sums="$base/$(grep -oE 'Fedora-Cloud-[0-9]+-[0-9.]+-x86_64-CHECKSUM' <<<"$listing" | sort -u | tail -1)"; sumtool=sha256sum
        prepare='sudo dnf -y -q install python3 curl util-linux sudo tar gzip findutils >/dev/null' ;;
    debian-12)
        base=https://cloud.debian.org/images/cloud/bookworm/latest
        image=debian-12-genericcloud-amd64.qcow2
        sums="$base/SHA512SUMS"; sumtool=sha512sum
        prepare='sudo DEBIAN_FRONTEND=noninteractive apt-get -qq update && sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y python3 curl util-linux sudo >/dev/null' ;;
    arch)
        base=https://geo.mirror.pkgbuild.com/images/latest
        image=Arch-Linux-x86_64-cloudimg.qcow2
        sums="$base/$image.SHA256"; sumtool=sha256sum
        prepare='sudo pacman -Sy --noconfirm --needed python curl sudo >/dev/null' ;;
    *) echo "unknown distro: $distro" >&2; exit 2 ;;
esac

# The image, checked against what the distribution publishes.
curl -fsSL --retry 3 -o "$work/sums" "$sums"
want="$(grep -E "[ (*]$image\)?( |$)" "$work/sums" | grep -Eo '[0-9a-f]{64,128}' | head -1)"
[[ -n "$want" ]] || { log "no checksum for $image in $sums"; exit 1; }
if [[ ! -f "$cache/$image" ]] || ! echo "$want  $cache/$image" | $sumtool -c - >/dev/null 2>&1; then
    log "downloading $image"
    curl -fsSL --retry 3 -o "$cache/$image.part" "$base/$image"
    echo "$want  $cache/$image.part" | $sumtool -c - >/dev/null || { log "checksum mismatch"; exit 1; }
    mv "$cache/$image.part" "$cache/$image"
fi
qemu-img create -q -f qcow2 -b "$cache/$image" -F qcow2 "$work/disk.qcow2" 30G

# An administrator with sudo; run.sh makes the ordinary user inside.
ssh-keygen -q -t ed25519 -N '' -f "$work/key"
cat > "$work/user-data" <<EOF
#cloud-config
users:
  - name: admin
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys: [$(cat "$work/key.pub")]
EOF
printf 'instance-id: hm-%s\nlocal-hostname: hm-%s\n' "$distro" "$distro" > "$work/meta-data"
cloud-localds "$work/seed.img" "$work/user-data" "$work/meta-data"

log "booting (ssh on 127.0.0.1:$port)"
qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 4 -display none \
    -drive "file=$work/disk.qcow2,if=virtio" -drive "file=$work/seed.img,if=virtio,format=raw" \
    -nic "user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:$port-:22" -serial "file:$evidence/console-$distro.log" \
    -pidfile "$work/qemu.pid" -daemonize
trap 'kill "$(cat "$work/qemu.pid" 2>/dev/null)" 2>/dev/null || true; rm -rf "$work"' EXIT
# virtio-net: Debian's genericcloud kernel has no e1000 (qemu's default NIC).
SSH=(ssh -q -i "$work/key" -p "$port" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 admin@127.0.0.1)
SCP=(scp -q -i "$work/key" -P "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
for i in $(seq 120); do
    "${SSH[@]}" true 2>/dev/null && break
    sleep 5
    if [[ $i -eq 120 ]]; then
        log "no ssh"; tail -40 "$evidence/console-$distro.log"
        "${SSH[@]/-q/-v}" true 2>&1 | tail -20
        exit 1
    fi
done
log "up: $("${SSH[@]}" 'uname -r; cat /sys/fs/selinux/enforce 2>/dev/null || true' | tr '\n' ' ')"
"${SSH[@]}" "$prepare"
"${SSH[@]}" 'mkdir -p hm-in'
"${SSH[@]}" 'mkdir -p hm-in/suite'
"${SCP[@]}" -r "$here/." "admin@127.0.0.1:hm-in/suite/"
"${SCP[@]}" "$tarball" "admin@127.0.0.1:hm-in/xlings.tar.gz"
extra=()
if [[ -n "$index" ]]; then
    tar -czf "$work/index.tar.gz" -C "$index" .
    "${SCP[@]}" "$work/index.tar.gz" "admin@127.0.0.1:hm-in/index.tar.gz"
    "${SSH[@]}" 'mkdir -p hm-in/index && tar -xzf hm-in/index.tar.gz -C hm-in/index'
    extra+=(--index hm-in/index)
fi
[[ -n "$scenarios" ]] && extra+=(--scenarios "'$scenarios'")
set +e
"${SSH[@]}" "sudo bash hm-in/suite/run.sh --tarball hm-in/xlings.tar.gz --evidence hm-out ${extra[*]}"
rc=$?
set -e
"${SCP[@]}" -r "admin@127.0.0.1:hm-out/." "$evidence/" || true
exit $rc
