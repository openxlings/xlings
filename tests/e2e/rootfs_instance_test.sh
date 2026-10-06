#!/usr/bin/env bash
# A SubOS presented as / (design part 2 §3, §6): Luban roots made from the
# published editions, entered in bwrap, changed from inside, rolled back.
#
#   1. luban-tiny: the projection's shape (merged-usr, /usr at the current
#      generation, links straight into payloads), its kernel, its /etc;
#   2. entered: uid 0, the root's own userland and nothing of the host's;
#   3. an install from inside goes through the broker and is in /usr at once;
#      rollback moves the pointer back;
#   4. the role table at the command line: a boot entry is not removed, a
#      view has no root to boot, export or roll back;
#   5. luban-core, `from` tiny: the chain merged (upper wins), GNU tools over
#      busybox, a C program compiled and run inside, mapping nothing of the host;
#   6. every form-X executable of core resolves its closure in an empty root,
#      and the gcc shim (an alias) runs with no /bin/sh at all;
#   7. what is searched (interpreters, run paths, shebangs) names only this
#      home -- no build machine;
#   8. fetch = layer: a root's own scope; a view refuses it.
#
# xtest: covers=ROOT-PROJECT,INST-ROOTFS,ROOT-NO-HOST,ROOT-ROLE-TABLE,ROOT-ROLLBACK,LUBAN-TINY,LUBAN-CORE,LUBAN-FROM-CHAIN,ECO-HOST-INDEPENDENT,SHIM-NO-SHELL,DOM-PREFIX,PERM-FETCH-LAYER,ROOT-ETC-FACTORY requires=linux,bwrap,network
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/rootfs_lib.sh"

BIN="$(find_xlings_bin)"
RUNTIME_DIR="$(runtime_home_dir rootfs_instance)"
rm -rf "$RUNTIME_DIR"; mkdir -p "$RUNTIME_DIR"
rootfs_home "$RUNTIME_DIR/home"
H="$RH"

log "1. luban-tiny"
rootfs_new tiny subos:luban-tiny "$RUNTIME_DIR/new-tiny.log" || fail "subos new --rootfs --from subos:luban-tiny"
S="$H/subos/tiny"
[[ "$(readlink "$S/rootfs/usr")" == "$S/root/usr" ]] || fail "rootfs/usr is $(readlink "$S/rootfs/usr")"
[[ "$(readlink "$S/root")" == root.gen/* ]] || fail "the pointer is $(readlink "$S/root")"
for l in bin:usr/bin sbin:usr/bin lib:usr/lib lib64:usr/lib64; do
  [[ "$(readlink "$S/rootfs/${l%%:*}")" == "${l#*:}" ]] || fail "/${l%%:*} is not -> ${l#*:}"
done
sh_target="$(readlink "$S/root/usr/bin/sh")"
[[ "$sh_target" == "$H/data/xpkgs/xim-x-busybox/"*"/bin/sh" ]] \
  || fail "/usr/bin/sh is not a direct link into busybox: $sh_target"
[[ -e "$S/root/usr/lib/modules/6.8.0-71-generic/vmlinuz" ]] || fail "no kernel at /usr/lib/modules"
[[ -L "$S/rootfs/etc/inittab" && -x "$S/rootfs/etc/init.d/rcS" ]] || fail "factory /etc missing"
grep -q '^ID=luban' "$S/rootfs/etc/os-release" || fail "no luban os-release"
echo "mine" > "$S/rootfs/etc/hostname.local"
grep -q '^root:x:0:0:' "$S/rootfs/etc/passwd" || fail "no root user"
grep -q '^nobody:' "$S/rootfs/etc/passwd" || fail "sysusers did not add nobody"

log "2. entered as its own root"
out="$(X subos exec tiny -- /bin/sh -c 'id -u; . /etc/os-release; echo "$ID/$VARIANT_ID"; test -e /usr/bin/apt-get && echo HOST-USERLAND; echo done' 2>&1)" \
  || fail "exec failed: $out"
grep -qx 0 <<<"$out" || fail "not uid 0 inside: $out"
grep -qx 'luban/tiny' <<<"$out" || fail "not the root's os-release: $out"
! grep -q HOST-USERLAND <<<"$out" || fail "the host's /usr is visible"
audit="$(X subos log tiny --json 2>/dev/null || true)"
grep -q '"session-start"' <<<"$audit" || fail "no audit for the root"

log "3. install from inside, then roll back"
before="$(readlink "$S/root")"
out="$(X subos exec tiny -- /bin/sh -c 'xlings install -y make >/dev/null 2>&1; make --version | head -1' 2>&1)" \
  || fail "install inside failed: $out"
grep -q 'GNU Make' <<<"$out" || fail "make not in /usr/bin after installing it inside: $out"
after="$(readlink "$S/root")"
[[ "$after" != "$before" ]] || fail "no new generation"
X subos rollback tiny >/dev/null 2>&1 || fail "rollback failed"
[[ "$(readlink "$S/root")" != "$after" ]] || fail "rollback did not move the pointer"
[[ ! -e "$S/root/usr/bin/make" ]] || fail "make still in the rolled-back generation"
X subos rollback tiny --to "${after##*/}" >/dev/null 2>&1 || fail "rollback --to failed"
[[ -e "$S/root/usr/bin/make" ]] || fail "rollback --to did not bring make back"

log "4. the role table"
X subos boot tiny >/dev/null 2>&1 || fail "subos boot tiny"
out="$(X subos remove -y tiny 2>&1)" && fail "a boot entry was removed"
grep -q "boot entry" <<<"$out" || fail "the refusal does not say why: $out"
for verb in "rollback default" "boot default" "export default --tar $RUNTIME_DIR/x.tar"; do
  out="$(X subos $verb 2>&1)" && fail "subos $verb on a view succeeded"
  grep -q "rootfs\|root" <<<"$out" || fail "subos $verb: no reason given: $out"
done

log "5. luban-core from tiny"
rootfs_new core subos:luban-core "$RUNTIME_DIR/new-core.log" || fail "subos new --from subos:luban-core"
C="$H/subos/core"
grep -q '^VARIANT_ID=core' "$C/rootfs/etc/os-release" || fail "core's os-release did not win"
[[ -e "$C/rootfs/etc/inittab" ]] || fail "tiny's inittab did not come down the chain"
cat > "$C/rootfs/root/hello.c" <<'EOF'
#include <stdio.h>
int main(void) {
    FILE *m = fopen("/proc/self/maps", "r");
    char line[4096];
    while (m && fgets(line, sizeof line, m)) {
        char *p = line; while (*p && *p != '/') ++p;
        if (*p) fputs(p, stdout);
    }
    puts("hello from luban-core");
    return 0;
}
EOF
out="$(X subos exec core -- /bin/sh -c 'ls --version | head -1; bash -c "echo bash-ok"; cd /root && gcc hello.c -o hello && ./hello' 2>&1)" \
  || fail "core: $out"
grep -q 'GNU coreutils' <<<"$out" || fail "ls is not GNU coreutils: $out"
grep -q 'bash-ok' <<<"$out" || fail "no bash: $out"
grep -q 'hello from luban-core' <<<"$out" || fail "the program did not run: $out"
# What it mapped: the root's own files and the home's payloads, nothing else.
foreign="$(grep '^/' <<<"$out" | grep -v "^$H/\|^/root/\|^/usr/" || true)"
[[ -z "$foreign" ]] || fail "mapped from outside the root: $foreign"

log "6. closures in an empty root; the gcc shim without a shell"
loader="$(readlink -f "$C/root/usr/lib64/ld-linux-x86-64.so.2")"
checked=0
while IFS= read -r f; do
  interp="$(readelf -lW "$f" 2>/dev/null | sed -n 's/.*interpreter: \(.*\)]/\1/p' || true)"
  [[ "$interp" == "$H/"* ]] || continue
  res="$(bwrap --tmpfs /tmp --ro-bind "$H" "$H" --proc /proc --dev /dev "$loader" --list "$f" 2>&1)" \
    || fail "$f: $res"
  grep -q "not found" <<<"$res" && fail "$f: $res"
  outside="$(grep '=>' <<<"$res" | grep -v "=> $H/" || true)"
  [[ -z "$outside" ]] || fail "$f resolves outside the home: $outside"
  checked=$((checked + 1))
done < <(for l in "$C"/root/usr/bin/*; do readlink -f "$l"; done | sort -u)
(( checked > 50 )) || fail "only $checked form-X executables checked"
log "  $checked executables, every closure inside the home"
printf 'int main(void){return 0;}\n' > "$RUNTIME_DIR/a.c"
# core's own shims, in a root with nothing but the home: no /bin, no /usr.
out="$(bwrap --tmpfs /tmp --bind "$H" "$H" --ro-bind "$RUNTIME_DIR/a.c" /tmp/a.c --proc /proc --dev /dev \
        --clearenv --setenv PATH "$C/bin" --setenv XLINGS_HOME "$H" --setenv XLINGS_ACTIVE_SUBOS core \
        --setenv HOME /tmp "$C/root/usr/bin/busybox" sh -c \
        'gcc -c /tmp/a.c -o /tmp/a.o && echo shim-ok; ls /bin >/dev/null 2>&1 || echo no-bin' 2>&1)" || true
grep -q shim-ok <<<"$out" || fail "the gcc shim did not compile without /bin/sh: $out"
grep -q no-bin <<<"$out" || fail "the root had a /bin after all: $out"

log "7. only this home is named where things are looked up"
bad=""
while IFS= read -r f; do
  if [[ "$(head -c 4 "$f" 2>/dev/null)" == $'\x7fELF' ]]; then
    for p in $(readelf -lW "$f" 2>/dev/null | sed -n 's/.*interpreter: \(.*\)]/\1/p') \
             $(readelf -dW "$f" 2>/dev/null | sed -n 's/.*R[UN]*PATH.*\[\(.*\)\]/\1/p' | tr ':' ' '); do
      [[ "$p" == "$H/"* || "$p" == '$ORIGIN'* ]] || bad+="$f: $p"$'\n'
    done
  elif [[ "$(head -c 2 "$f" 2>/dev/null)" == '#!' ]]; then
    interp="$(head -1 "$f" | sed -E 's/^#! *([^ ]+).*/\1/')"
    case "$interp" in
      "$H/"*|/bin/sh|/bin/bash|/usr/bin/env|/usr/bin/perl|/usr/bin/python3) ;;
      *) bad+="$f: shebang $interp"$'\n' ;;
    esac
  fi
done < <(for l in "$C"/root/usr/bin/*; do readlink -f "$l"; done | sort -u)
[[ -z "$bad" ]] || fail "names outside this home:"$'\n'"$bad"

log "8. fetch = layer"
X subos config core --fetch layer >/dev/null 2>&1 || fail "fetch=layer refused on a root"
out="$(X subos exec core -- /bin/sh -c 'xlings install -y xz >/dev/null 2>&1; xz --version | head -1' 2>&1)" \
  || fail "install into the layer: $out"
grep -q 'XZ Utils' <<<"$out" || fail "xz not in the root: $out"
X subos new aview >/dev/null 2>&1 || fail "subos new aview"
X subos config aview --fetch layer >/dev/null 2>&1 || fail "subos config --fetch layer"
out="$(X subos exec aview -- true 2>&1)" && fail "a view with fetch=layer was entered"
grep -q "system scope" <<<"$out" || fail "no reason given: $out"

log "PASS: roots are made, entered, changed from inside and rolled back"
