#!/usr/bin/env bash
# A root exported as an image and run as a machine's whole userland (design
# part 2 §5, §6, §11): deployment R in a container, where xlings is the only
# package manager there is.
#
#   1. `subos export --tar`, `docker import`: the image's /usr is the
#      projection, its xlings the static build, its glibc ours;
#   2. inside, xlings installs with the root's own tools (busybox, patchelf):
#      nothing of a host under it;
#   3. a host-built program (PT_INTERP /lib64/ld-linux-x86-64.so.2) finds its
#      libraries through /lib64 and the logical-root loader;
#   4. a half-written /usr (libc gone from the current generation): programs
#      fail, the static xlings rolls the root back, they run again;
#   5. no patchelf and no way to get one: an install fails and says why,
#      instead of reporting a payload it could not relocate as installed;
#   6. both layouts: single (the builder's home path) and multi (/xlings,
#      built in an owner-private namespace), each recorded in the image and running;
#   7. self update in deployment R is a new generation: /usr/bin/xlings
#      becomes the published payload, and a rollback brings the previous back.
#
# xtest: covers=EXPORT-OCI,DOM-BOOTSTRAP,ROOT-LIBSEARCH,ROOT-SURVIVES-GLIBC,DOM-ELFPATCH-FAILCLOSED,DOM-LAYOUTS,DOM-BUILD-INSIDE,ROOT-SELF-UPDATE requires=linux,docker,sudo,network
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/rootfs_lib.sh"

BIN="$(find_xlings_bin)"
RUNTIME_DIR="$(runtime_home_dir rootfs_image)"
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"
IMG=xlings-e2e-luban
cleanup() { docker rm -f warm >/dev/null 2>&1 || true; docker rmi -f "$IMG" "$IMG-warm" "$IMG-multi" >/dev/null 2>&1 || true; }
trap cleanup EXIT

rootfs_home "$RUNTIME_DIR/home"
H="$RH"
rootfs_new tiny subos:luban-tiny "$RUNTIME_DIR/new.log" || fail "subos new --from subos:luban-tiny"

log "1. export --tar, docker import"
X subos export tiny --tar "$RUNTIME_DIR/tiny.tar.gz" >"$RUNTIME_DIR/export.log" 2>&1 \
  || { cat "$RUNTIME_DIR/export.log"; fail "export --tar"; }
docker import "$RUNTIME_DIR/tiny.tar.gz" "$IMG" >/dev/null
D() { docker run --rm "$@"; }
out="$(D --network none "$IMG" /bin/sh -c '. /etc/os-release; echo "os=$ID/$VARIANT_ID"; readlink /usr; xlings --version; getconf GNU_LIBC_VERSION; cat /etc/xlings/root.json' 2>&1)" \
  || fail "the image does not run: $out"
grep -q 'os=luban/tiny' <<<"$out" || fail "not luban tiny: $out"
grep -q "$H/subos/default/root/usr" <<<"$out" || fail "/usr is not the default SubOS's projection: $out"
grep -q "^xlings " <<<"$out" || fail "no xlings in the image: $out"
grep -q 'glibc 2.44' <<<"$out" || fail "not the xlings glibc: $out"

log "2. xlings installs inside, with the root's own tools"
out="$(D "$IMG" /bin/sh -c 'xlings install -y xz 2>&1 | tail -3; xz --version | head -1; readlink -f /usr/bin/xz' 2>&1)" \
  || fail "install inside the image: $out"
grep -q 'XZ Utils' <<<"$out" || fail "xz did not install or run: $out"
grep -q "$H/data/xpkgs/xim-x-xz/" <<<"$out" || fail "xz is not linked from its payload: $out"

log "3. a host-built program in the root"
cp "$(command -v true)" "$RUNTIME_DIR/host-true"
out="$(D -v "$RUNTIME_DIR/host-true:/root/host-true:ro" "$IMG" /bin/sh -c \
        '/root/host-true && echo host-binary-ran' 2>&1)" || true
grep -q host-binary-ran <<<"$out" || fail "a host-built binary did not run against /lib64: $out"

log "4. a half-written /usr, rolled back by the static xlings"
# A newer generation first, so there is a good one to go back to.
out="$(D -e H="$H" "$IMG" /bin/sh -c '
    xlings install -y zlib >/dev/null 2>&1
    rm -f "$H/subos/default/root/usr/lib/libc.so.6"
    getconf GNU_LIBC_VERSION >/dev/null 2>&1 && echo still-works || echo broken
    xlings subos rollback default && getconf GNU_LIBC_VERSION >/dev/null 2>&1 && echo restored' 2>&1)" || true
grep -q broken <<<"$out" || fail "removing libc from /usr broke nothing: $out"
grep -q restored <<<"$out" || fail "rollback did not restore the root: $out"

log "5. no patchelf, none to fetch"
docker run --name warm "$IMG" /bin/sh -c 'xlings install -y zlib >/dev/null 2>&1 && xlings remove -y zlib >/dev/null 2>&1' \
  || fail "warming the download cache"
docker commit warm "$IMG-warm" >/dev/null
out="$(D --network none "$IMG-warm" /bin/sh -c 'xlings remove -y patchelf >/dev/null 2>&1; xlings install -y zlib; echo rc=$?' 2>&1)" || true
grep -q 'rc=0' <<<"$out" && fail "zlib reported installed without a patchelf to relocate it: $out"
grep -q 'patchelf' <<<"$out" || fail "the failure does not name patchelf: $out"

log "6. the multi layout: the system home at /xlings"
rootfs_home "$RUNTIME_DIR/domain-owner"
X subos new luban --rootfs --domain /xlings --from subos:luban-tiny >"$RUNTIME_DIR/new-multi.log" 2>&1 \
  || { cat "$RUNTIME_DIR/new-multi.log"; fail "multi: namespace producer"; }
X subos export luban --tar "$RUNTIME_DIR/multi.tar.gz" >/dev/null 2>&1 || fail "multi: export"
docker import "$RUNTIME_DIR/multi.tar.gz" "$IMG-multi" >/dev/null
out="$(D --network none "$IMG-multi" /bin/sh -c 'cat /etc/xlings/root.json /xlings/.xlings-home; readlink /usr; xz --version 2>/dev/null; ls /usr/bin/sh' 2>&1)" \
  || fail "multi image: $out"
grep -q '"home": "/xlings"' <<<"$out" || fail "multi: root.json: $out"
grep -q '"root_layout": "multi"' <<<"$out" || fail "multi: marker: $out"
grep -q '^/xlings/subos/default/root/usr' <<<"$out" || fail "multi: /usr: $out"
grep -q '"root_layout": "single"' "$H/.xlings-home" 2>/dev/null && fail "the builder's home was marked"
marker="$(tar -xzOf "$RUNTIME_DIR/tiny.tar.gz" ".${H}/.xlings-home" 2>/dev/null || true)"
grep -q '"root_layout": "single"' <<<"$marker" || fail "single: the first image is not marked single"

log "7. self update is a generation"
out="$(D "$IMG" /bin/sh -c '
    before=$(readlink -f /usr/bin/xlings)
    xlings self update >/tmp/u.log 2>&1 || { tail -5 /tmp/u.log; }
    after=$(readlink -f /usr/bin/xlings)
    echo "before=$before"; echo "after=$after"
    xlings subos rollback default >/dev/null 2>&1
    echo "rolled=$(readlink -f /usr/bin/xlings)"' 2>&1)" || true
before="$(sed -n 's/^before=//p' <<<"$out")"; after="$(sed -n 's/^after=//p' <<<"$out")"
rolled="$(sed -n 's/^rolled=//p' <<<"$out")"
[[ "$after" == *"/data/xpkgs/xim-x-xlings/"* ]] || fail "self update did not make /usr/bin/xlings the payload: $out"
[[ "$rolled" == "$before" ]] || fail "rollback did not bring the previous xlings back: $out"

log "PASS: an exported root is a machine whose only package manager is xlings"
