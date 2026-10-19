# Luban as a person meets it, on a machine that has just installed xlings.
#
# Sourced by smoke.sh: each suite_luban_* is one leg of
# .github/workflows/xlings-ci-luban-fresh.yml, run against the RELEASED
# client (and, with XIM_PKGINDEX_REF, an unmerged index). What is checked is
# what a person sees: the conclusion, the next step, the prompt, the file.

# capture <what> <command...> -- run it, show its output indented, keep it in
# $OUT; a non-zero exit fails the suite.
capture() {
    local what="$1"; shift
    log "\$ $*"
    if ! OUT="$("$@" 2>&1)"; then
        printf '%s\n' "$OUT" | sed 's/^/      /'
        fail "$what: '$*' exited non-zero"
    fi
    printf '%s\n' "$OUT" | sed 's/^/      /'
}

# refuses <what> <command...> -- it must fail, saying why; its output in $OUT.
refuses() {
    local what="$1"; shift
    log "\$ $*"
    local rc=0
    OUT="$("$@" 2>&1)" || rc=$?
    printf '%s\n' "$OUT" | sed 's/^/      /'
    [ "$rc" -ne 0 ] || fail "$what: '$*' succeeded"
    [ "$rc" -lt 128 ] || fail "$what: '$*' died (exit $rc)"
    [ -n "$OUT" ] || fail "$what: '$*' failed without a word"
    ok "$what"
}

# says <what> <text> -- $OUT has it.
says() {
    grep -qF -- "$2" <<<"$OUT" || fail "$1: expected '$2' in the output above"
    ok "$1"
}

# session <seconds> <input> <luban enter args...> -- a terminal session, as a
# person has one: what is typed goes in once it has settled; its screen in $OUT.
session() {
    local settle="$1" input="$2"; shift 2
    log "\$ luban enter $* (typed: $input)"
    OUT="$( { sleep "$settle"; printf '%s\n' "$input"; sleep 5; } 2>/dev/null \
        | timeout 900 script -qfec "luban enter $*" /dev/null 2>&1 | tr -d '\r' || true)"
    printf '%s\n' "$OUT" | sed 's/^/      /'
}

# ── console: nano, tiny and core, made and used ──────────────────────

suite_luban_console() {
    [ "$(uname -s)" = Linux ] || fail "the console suite is Linux's: Luban systems are Linux systems"
    section "luban, before anything: where to start"
    capture "luban" luban
    says "an empty home names the first command" "luban new"
    capture "luban setup" luban setup -y
    ok "the one-time setup is one step"
    capture "luban search" luban search
    says "the index's distributions" "distributions"
    says "and its desktops" "desktops"

    local e
    for e in nano tiny core; do
        section "luban new e-$e --edition $e"
        capture "new $e" luban new "e-$e" --edition "$e"
        says "e-$e is made" "e-$e is ready"
    done
    capture "ls" luban ls
    for e in nano tiny core; do says "ls lists e-$e" "e-$e"; done
    capture "status" luban status e-tiny
    # "Luban tiny 2026.10.18.1" from 2026.10.18.1; "tiny@<version>" before.
    grep -qE "Luban tiny [0-9]|tiny@" <<<"$OUT" || fail "status names the edition: expected 'Luban tiny <version>' in the output above"
    ok "status names the edition"

    section "nano: xlings and luban, and no shell to go into"
    capture "nano's xlings" luban run e-nano -- /usr/bin/xlings --version
    says "nano runs its xlings" "xlings"
    refuses "nano has nothing to log into" luban enter e-nano
    says "and says what to do instead" "luban run e-nano"

    section "tiny: a command, and a session typed into"
    capture "run" luban run e-tiny -- /bin/sh -c 'echo luban-$((6*7))'
    says "run" "luban-42"
    session 20 'echo IN-$((6*7)) > /root/kept; cat /root/kept; exit' e-tiny
    says "the welcome names Luban OS" "Luban OS"
    says "and how it runs" "shares this host's kernel"
    says "the prompt marks the system" "[luban]"
    says "and the host is the system's name" "root@e-tiny:"
    says "what is typed runs" "IN-42"
    capture "kept" luban run e-tiny -- cat /root/kept
    says "a session keeps what it wrote" "IN-42"
    # With no network (a network namespace of its own): entered from what is
    # here (design 2026-10-19 §9.3).
    if unshare -rn true 2>/dev/null; then
        capture "no network" unshare -rn luban run e-tiny -- /bin/sh -c 'echo offline-$((6*7))'
        says "entered with no network" "offline-42"
    fi

    section "tiny, keeping nothing: --temp"
    session 20 'echo GONE > /root/gone; cat /root/gone; exit' e-tiny --temp
    says "the welcome says it keeps nothing" "what you write is gone"
    says "and the prompt says so" "[luban:temp]"
    says "the session wrote" "GONE"
    refuses "what --temp wrote is gone" luban run e-tiny -- cat /root/gone

    section "tiny, keeping only /home: luban config --persist home"
    capture "config" luban config e-tiny --persist home
    capture "status" luban status e-tiny
    says "status says what it keeps" "only /home"
    says "and its network, in one line" "like the host"
    session 20 'echo H > /home/h; echo R > /root/r; exit' e-tiny
    capture "home kept" luban run e-tiny -- cat /home/h
    refuses "the rest is not" luban run e-tiny -- cat /root/r
    capture "config back" luban config e-tiny --persist all

    section "core: its own gcc builds C++23"
    capture "c++23" luban run e-core -- /bin/sh -c 'cd /tmp && printf "import std;\nint main(){ std::println(\"core {}\", __cplusplus >= 202302L); }\n" > t.cpp && g++ -std=c++23 -fmodules -fsearch-include-path -c bits/std.cc -o std.o && g++ -std=c++23 -fmodules t.cpp std.o -o t && ./t'
    says "import std builds and runs" "core true"

    section "generations: a package in, and rolled back"
    capture "install into it" xlings install -y --subos e-tiny xim:jq
    capture "jq inside" luban run e-tiny -- jq --version
    says "the package is there" "jq-"
    capture "history" luban history e-tiny
    capture "rollback" luban rollback e-tiny -y
    refuses "rolled back: it is gone" luban run e-tiny -- jq --version

    section "upgrade: a fresh system is the newest"
    capture "upgrade --dry-run" luban upgrade e-tiny --dry-run
    says "it plans, and has nothing to move" "is up to date"

    section "rm: asks first, then removes"
    refuses "rm asks" luban rm e-nano
    says "the answer is named" "luban rm e-nano -y"
    capture "rm -y" luban rm e-nano -y
    capture "ls" luban ls
    if grep -qF "e-nano" <<<"$OUT"; then fail "e-nano is still listed"; fi
    ok "e-nano is gone"
}

# ── archive: an environment carried to another place ─────────────────

suite_luban_archive() {
    [ "$(uname -s)" = Linux ] || fail "the archive suite is Linux's"
    local work; work="$(mktemp -d)"
    capture "setup" luban setup -y
    capture "new" luban new box --edition tiny
    capture "a file in it" luban run box -- /bin/sh -c 'echo carried > /root/note'

    section "luban export box box.luban -> luban import"
    capture "export" luban export box "$work/box.luban"
    says "the next step is the import" "luban import"
    capture "import" luban import "$work/box.luban" --name box2 -y
    capture "the file came along" luban run box2 -- cat /root/note
    says "box2 has what box had" "carried"

    section "luban export box rootfs/: a root that runs with no home"
    capture "export dir" luban export box "$work/rootfs/"
    # The bwrap xlings would choose: the root-owned copy the setup made
    # (AppArmor lets it make namespaces), the system's, then the home's payload.
    local bwrap=""
    for b in /usr/lib/xlings/bwrap /usr/bin/bwrap; do [ -x "$b" ] && { bwrap="$b"; break; }; done
    [ -n "$bwrap" ] || bwrap="$( (ls "$HOME"/.xlings/data/xpkgs/xim-x-bwrap/*/bin/bwrap 2>/dev/null || true) | head -1)"
    [ -n "$bwrap" ] || fail "no bwrap to run the exported root with"
    capture "it runs on its own" "$bwrap" --unshare-all --die-with-parent --ro-bind "$work/rootfs" / --dev /dev --proc /proc \
        --tmpfs /root --setenv HOME /root --setenv PATH /usr/bin:/bin -- /bin/sh -c 'xlings --version'
    says "its own xlings runs" "xlings"

    section "luban export box box.tar"
    capture "export tar" luban export box "$work/box.tar"
    local listing; listing="$(tar -tf "$work/box.tar")"
    grep -qx './usr' <<<"$listing" || fail "the tar has no /usr"
    grep -q 'bin/xlings$' <<<"$listing" || fail "the tar has no xlings"
    ok "the tar holds the root"
}

# ── machine: the environment itself, booted (KVM) ────────────────────

suite_luban_machine() {
    [ "$(uname -s)" = Linux ] || fail "the machine suite is Linux's"
    [ -r /dev/kvm ] && [ -w /dev/kvm ] || fail "no usable /dev/kvm: this leg is the machine's"
    capture "setup" luban setup -y
    capture "new" luban new m --edition tiny
    capture "as a machine" luban config m --mode machine

    section "luban enter m: a first time names what it needs, then boots"
    local rc=0
    OUT="$( { sleep 5; printf 'exit\n'; } 2>/dev/null | timeout 300 script -qfec "luban enter m" /dev/null 2>&1 | tr -d '\r')" || rc=$?
    printf '%s\n' "$OUT" | sed 's/^/      /'
    local hint
    hint="$(sed 's/\x1b\[[0-9;]*m//g' <<<"$OUT" | grep -oE 'xlings install [^ ]+' | head -1 || true)"
    if [ -n "$hint" ] && ! grep -qF "m is up" <<<"$OUT"; then
        # A person does what it says.
        capture "its hint" $hint -y
    fi
    # The first entry boots it; leaving keeps it running (the default of
    # "(b)ackground or (p)ower off?"), so the next entry joins it.
    local booted="$OUT"
    session 120 'echo MACHINE-$(uname -r); exit' m
    grep -qF "m is up" <<<"$booted$OUT" || fail "the machine came up: no 'm is up' in either entry"
    ok "the machine came up"
    says "the welcome says it is a machine" "machine: its own kernel"
    says "and the prompt says so" "<luban>"
    says "its own kernel answers" "MACHINE-"
    capture "stop" luban stop m

    section "luban export m m.iso, booted as it is"
    local work; work="$(mktemp -d)"
    capture "export iso" luban export m "$work/m.iso"
    says "the next step boots it" "luban enter"
    OUT="$( { sleep 200; } 2>/dev/null | timeout 240 script -qfec "luban enter $work/m.iso" /dev/null 2>&1 | tr -d '\r' || true)"
    printf '%s\n' "$OUT" | tail -15 | sed 's/^/      /'
    grep -qaF "luban-init: booting" <<<"$OUT" || fail "the image did not reach its init"
    ok "the image boots to luban-init"
}

# ── upgrade: the previous release's environment, after self update ──

suite_luban_upgrade() {
    [ "$(uname -s)" = Linux ] || fail "the upgrade suite is Linux's"
    # Made by the release before, with what both releases speak (xlings'
    # own subos commands), then the client is updated as a person updates it.
    section "an environment made by the previous release"
    # The one-time setup first, answered (that release's luban setup did not
    # pass -y on; its xlings does).
    capture "setup" xlings self doctor --isolation --fix -y
    capture "made" xlings subos new old --from subos:luban-tiny
    capture "it runs" xlings subos exec old -- /bin/sh -c 'echo before > /root/before; echo ok'
    says "the old client's environment runs" "ok"

    section "xlings self update, to the newest"
    capture "self update" xlings self update
    capture "version" xlings --version
    log "now $(tool_version xlings)"
    capture "doctor" xlings self doctor
    capture "luban ls" luban ls
    says "the environment is still listed" "old"
    capture "it still runs" luban run old -- cat /root/before
    says "what it had is there" "before"
    session 20 'echo after-$((6*7)); exit' old
    says "a console opens on it" "after-42"
    capture "upgrade plan" luban upgrade old --dry-run
}

# ── lived-in: a home used for a while, then the new client ───────────

suite_luban_lived_in() {
    [ "$(uname -s)" = Linux ] || fail "the lived-in suite is Linux's"
    # What a person had before the new client: tools in their default SubOS
    # on an older glibc than the editions use now, and a system made by the
    # release before (its own prefix shared with the host's).
    section "a home used for a while, by the release before"
    capture "setup" xlings self doctor --isolation --fix -y
    capture "an older runtime in default" xlings install -y xim:glibc@2.44
    capture "tools on it" xlings install -y xim:xz xim:vim xim:git xim:tmux
    capture "a system made then" xlings subos new old --from subos:luban-tiny
    capture "with a file in it" xlings subos exec old -- /bin/sh -c 'echo kept > /root/kept'

    section "xlings self update"
    capture "self update" xlings self update
    log "now $(tool_version xlings)"

    section "luban new on that home: no runtime of default's in the way"
    local e
    for e in tiny core; do
        capture "new $e" luban new "l-$e" --edition "$e"
        says "l-$e is made" "l-$e is ready"
        if grep -qF "still uses" <<<"$OUT"; then fail "l-$e: a runtime of another SubOS was in the way"; fi
    done

    section "the system made before moves into its own userland"
    capture "upgrade it" luban upgrade old -y
    capture "its file came along" luban run old -- cat /root/kept
    says "what it had is there" "kept"
    capture "its home is its own" luban run old -- /bin/sh -c 'echo "XH=$XLINGS_HOME"; ls /home'
    says "xlings there is its own" "XH=/xlings"
    capture "the way back is kept" luban history old
    says "history names it" "before its move"
    capture "back to before the move" luban rollback old --to relayout -y
    capture "its file is there" luban run old -- cat /root/kept
    says "what it had is there" "kept"

    section "programs in a system have what their recipes registered"
    capture "vim and git into it" xlings install -y --subos l-core xim:vim xim:git
    capture "vim finds its runtime" luban run l-core -- /bin/sh -c \
        'vim -es -c "redir! > /tmp/v" -c "echo \$VIM" -c "redir END" -c q; cat /tmp/v'
    says "\$VIM is set" "xpkgs/xim-x-vim"
    capture "git speaks https" luban run l-core -- git ls-remote https://github.com/openxlings/xlings HEAD
    says "git reached the remote" "HEAD"

    section "a host locale the system does not have"
    log "\$ LC_ALL=en_US.UTF-8 luban run l-core -- bash -c true"
    OUT="$(LC_ALL=en_US.UTF-8 luban run l-core -- bash -c 'echo LANG=$LANG' 2>&1)" || fail "bash did not run"
    printf '%s\n' "$OUT" | sed 's/^/      /'
    if grep -qF "setlocale" <<<"$OUT"; then fail "the host's locale reached the system"; fi
    says "it speaks its own" "LANG=C.UTF-8"

    section "the runtime is the environment's"
    capture "the home's runtime" xlings config --runtime glibc@latest
    says "it names one" "runtime linux-"

    section "self doctor --fix ends, asking nothing it cannot"
    local rc=0
    timeout 1800 xlings self doctor --fix -y </dev/null >"$HOME/doctor.log" 2>&1 || rc=$?
    tail -n 20 "$HOME/doctor.log" | sed 's/^/      /'
    [ "$rc" -ne 124 ] || fail "self doctor --fix did not end in 30 minutes"
    ok "self doctor --fix ended (exit $rc)"
}

# ── host: macOS and Windows say what this computer can do ────────────

suite_luban_host() {
    section "luban on $(uname -s): what this computer can do"
    capture "version" luban --version
    capture "overview" luban
    says "the overview answers" "Luban OS"
    # Ready or not, it answers (exit 1 is "not ready", with what to do).
    log "\$ luban status"
    OUT="$(luban status 2>&1)" || true
    printf '%s\n' "$OUT" | sed 's/^/      /'
    says "the host status answers" "ready"
    # A hosted Mac has no nested virtualization: the Luban host machine cannot
    # start, and `luban new` says so; with it, a system is made there.
    if grep -qE "sandbox +ready" <<<"$OUT"; then
        capture "a Luban system in the Luban host machine" luban new box --edition tiny -y
        capture "a command in it" luban run box -- uname -s
        says "it runs Linux" "Linux"
        capture "removed" luban rm box -y
    else
        refuses "a Luban system is not made here" luban new box --edition tiny
    fi
    # The window is published for macOS from 2026.10.15.1 (part 4 §8.1).
    capture "the window" xlings install luban-os -y
    capture "it answers" luban-os --version
    says "the Luban OS window is installed" "luban-os"
}
