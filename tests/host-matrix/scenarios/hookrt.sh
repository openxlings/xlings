#!/usr/bin/env bash
# A hook that starts a downloaded program -- one whose PT_INTERP is the
# standard loader path, not rewritten yet -- installs on every host (design
# 2026-11-21-issue-724 §4.1, HOOK-RUNTIME-CONTRACT): with the host's glibc
# where it has one, with the environment's where it has none (Alpine: the
# hook runs in a worker given it), in an ordinary home and in a SubOS with a
# policy. A host whose glibc is older than the program needs fails, and says
# which glibc the hook saw.
source "$(dirname "$0")/../lib.sh"
# Where the host's root has no runtime there, the hook's worker gives it:
# isolation as a user gets it (bwrap from the index).
require_isolation
case "$(uname -m)" in
    x86_64) std=/lib64/ld-linux-x86-64.so.2 ;;
    aarch64) std=/lib/ld-linux-aarch64.so.1 ;;
    *) skip "no glibc family for $(uname -m)" ;;
esac

# The fixture: a recipe whose install hook copies glibc's getconf (it needs
# glibc 2.34), points its PT_INTERP at the standard path, and runs it. Each
# step has to say when it did not happen: system.exec fails the hook when the
# program fails (os.exec only returns a status), and the interpreter is
# checked as rewritten -- without patchelf set_interpreter changes nothing and
# the program would start with the payload's own loader.
idx="$HM_STATE/hookrt-index"
rm -rf "$idx"; mkdir -p "$idx/pkgs/h"
echo 'xim_indexrepos = {}' > "$idx/xim-indexrepos.lua"
cat > "$idx/pkgs/h/hookrt.lua" <<LUA
package = {
    spec = "1", name = "hookrt", type = "package", archs = {"x86_64", "aarch64"},
    xpm = { linux = {
        deps = { runtime = { "xim:glibc" }, build = { "xim:patchelf" } },
        ["1.0.0"] = {}, ["1.0.1"] = {},
    } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.elfpatch")
import("xim.libxpkg.system")
function install()
    local glibc = pkginfo.dep_install_dir("glibc")
    local dir = pkginfo.install_dir()
    if not os.mkdir(path.join(dir, "bin")) then error("cannot create " .. path.join(dir, "bin")) end
    local program = path.join(dir, "bin", "hookrt-getconf")
    if not os.cp(path.join(glibc, "bin", "getconf"), program) then error("cannot copy getconf from " .. tostring(glibc)) end
    local patched = elfpatch.set_interpreter(program, "$std")
    if patched.patched ~= 1 then
        error("set_interpreter rewrote " .. tostring(patched.patched) .. " of " .. tostring(patched.scanned) .. " files")
    end
    system.exec(program .. " PAGESIZE")
    return true
end
LUA
"$HM_PYTHON" - "$XLINGS_HOME/.xlings.json" "$idx" <<'PY'
import json, sys
p = sys.argv[1]
cfg = json.load(open(p))
repos = [r for r in cfg.get("index_repos", []) if r.get("name") != "hmfix"]
repos.append({"name": "hmfix", "url": sys.argv[2]})
cfg["index_repos"] = repos
json.dump(cfg, open(p, "w"), indent=2)
PY

# What the hook itself wrote, for a step that did not go as expected: the
# install's own output names the failure, the hook log says what led to it.
hook_logs() {
    local f
    for f in "$XLINGS_HOME"/logs/hooks/*hookrt*; do
        [[ -f "$f" ]] || continue
        printf -- '--- %s\n' "${f#"$XLINGS_HOME"/}"
        tail -n 40 "$f"
    done
}

step "the fixture index is read" ok -- X update

host_glibc="$( (getconf GNU_LIBC_VERSION 2>/dev/null || true) | awk '{print $2}')"
older=false
if [[ -n "$host_glibc" && -e "$std" ]] && [[ "$(printf '%s\n2.34\n' "$host_glibc" | sort -V | head -1)" != 2.34 ]]; then
    older=true
fi
if [[ -e "$std" ]]; then form=present; elif [[ -e "$(dirname "$std")" ]]; then form=merge; else form=link; fi
record "this host's standard path: $form (glibc ${host_glibc:-none})" true 0 ""

if [[ "$older" == true ]]; then
    # The host's own runtime, kept (取舍 1): the failure names it.
    step "a hook's program that needs a newer glibc fails, and says which it saw" nonzero \
        "this machine's glibc $host_glibc" -- X install -y hmfix:hookrt@1.0.0 || hook_logs
    done_
fi
step "a hook starts a downloaded program (an ordinary home)" ok -- X install -y hmfix:hookrt@1.0.0 || hook_logs

# A SubOS with a policy: its hooks run in a worker already, given the same runtime.
if out="$(X subos new hmhook --sandbox dev 2>&1)"; then
    step "the same in a SubOS with a policy" ok -- env XLINGS_ACTIVE_SUBOS=hmhook "$XLINGS_HOME/bin/xlings" install -y hmfix:hookrt@1.0.1
else
    needs_setup "$out" || record "a SubOS with a policy could not be made here" true 0 "$out"
fi
done_
