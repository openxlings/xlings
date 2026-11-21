#!/usr/bin/env bash
# Temporary verification for issue #724 (not for merge): the mechanisms the
# design proposes, measured on a Linux host with the PUBLISHED release.
#
#   linux.sh <release-tarball> <outdir> [experiments]
#
# Started as root (a container, or sudo on a runner); it makes an ordinary
# user and runs itself again as that user: every namespace below is an
# unprivileged one, as a user's machine gives.
set -u
tarball="$(realpath "$1")"; out="$2"; only="${3:-all}"
mkdir -p "$out"; out="$(realpath "$out")"
self="$(realpath "$0")"

if [[ $EUID -eq 0 ]]; then
    id hm >/dev/null 2>&1 || useradd -m -s /bin/bash hm
    mkdir -p /x724 && cp "$tarball" /x724/rel.tar.gz && cp "$self" /x724/linux.sh
    # the user's own directory for the outputs: a runner's work tree is not theirs to enter
    mkdir -p /x724/out; chown -R hm: /x724
    runuser -u hm -- env -i HOME=/home/hm USER=hm LOGNAME=hm LANG=C.UTF-8 TERM=dumb \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        bash /x724/linux.sh /x724/rel.tar.gz /x724/out "$only"
    cp -a /x724/out/. "$out"/; exit 0
fi

export XLINGS_NON_INTERACTIVE=1 NO_COLOR=1
cd "$HOME"
arch="$(uname -m)"
case "$arch" in
    x86_64)  FAMTAG=linux-x86_64-glibc;  STD=/lib64; LOADER=ld-linux-x86-64.so.2;  BUNPKG=bun-linux-x64 ;;
    aarch64) FAMTAG=linux-aarch64-glibc; STD=/lib;   LOADER=ld-linux-aarch64.so.1; BUNPKG=bun-linux-aarch64 ;;
    *) echo "unknown arch $arch"; exit 2 ;;
esac

log() { echo; echo "===== $* ====="; }
rec() { printf '%s\t%s\n' "$1" "$2" >> "$out/results.tsv"; echo "[result] $1 = $2"; }
# run <name> <cmd...>: output kept in <name>.txt, its tail shown, exit recorded.
run() {
    local name="$1"; shift
    log "$name: $*"
    ( "$@" ) > "$out/$name.txt" 2>&1; local rc=$?
    tail -n 40 "$out/$name.txt"
    rec "$name" "$rc"
    return 0
}
want() { [[ "$only" == all || " $only " == *" $1 "* ]]; }

# ---------------------------------------------------------------- setup
log "host"
{ . /etc/os-release; echo "os=$PRETTY_NAME"; echo "kernel=$(uname -r) arch=$arch"; id
  ls -ld /lib /lib64 /usr/lib /usr/lib64 2>&1; ls -l "$STD/$LOADER" 2>&1
  ls -l /lib/ld-musl-* /lib/libgcompat* 2>&1; echo "host libc: $( (ldd --version 2>&1 || true) | head -1)"
  echo "userns_restrict=$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo absent)"
} 2>&1 | tee "$out/host.txt"
if [[ -e "$STD/$LOADER" ]]; then HOSTFORM=C; elif [[ -e "$STD" ]]; then HOSTFORM=B; else HOSTFORM=A; fi
rec host-form "$HOSTFORM"

mkdir -p "$HOME/rel" && tar -xzf "$tarball" -C "$HOME/rel"
REL="$(echo "$HOME"/rel/*/)"; REL="${REL%/}"
run setup-self-install bash -c "cd '$REL' && ./bin/xlings self install </dev/null"
export PATH="$HOME/.xlings/bin:$HOME/.xlings/subos/current/bin:$PATH"
X="$HOME/.xlings/bin/xlings"
run setup-version "$X" --version
run setup-tools timeout 900 "$X" install -y xim:bwrap xim:busybox
BWRAP="$(find "$HOME/.xlings/data/xpkgs" -type f -name bwrap 2>/dev/null | head -1)"
BB="$(find "$HOME/.xlings/data/xpkgs" -type f -name busybox 2>/dev/null | head -1)"
echo "bwrap=$BWRAP busybox=$BB"; "$BWRAP" --version 2>&1 | head -1
FAM="$HOME/.xlings/abi/$FAMTAG"
# node by its own path: a package installed as a dependency has no command on PATH
run setup-node timeout 900 "$X" install -y xim:node
NODEBIN="$(dirname "$(find "$HOME/.xlings/data/xpkgs" -path '*xim-x-node*/bin/node' 2>/dev/null | head -1)")"
export PATH="$NODEBIN:$PATH"; echo "node at $NODEBIN: $(node --version 2>&1)"

# The host's root as a worker or the private domain presents it: each entry
# of / at its own path, read-only. `$1` (a directory) is left out.
hostroot() {
    local e
    for e in /*; do
        case "$e" in /proc|/dev|/sys|"$1") continue ;; esac
        if [[ -L "$e" ]]; then printf -- '--symlink\n%s\n%s\n' "$(readlink "$e")" "$e"
        elif [[ -e "$e" ]]; then printf -- '--ro-bind\n%s\n%s\n' "$e" "$e"; fi
    done
}

# ---------------------------------------------------------------- L1, L2
if want L1; then
    run L1-install-bun timeout 1200 "$X" install -y xim:bun@1.3.11
    run L1-after bash -c "ls -l '$FAM/' 2>&1 | head -8; echo; f=\$(ls -t '$HOME'/.xlings/logs/hooks/* 2>/dev/null | head -1); echo \"hook log: \$f\"; tail -n 25 \"\$f\" 2>/dev/null"
fi
B="$HOME/b"; BUN="$B/node_modules/@oven/$BUNPKG/bin/bun"
if want L2; then
    mkdir -p "$B"
    run L2-npm-ignore-scripts bash -c "cd '$B' && timeout 600 npm install --prefix '$B' --ignore-scripts --no-fund --no-audit bun@1.3.11"
    run L2-what-npm-chose bash -c "ls '$B/node_modules/@oven/' ; ls -l '$B/node_modules/bun/bin/' ; ls -l '$BUN'; head -c 4096 '$BUN' | tr -c '[:print:]' '\n' | grep -m1 'ld-'; node -p 'process.report.getReport().header.glibcVersionRuntime'"
    run L2-direct "$BUN" --version
fi

# ---------------------------------------------------------------- L3 link
mapfile -t HR < <(hostroot "$STD")
mapfile -t HRALL < <(hostroot "")
LINK=(--ro-bind "$(realpath "$FAM" 2>/dev/null)" "$STD")
if want L3 && [[ $HOSTFORM == A ]]; then
    run L3-link "$BWRAP" --unshare-user "${HR[@]}" "${LINK[@]}" --dev /dev --proc /proc "$BUN" --version
    run L3-link-symlink "$BWRAP" --unshare-user "${HR[@]}" --symlink "$FAM" "$STD" --dev /dev --proc /proc "$BUN" --version
fi

# ---------------------------------------------------------------- L4 merge
# The standard directory exists and holds no glibc loader: the host's content
# under a memory overlay, the family's entries the host has not added on top.
merge_args() {   # $1: the host directory; $2: bind | symlink
    local src="$1" f n
    MA=(--overlay-src "$src" --tmp-overlay "$STD")
    for f in "$FAM"/*; do
        n="$(basename "$f")"
        [[ -e "$src/$n" || -L "$src/$n" ]] && { echo "host keeps $n" >&2; continue; }
        if [[ $2 == bind ]]; then MA+=(--ro-bind "$(realpath "$f")" "$STD/$n"); else MA+=(--symlink "$FAM/$n" "$STD/$n"); fi
    done
}
inside="'$BUN' --version; echo bun-rc=\$?; ls '$STD' | wc -l; ls -l '$STD/$LOADER'; /bin/ls / >/dev/null && echo host-ls-ok; /bin/sh -c 'echo host-sh-ok'"
if want L4; then
    if [[ $HOSTFORM == B ]]; then SRC="$(realpath "$STD")"; else SRC="$HOME/fakestd"; mkdir -p "$SRC"; echo x > "$SRC/hostfile"; ln -sf nowhere "$SRC/dangling"; fi
    echo "merge source: $SRC ($(ls "$SRC" | wc -l) entries)"
    if [[ $HOSTFORM != C ]]; then
        merge_args "$SRC" bind 2> "$out/L4-kept.txt";    run L4-merge-bind    "$BWRAP" --unshare-user "${HR[@]}" "${MA[@]}" --dev /dev --proc /proc /bin/sh -c "$inside"
        merge_args "$SRC" symlink 2>/dev/null;           run L4-merge-symlink "$BWRAP" --unshare-user "${HR[@]}" "${MA[@]}" --dev /dev --proc /proc /bin/sh -c "$inside"
    fi
fi

# ---------------------------------------------------------------- L5 nested
# The private domain's namespace (uid 1000, the host's root entry by entry),
# and a worker's namespace made inside it.
if want L5 && [[ $HOSTFORM != C ]]; then
    if [[ $HOSTFORM == A ]]; then INNER=("${LINK[@]}"); else merge_args "$SRC" bind 2>/dev/null; INNER=("${MA[@]}"); fi
    run L5-nested "$BWRAP" --unshare-user --uid 1000 --gid 1000 "${HRALL[@]}" --dev /dev --proc /proc -- \
        "$BWRAP" --unshare-user "${HR[@]}" "${INNER[@]}" --dev /dev --proc /proc "$BUN" --version
fi

# ---------------------------------------------------------------- L7 replace
# A glibc host: the standard loader replaced by the environment's while the
# host's userland stays. What of the host's own programs still runs.
if want L7 && [[ $HOSTFORM == C ]]; then
    REALSTD="$(realpath "$STD")"
    if [[ $arch == x86_64 ]]; then REPL=(--ro-bind "$(realpath "$FAM")" "$REALSTD")
    else
        REPL=(--overlay-src "$REALSTD" --tmp-overlay "$REALSTD")
        for f in "$FAM"/*; do REPL+=(--ro-bind "$(realpath "$f")" "$REALSTD/$(basename "$f")"); done
    fi
    probe() {   # <mode> <extra bwrap args...>
        local mode="$1"; shift
        local cmds=(
            "/usr/bin/true"
            "/bin/busybox ls /"
            "/usr/bin/tar --version"
            "/usr/bin/ls /"
            "/usr/bin/id"
            "/bin/sh -c 'echo ok'"
            "/usr/bin/tar --version"
            "/usr/bin/git --version"
            "/usr/bin/perl -e 1"
            "/usr/bin/python3 -c 'import ssl,pwd,socket;pwd.getpwuid(0);print(socket.gethostbyname(\"github.com\"))'"
            "/usr/bin/curl -sI https://github.com"
            "$BUN --version"
        )
        local c rc
        for c in "${cmds[@]}"; do
            eval "set -- $c"
            "$BWRAP" --unshare-user "${HRALL[@]}" --tmpfs /tmp "${EXTRA[@]}" --dev /dev --proc /proc "$@" > "$out/.o" 2> "$out/.e"; rc=$?
            printf '%s\trc=%s\t%s\t%s\n' "$mode" "$rc" "$c" "$(head -c 240 "$out/.e" | tr '\n\t' '  ')" | tee -a "$out/L7-replace.tsv"
        done
    }
    log "L7: the host's programs under each loader"
    EXTRA=();                                                   probe host-loader
    EXTRA=("${REPL[@]}");                                       probe env-loader+host-cache
    EXTRA=("${REPL[@]}" --ro-bind /dev/null /etc/ld.so.cache);  probe env-loader+no-cache
    rm -f "$out/.o" "$out/.e"
    rec L7 done
fi

# ---------------------------------------------------------------- L6 hermetic
# The carrier's root, made here: a static xlings, busybox and bwrap, nothing
# of this host's userland. Packages installed in it without, then with, the
# standard loader directory linked to the home's runtime.
if want L6; then
    R="$HOME/hroot"
    mkdir -p "$R"/{etc/xlings,xlings/bin,bin,sbin,usr/bin,usr/sbin,root,tmp,proc,sys,dev,run}
    cp "$REL/bin/xlings" "$R/xlings/bin/xlings"; [[ -f "$REL/bin/luban" ]] && cp "$REL/bin/luban" "$R/xlings/bin/luban"
    cp "$BB" "$R/bin/busybox"; ln -sf busybox "$R/bin/sh"; cp "$BWRAP" "$R/usr/bin/bwrap"
    ln -sf /xlings/bin/xlings "$R/usr/bin/xlings"
    printf 'root:x:0:0:root:/root:/bin/sh\n' > "$R/etc/passwd"; printf 'root:x:0:\n' > "$R/etc/group"
    cat /etc/resolv.conf > "$R/etc/resolv.conf"; echo 'xlings-carrier' > "$R/etc/hostname"
    echo '{ "home": "/xlings" }' > "$R/etc/xlings/root.json"; echo '{}' > "$R/xlings/.xlings.json"
    chmod 1777 "$R/tmp"
    H() { env -i "$BWRAP" --unshare-user --uid 0 --gid 0 --bind "$R" / --dev /dev --proc /proc \
            --setenv HOME /root --setenv XLINGS_HOME /xlings --setenv XLINGS_NON_INTERACTIVE 1 --setenv NO_COLOR 1 \
            --setenv PATH /xlings/bin:/xlings/subos/current/bin:/usr/bin:/bin:/usr/sbin:/sbin "$@"; }
    run L6-applets H /bin/busybox --install -s
    run L6-version H /xlings/bin/xlings --version
    LIST="${X724_LIST:-xim:bun@1.3.11 xim:esbuild xim:codex xim:pnpm xim:openclaw xim:claude xim:opencode xim:pi xim:node xim:python xim:uv xim:cmake xim:ninja xim:git xim:ripgrep xim:fish xim:go xim:gcc}"
    failed=()
    for p in $LIST; do
        n="${p//[:@]/_}"
        log "L6a (no standard loader): $p"
        H timeout 1500 /xlings/bin/xlings install -y "$p" > "$out/L6a-$n.txt" 2>&1; rc=$?
        tail -n 6 "$out/L6a-$n.txt"; rec "L6a:$p" "$rc"
        [[ $rc -ne 0 ]] && failed+=("$p")
    done
    ln -s "/xlings/abi/$FAMTAG" "$R$STD"
    run L6-link H /bin/sh -c "ls -ld $STD; ls -l $STD/$LOADER"
    for p in "${failed[@]}"; do
        n="${p//[:@]/_}"
        log "L6b (linked): $p"
        H timeout 1500 /xlings/bin/xlings install -y "$p" > "$out/L6b-$n.txt" 2>&1; rc=$?
        tail -n 12 "$out/L6b-$n.txt"; rec "L6b:$p" "$rc"
    done
    run L6-hooklogs H /bin/sh -c 'for f in /xlings/logs/hooks/*; do echo "--- $f"; tail -n 12 "$f"; done 2>/dev/null | tail -n 150'
fi

# ---------------------------------------------------------------- L9 env userland
# A worker over THIS host's home whose userland is the environment's (busybox
# and the home's runtime), none of the host's: bun's own postinstall in it,
# without and with the standard loader directory.
if want L9 && [[ -d "$HOME/hroot/bin" ]]; then
    R="$HOME/hroot"
    EU=(--unshare-user --tmpfs / --ro-bind "$R/bin" /bin --ro-bind "$R/sbin" /sbin --ro-bind "$R/usr" /usr
        --ro-bind "$R/etc" /etc --dir /tmp --dev /dev --proc /proc --bind "$HOME" "$HOME"
        --setenv HOME "$HOME" --setenv PATH "$NODEBIN:/bin:/usr/bin:/sbin:/usr/sbin")
    post="rm -rf '$HOME/b9'; mkdir -p '$HOME/b9'; cd '$HOME/b9' && npm install --no-fund --no-audit bun@1.3.11 2>&1 | tail -n 12; ls node_modules/@oven/ 2>&1; node_modules/.bin/bun --version; echo bun-rc=\$?"
    run L9a-env-userland-no-loader env -i "$BWRAP" "${EU[@]}" /bin/sh -c "$post"
    run L9b-env-userland-linked    env -i "$BWRAP" "${EU[@]}" --symlink "$FAM" "$STD" /bin/sh -c "$post; ls -l $STD/$LOADER"
fi

# ---------------------------------------------------------------- L8 two glibc
if want L8 && [[ $arch == x86_64 && $HOSTFORM == C ]]; then
    L="$HOME/.xlings/bin/luban"; [[ -x "$L" ]] || { run L8-get-luban timeout 600 "$X" install -y luban; L="$(command -v luban)"; }
    probe='for p in /lib64/ld-linux-x86-64.so.2 /usr/lib/ld-linux-x86-64.so.2 /usr/lib/libc.so.6 /xlings/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2 /xlings/abi/linux-x86_64-glibc/libc.so.6; do echo "$p => $(readlink -f $p)"; done; ls /xlings/data/xpkgs 2>/dev/null | grep -i glibc'
    run L8-new timeout 1500 "$L" new v6 --edition tiny -y
    run L8-before "$L" run v6 -- /bin/sh -c "$probe"
    run L8-install-older timeout 900 "$L" run v6 -- xlings install -y xim:glibc@2.39; rec L8-install-older-exit "$(tail -n 3 "$out/L8-install-older.txt" | tr '\n' ' ')"
    run L8-after "$L" run v6 -- /bin/sh -c "$probe"
    # a second glibc that IS relocated, then made the active one
    run L8-install-2442 timeout 900 "$L" run v6 -- xlings install -y xim:glibc@2.44.2
    run L8-after-2442 "$L" run v6 -- /bin/sh -c "$probe"
    run L8-use-2442 timeout 600 "$L" run v6 -- xlings use glibc 2.44.2
    run L8-after-use "$L" run v6 -- /bin/sh -c "$probe; ls -l /xlings/abi; /bin/sh -c 'echo shell-ok'; busybox true && echo busybox-ok"
    run L8-status "$L" status v6
    run L8-check "$L" status v6 --check
fi

log "results"
cat "$out/results.tsv"
exit 0
