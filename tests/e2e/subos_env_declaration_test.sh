#!/usr/bin/env bash
# E2E: subos.env — a package declares an environment variable, and a program
# the package does not own can see it.
#
# This is the whole of subos slice 1 end to end. The layer being tested is
# configuration: PATH and RPATH get a binary loaded, and neither can tell it
# where to find a GL driver. That is mcpp-community/mcpp#352 — a GLFW binary
# that links fine and exits 255 because LIBGL_DRIVERS_PATH points nowhere.
#
# What has to hold:
#   1. install       — the declaration lands in the subos manifest, keyed by
#                      the declaring package
#   2. --shell       — eval'ing the emitted code sets the variable
#   3. --cmd         — a command run in the subos inherits it
#   4. user override — a value the user exported already wins (UC-1)
#   5. uninstall     — the section goes, and `envs` stays as {}
#   6. doctor        — clean afterwards, and it catches an orphaned section
#
# The probe rule has its own test (E2E: subos_env_probe_compat_test.sh); this
# one assumes the capability is present.
#
# Design: .agents/docs/2026-08-05-subos-minimum-design.md

set -uo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/subos_env"
LOCAL_INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
HOME_DIR="$RUNTIME_DIR/home"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup
mkdir -p "$RUNTIME_DIR"

BIN="$(find_xlings_bin)"
log "client: $("$BIN" --version 2>&1 | head -1)"

# ── a package that declares two variables ───────────────────────────────
cp -r "$FIXTURE_INDEX_DIR" "$LOCAL_INDEX_DIR"
printf 'xim_indexrepos = {}\n' > "$LOCAL_INDEX_DIR/xim-indexrepos.lua"
rm -f "$LOCAL_INDEX_DIR/.xlings-index-cache.json"
mkdir -p "$LOCAL_INDEX_DIR/pkgs/e"

cat > "$LOCAL_INDEX_DIR/pkgs/e/envfixture.lua" <<'LUA'
package = {
    spec = "1",
    name = "envfixture",
    description = "Local fixture for tests/e2e/subos_env_declaration_test.sh",
    authors = {"xlings-ci"},
    licenses = {"MIT"},
    type = "package",
    archs = {"x86_64"},
    status = "stable",
    categories = {"test-fixture"},

    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
import("xim.libxpkg.subos")

function install()
    local dir = pkginfo.install_dir()
    os.tryrm(dir)
    os.mkdir(path.join(dir, "bin"))
    os.mkdir(path.join(dir, "drivers"))
    os.mkdir(path.join(dir, "share"))
    io.writefile(path.join(dir, "bin", "envfixture"), "#!/bin/sh\necho envfixture\n")
    os.exec("chmod +x " .. path.join(dir, "bin", "envfixture"))
    return true
end

function config()
    xvm.add(package.name, { bindir = path.join(pkginfo.install_dir(), "bin") })

    -- type(), not truthiness: import() hands back a permissive proxy for a
    -- module the client does not ship, and every key on it is truthy.
    if type(subos.env) == "function" then
        local binding = package.name .. "@" .. pkginfo.version()
        subos.env{ var = "E2E_DRIVERS_PATH", op = "set",
                   value = "${pkgdir}/drivers", binding = binding }
        subos.env{ var = "E2E_DATA_DIRS", op = "prepend",
                   value = "${pkgdir}/share", binding = binding }
    end
    return true
end

function uninstall()
    -- Nothing here on purpose. The env section is provider-scoped, and xlings
    -- drops it with the package; a recipe writing its own cleanup would be
    -- the second owner of that state.
    return true
end
LUA

# ── an isolated home ────────────────────────────────────────────────────
mkdir -p "$HOME_DIR/subos/default/bin" "$HOME_DIR/data/xim-index-repos"
cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [
    { "name": "xim", "url": "$LOCAL_INDEX_DIR" }
  ]
}
EOF
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
cp "$BIN" "$HOME_DIR/xlings"

# env -i: the variables under test must come from the subos, not be inherited
# from whatever shell is running the suite.
x() { ( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
        XLINGS_HOME="$HOME_DIR" "$BIN" "$@" ) }

x self init >/dev/null 2>&1 || true

MANIFEST="$HOME_DIR/subos/default/.xlings.json"
[[ -f "$MANIFEST" ]] || fail "self init produced no subos manifest"

# ── 1. install records the declaration ──────────────────────────────────
OUT="$(x install envfixture@1.0.0 -y 2>&1)" || { echo "$OUT" >&2; fail "install failed"; }

python3 - "$MANIFEST" <<'PY' || exit 1
import json, sys
info = json.load(open(sys.argv[1])).get("subos_info")
if info is None:
    raise SystemExit("no subos_info block after install")
envs = info.get("envs", {})
key = "envfixture@1.0.0"
if key not in envs:
    raise SystemExit(f"no section for {key}; envs={envs}")
got = {d["var"]: (d["op"], d["value"]) for d in envs[key]}
want = {
    "E2E_DRIVERS_PATH": ("set", "${pkgdir}/drivers"),
    "E2E_DATA_DIRS": ("prepend", "${pkgdir}/share"),
}
if got != want:
    raise SystemExit(f"recorded {got}, expected {want}")
# The stored value must stay a placeholder. A manifest holding this machine's
# absolute paths describes this machine, and a subos description that only
# works where it was written is not a description.
print("[project-e2e]   ✓ recorded, and still portable")
PY

# Installing again must not grow the section — config() re-runs on every
# dependent install.
x install envfixture@1.0.0 -y >/dev/null 2>&1
COUNT="$(python3 -c '
import json,sys
print(len(json.load(open(sys.argv[1]))["subos_info"]["envs"]["envfixture@1.0.0"]))' "$MANIFEST")"
[[ "$COUNT" == "2" ]] || fail "reinstall grew the section to $COUNT declarations"
log "  ✓ re-install is idempotent"

# ── 2. --shell emits code that sets them ────────────────────────────────
SHELL_OUT="$(x subos use default --shell sh 2>/dev/null)"
EVALED="$( env -i PATH=/usr/bin:/bin bash -c "eval '$SHELL_OUT'
  echo \"DRIVERS=\$E2E_DRIVERS_PATH\"
  echo \"DATA=\$E2E_DATA_DIRS\"" )"
grep -q "DRIVERS=.*/drivers$" <<<"$EVALED" \
  || { echo "$EVALED" >&2; fail "--shell did not set E2E_DRIVERS_PATH"; }
grep -q "DATA=.*/share$" <<<"$EVALED" \
  || { echo "$EVALED" >&2; fail "--shell did not set E2E_DATA_DIRS"; }
# The expanded path must exist — the placeholder resolved to a real payload.
DRIVERS_PATH="$(sed -n 's/^DRIVERS=//p' <<<"$EVALED")"
[[ -d "$DRIVERS_PATH" ]] || fail "E2E_DRIVERS_PATH=$DRIVERS_PATH is not a directory"
log "  ✓ --shell exports both, expanded to a real payload"

# UC-2: the user is told what was injected, on stderr so the stdout stays
# eval-safe.
REPORT="$(x subos use default --shell sh 2>&1 >/dev/null)"
grep -q "2 env var(s) from 1 package(s)" <<<"$REPORT" \
  || { echo "$REPORT" >&2; fail "--shell did not report what it injected"; }
log "  ✓ --shell reports the injected set on stderr"

# ── 3. --cmd inherits them ──────────────────────────────────────────────
CMD_OUT="$(x subos use default --cmd 'echo CMD=$E2E_DRIVERS_PATH' 2>/dev/null)"
grep -q "CMD=.*/drivers$" <<<"$CMD_OUT" \
  || { echo "$CMD_OUT" >&2; fail "--cmd did not inject E2E_DRIVERS_PATH"; }
log "  ✓ --cmd injects into the process environment"

# ── 4. the user's own value wins (UC-1) ─────────────────────────────────
OVERRIDE="$( cd /tmp && env -i HOME="$HOME" PATH=/usr/bin:/bin \
             XLINGS_HOME="$HOME_DIR" \
             E2E_DRIVERS_PATH=/user/choice E2E_DATA_DIRS=/user/share \
             "$BIN" subos use default \
             --cmd 'echo "D=$E2E_DRIVERS_PATH"; echo "X=$E2E_DATA_DIRS"' 2>/dev/null )"
grep -q "^D=/user/choice$" <<<"$OVERRIDE" \
  || { echo "$OVERRIDE" >&2; fail "a 'set' declaration overwrote the user's value"; }
# prepend still composes — that is what prepend means — but it must not
# discard what was there.
grep -q "^X=.*/share:/user/share$" <<<"$OVERRIDE" \
  || { echo "$OVERRIDE" >&2; fail "'prepend' did not compose with the user's value"; }
log "  ✓ the user's value survives 'set' and is kept by 'prepend'"

# ── 5. doctor is clean ──────────────────────────────────────────────────
DOC="$(x self doctor 2>&1)"
grep -qiE "subos env (orphan|unresolved)" <<<"$DOC" \
  && { echo "$DOC" >&2; fail "doctor reports a defect on a healthy subos"; }
log "  ✓ doctor is clean while the package is installed"

# ...and catches a section whose owner is gone. Removing the payload behind
# the package's back is what an interrupted uninstall leaves.
python3 - "$MANIFEST" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d["subos_info"]["envs"]["ghost@9.9.9"] = [
    {"var": "GHOST", "op": "set", "value": "${pkgdir}/lib"}]
json.dump(d, open(p, "w"), indent=2)
PY
DOC="$(x self doctor 2>&1)"
grep -qi "subos env orphan" <<<"$DOC" \
  || { echo "$DOC" >&2; fail "doctor missed an orphaned env section"; }
x self doctor --fix >/dev/null 2>&1
python3 -c '
import json,sys
envs = json.load(open(sys.argv[1]))["subos_info"]["envs"]
assert "ghost@9.9.9" not in envs, "--fix left the orphan behind"
assert "envfixture@1.0.0" in envs, "--fix took the healthy section too"' "$MANIFEST" \
  || fail "doctor --fix did not repair exactly the orphan"
log "  ✓ doctor detects and repairs an orphaned section, and only that"

# ── 6. uninstall drops the section ──────────────────────────────────────
x remove envfixture -y >/dev/null 2>&1 || x uninstall envfixture -y >/dev/null 2>&1

python3 - "$MANIFEST" <<'PY' || exit 1
import json, sys
info = json.load(open(sys.argv[1]))["subos_info"]
envs = info["envs"]
if "envfixture@1.0.0" in envs:
    raise SystemExit(f"uninstall left the section behind: {envs}")
# "envs" itself must stay, as {}. An absent key and an empty one would be two
# states meaning the same thing, and every reader would have to handle both.
if not isinstance(envs, dict):
    raise SystemExit(f"envs is no longer an object: {envs!r}")
print("[project-e2e]   ✓ section removed, envs kept as {}")
PY

# And the variable is gone from what a new shell would get.
AFTER="$(x subos use default --cmd 'echo AFTER=${E2E_DRIVERS_PATH:-<unset>}' 2>/dev/null)"
grep -q "AFTER=<unset>" <<<"$AFTER" \
  || { echo "$AFTER" >&2; fail "the variable is still injected after uninstall"; }
log "  ✓ the variable is no longer injected"

log "E2E subos.env declaration lifecycle: PASS"
