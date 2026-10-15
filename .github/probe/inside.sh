#!/usr/bin/env bash
# O1 (vim and terminfo), O2 (directory entries of a generation; bwrap bind
# cost), O3 (mount --move inside a user namespace), with xlings $XV on an
# Ubuntu runner. Prints observations; never asserts.
set -uo pipefail
XV="${XV:-2026.10.15.4}"
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$RUNNER_TEMP/p"; mkdir -p "$T/logs"; cd "$T"
S="${GITHUB_STEP_SUMMARY:-/dev/null}"
say() { echo "$*"; echo "$*" >> "$S"; }
block() {
    local title="$1"; shift; local out rc
    out="$("$@" 2>&1)"; rc=$?
    echo "== $title (exit $rc)"; echo "$out"
    { echo; echo "**$title** (exit $rc)"; echo '```'; echo "$out" | tail -n "${LINES_:-40}"; echo '```'; } >> "$S"
}

sudo sysctl -q -w kernel.apparmor_restrict_unprivileged_userns=0
curl -fsSL -o xl.tgz "https://github.com/openxlings/xlings/releases/download/v$XV/xlings-$XV-linux-x86_64.tar.gz"
mkdir -p inst && tar -xzf xl.tgz -C inst
(cd inst/*/ && ./bin/xlings self install </dev/null) > logs/install.log 2>&1
export PATH="$HOME/.xlings/bin:$PATH"
X() { env -u XLINGS_PROJECT_DIR XLINGS_NON_INTERACTIVE=1 "$HOME/.xlings/bin/xlings" "$@"; }
L() { env -u XLINGS_PROJECT_DIR "$HOME/.xlings/bin/luban" "$@"; }
block "versions" bash -c "xlings --version; luban --version; uname -r; bwrap --version 2>&1 || true"

say "### making core and desktop"
LINES_=12 block "luban new pcore --edition core -y" timeout 2400 env XLINGS_NON_INTERACTIVE=1 "$HOME/.xlings/bin/luban" new pcore --edition core -y
LINES_=12 block "luban new pdesk --edition desktop -y" timeout 2400 env XLINGS_NON_INTERACTIVE=1 "$HOME/.xlings/bin/luban" new pdesk --edition desktop -y

gen_of() {   # the current generation directory of a system
    for base in /xlings "$HOME/.xlings"; do
        local g; g="$(readlink -f "$base/subos/$1/root" 2>/dev/null)"
        [[ -n "$g" && -f "$g/links.tsv" ]] && { echo "$g"; return; }
    done
    find / -path "*/subos/$1/root.gen/*/links.tsv" 2>/dev/null | sort -V | tail -1 | xargs -r dirname
}

say "### O2: directory entries of a generation"
for sys in pcore pdesk; do
    G="$(gen_of $sys)"
    say "$sys generation: ${G:-none}"
    [[ -n "$G" ]] || continue
    block "$sys: entries by kind" python3 - "$G" <<'PY'
import os, sys, collections
g = sys.argv[1]
rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(g, "links.tsv")) if "\t" in l]
kinds = collections.Counter(); dirs = []
for rel, target in rows[:]:
    t = target if os.path.isabs(target) else os.path.join(g, os.path.dirname(rel), target)
    if t.startswith("/xlings/") and not os.path.exists(t):
        t = os.path.expanduser("~/.xlings/domains/xlings/private") + t[len("/xlings"):]
    if os.path.isdir(t):
        kinds["dir"] += 1; dirs.append((rel, target))
    elif os.path.exists(t):
        kinds["file"] += 1
    else:
        kinds["dangling"] += 1
print("total", len(rows), dict(kinds))
by = collections.Counter("/".join(r.split("/")[:3]) for r, _ in dirs)
for k, v in by.most_common(25): print(f"  {v:4d}  {k}")
open(os.path.join(os.environ.get("RUNNER_TEMP", "/tmp"), f"dirs-{os.path.basename(os.path.dirname(os.path.dirname(g)))}.txt"), "w").write(
    "\n".join(t if os.path.isabs(t) else os.path.join(g, os.path.dirname(r), t) for r, t in dirs))
print("first dir entries:"); [print("  ", r, "->", t) for r, t in dirs[:15]]
PY
    block "$sys: /usr entry of the root tree" bash -c "ls -la $(dirname "$(dirname "$G")")/rootfs/ 2>/dev/null | grep -E ' usr| bin| lib' ; ls -la $(dirname "$(dirname "$G")")/root"
done

say "### O2: bwrap start-up cost per bind"
BW="$(command -v bwrap || ls "$HOME"/.xlings/data/xpkgs/xim-x-bwrap/*/bin/bwrap 2>/dev/null | head -1)"
say "bwrap: $BW ($("$BW" --version 2>&1))"
mkdir -p "$T/bench"; for i in $(seq 0 499); do mkdir -p "$T/bench/d$i"; done
block "bwrap time vs number of --ro-bind" python3 - "$BW" "$T/bench" <<'PY'
import subprocess, sys, time, statistics
bw, bench = sys.argv[1], sys.argv[2]
base = [bw, "--unshare-user", "--unshare-pid", "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/mnt"]
for n in [0, 10, 50, 100, 200, 500]:
    args = list(base)
    for i in range(n):
        args += ["--dir", f"/mnt/b{i}", "--ro-bind", f"{bench}/d{i}", f"/mnt/b{i}"]
    args += ["--", "/bin/true"]
    ts = []
    for _ in range(15):
        t0 = time.perf_counter(); r = subprocess.run(args, capture_output=True); ts.append((time.perf_counter() - t0) * 1000)
        if r.returncode: print("  n=%d failed: %s" % (n, r.stderr.decode()[:200])); break
    print(f"  binds={n:4d}  median={statistics.median(ts):7.1f} ms  max={max(ts):7.1f} ms")
PY

say "### O1: vim and terminfo"
G="$(gen_of pcore)"
block "what /usr/bin/vim is in the generation" bash -c "ls -la '$G/usr/bin/vim'; readlink -f '$G/usr/bin/vim'"
VIM="$(ls -d "$HOME"/.xlings/data/xpkgs/xim-x-vim/*/bin/vim "$HOME"/.xlings/domains/xlings/private/data/xpkgs/xim-x-vim/*/bin/vim 2>/dev/null | head -1)"
block "vim payload" bash -c "echo '$VIM'; file '$VIM'; '$VIM' --version | head -3; '$VIM' --version | grep -iE 'terminfo|tgetent|tlib' ; strings '$VIM' | grep -E '^/[a-z/]*terminfo' | sort -u | head"
block "terminfo dirs present on the runner" bash -c "ls -d /etc/terminfo /lib/terminfo /usr/share/terminfo /usr/lib/terminfo 2>&1"
printf 'hello\n' > "$T/t.txt"
for mode in hidden present; do
    hide=()
    [[ $mode == hidden ]] && for d in /etc/terminfo /lib/terminfo /usr/share/terminfo /usr/lib/terminfo; do [[ -d $d ]] && hide+=(--tmpfs "$d"); done
    block "O1 vim, TERM=xterm-256color, terminfo $mode" python3 "$HERE/pty_run.py" --out "$T/logs/vim-$mode.bin" \
        --step 'hello|:q\r' -- "$BW" --unshare-user --ro-bind / / --dev /dev --proc /proc --bind "$T" "$T" "${hide[@]}" \
        --setenv TERM xterm-256color --setenv HOME "$T" -- "$VIM" -u NONE -N "$T/t.txt"
done
block "O1 inside the system: luban enter pcore, vim, :q" python3 "$HERE/pty_run.py" --out "$T/logs/enter-vim.bin" --wait 90 \
    --step '@pcore|vim /tmp/x.txt\r' --step '~|ihello\x1b:wq\r' --step '@pcore|ls /usr/share/terminfo; echo TERM=$TERM; exit\r' \
    -- env TERM=xterm-256color "$HOME/.xlings/bin/luban" enter pcore

say "### O3: mount --move inside a user namespace"
mkdir -p "$T/genA/usr/bin" "$T/genB/usr/bin"; echo A > "$T/genA/usr/bin/which-gen"; echo B > "$T/genB/usr/bin/which-gen"
cat > "$T/o3.sh" <<'SH'
set -u
T="$1"
echo "caps: $(grep CapEff /proc/self/status)"
echo "mounted: $(cat /mnt/usr/bin/which-gen)"
if mount --bind "$T/genB/usr" /mnt/usr 2>&1; then echo "stack bind: ok -> $(cat /mnt/usr/bin/which-gen)"; umount /mnt/usr 2>&1 && echo "unmount top: ok -> $(cat /mnt/usr/bin/which-gen)"; else echo "stack bind: failed"; fi
mkdir -p /mnt/stage
if mount --bind "$T/genB/usr" /mnt/stage 2>&1 && mount --move /mnt/stage /mnt/usr 2>&1; then echo "stage+move: ok -> $(cat /mnt/usr/bin/which-gen)"; else echo "stage+move: failed"; fi
mount -o remount,bind,ro /mnt/usr 2>&1 && echo "remount ro: ok" || echo "remount ro: failed"
exec 9</mnt/usr/bin/which-gen
umount -l /mnt/usr 2>&1 && echo "lazy umount top: ok; held fd reads: $(cat <&9); /mnt/usr now: $(cat /mnt/usr/bin/which-gen 2>&1)"
python3 - <<'PY'
import ctypes, os
libc = ctypes.CDLL(None, use_errno=True)
for cap in range(0, 41): libc.prctl(24, cap, 0, 0, 0)
libc.prctl(47, 4, 0, 0, 0)  # PR_CAP_AMBIENT, CLEAR_ALL
pid = os.fork()
if pid == 0:
    os.execvp("sh", ["sh", "-c", "grep -E 'CapEff|CapBnd' /proc/self/status; mount --bind /tmp /mnt/stage 2>&1 || echo 'child mount: refused'"])
os.waitpid(pid, 0)
PY
SH
block "O3 without --cap-add (as xlings starts bwrap today)" "$BW" --unshare-user --uid 0 --gid 0 --unshare-pid --ro-bind / / --dev /dev --proc /proc \
    --tmpfs /mnt --bind "$T" "$T" --dir /mnt/usr --ro-bind "$T/genA/usr" /mnt/usr -- sh "$T/o3.sh" "$T"
block "O3 with --cap-add CAP_SYS_ADMIN" "$BW" --unshare-user --uid 0 --gid 0 --unshare-pid --cap-add CAP_SYS_ADMIN --ro-bind / / --dev /dev --proc /proc \
    --tmpfs /mnt --bind "$T" "$T" --dir /mnt/usr --ro-bind "$T/genA/usr" /mnt/usr -- sh "$T/o3.sh" "$T"
block "O3 a mount from the parent namespace (locked: /opt), with CAP_SYS_ADMIN" "$BW" --unshare-user --uid 0 --gid 0 --unshare-pid --cap-add CAP_SYS_ADMIN \
    --bind / / --dev /dev --proc /proc --tmpfs /mnt -- /bin/sh -c "mkdir -p /mnt/s; mount --bind $T/genB/usr /opt && echo 'stack over a locked mount: ok' && /usr/bin/umount /opt && echo 'umount the stacked one: ok'; /usr/bin/umount -l / 2>&1 | head -1; mount --bind $T/genB/usr /mnt/s && mount --move /mnt/s /opt && echo 'move onto a locked mount: ok' || echo 'move onto a locked mount: failed'"

say "### O6: a machine, and what /usr/bin/vim resolves to there"
sudo chmod 666 /dev/kvm 2>/dev/null || true
block "luban config pcore mode machine" timeout 300 "$HOME/.xlings/bin/luban" config pcore mode machine
LINES_=60 block "luban run pcore (machine) -- vim --version; active subos" timeout 900 env XLINGS_NON_INTERACTIVE=1 "$HOME/.xlings/bin/luban" run pcore -y -- /bin/sh -c 'vim --version | head -2; echo "XLINGS_ACTIVE_SUBOS=$XLINGS_ACTIVE_SUBOS"; xlings subos list 2>&1 | head -5; id; xlings install -y xim:jq 2>&1 | tail -2'

