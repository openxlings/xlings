#!/usr/bin/env bash
# E2E: the default doctor is a bounded metadata check; recursive ELF and
# runtime probes are explicit deep-audit work and scope never widens on error.
set -euo pipefail

# shellcheck source=./project_test_lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"
require_fixture_index

RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/self_doctor_depth"
HOME_DIR="$RUNTIME_DIR/home"
INDEX_DIR="$RUNTIME_DIR/xim-pkgindex"
RECORDER_DIR="$RUNTIME_DIR/recorders"
PATCHELF_TRACE="$RUNTIME_DIR/patchelf.trace"
GETENT_TRACE="$RUNTIME_DIR/getent.trace"
CHILD_TRACE="$RUNTIME_DIR/child-xlings.trace"
OUT_FILE="$RUNTIME_DIR/doctor.out"
FIRST_CALL_TRACE="$RUNTIME_DIR/first-patchelf.trace"

cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

XLINGS_BIN="$(find_xlings_bin)"
XLINGS_BIN="$(cd "$(dirname "$XLINGS_BIN")" && pwd)/$(basename "$XLINGS_BIN")"
mkdir -p "$HOME_DIR/subos/default/bin" "$RECORDER_DIR"
cp -r "$FIXTURE_INDEX_DIR" "$INDEX_DIR"
mkdir -p "$INDEX_DIR/pkgs/f"
cp "$XLINGS_BIN" "$HOME_DIR/xlings"

cat > "$INDEX_DIR/pkgs/f/fixture.lua" <<'LUA'
package = {
    spec = "1", name = "fixture",
    description = "self doctor depth fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        macosx  = { ["1.0.0"] = {}, ["2.0.0"] = {} },
        windows = { ["1.0.0"] = {}, ["2.0.0"] = {} },
    },
}

LUA
printf 'depth-fixture-v1\n' > "$INDEX_DIR/.xlings-index-version"
printf 'xim_indexrepos = {}\n' > "$INDEX_DIR/xim-indexrepos.lua"
rm -f "$INDEX_DIR/.xlings-index-cache.json"

cat > "$HOME_DIR/.xlings.json" <<EOF
{
  "mirror": "GLOBAL",
  "index_repos": [{"name": "xim", "url": "$INDEX_DIR"}]
}
EOF

cat > "$RECORDER_DIR/patchelf" <<'SH'
#!/bin/sh
if [ ! -e "$DOCTOR_FIRST_CALL_TRACE" ]; then
  if grep -q 'fixture@1' "$DOCTOR_OUTPUT_FILE" 2>/dev/null; then
    printf 'scope-announced\n' > "$DOCTOR_FIRST_CALL_TRACE"
  else
    printf 'scope-missing\n' > "$DOCTOR_FIRST_CALL_TRACE"
  fi
fi
printf '%s\n' "$*" >> "$DOCTOR_PATCHELF_TRACE"
case "$1" in
  --print-interpreter) printf '/lib64/ld-linux-x86-64.so.2\n' ;;
  --print-rpath)       printf '\n' ;;
esac
SH
chmod +x "$RECORDER_DIR/patchelf"

RUN() {
  local doctor_home="${DOCTOR_HOME_OVERRIDE:-$HOME_DIR}"
  ( cd /tmp && exec env -i HOME="$HOME" \
      PATH="$RECORDER_DIR:/usr/bin:/bin" \
      XLINGS_HOME="$doctor_home" \
      DOCTOR_PATCHELF_TRACE="$PATCHELF_TRACE" \
      DOCTOR_GETENT_TRACE="$GETENT_TRACE" \
      DOCTOR_FIRST_CALL_TRACE="$FIRST_CALL_TRACE" \
      DOCTOR_OUTPUT_FILE="$OUT_FILE" \
      "$XLINGS_BIN" "$@" )
}

# Observe real descendants through /proc. The old coordinate probe launches
# `<this xlings> info ...` under a shell; an in-process catalog lookup does not.
RUN_WATCHED() {
  : > "$CHILD_TRACE"
  # Keep the background PID as the tested xlings process itself. Backgrounding
  # RUN() would add a function subshell and make the command root look like a
  # recursive child.
  ( cd /tmp && exec env -i HOME="$HOME" \
      PATH="$RECORDER_DIR:/usr/bin:/bin" \
      XLINGS_HOME="$HOME_DIR" \
      DOCTOR_PATCHELF_TRACE="$PATCHELF_TRACE" \
      DOCTOR_GETENT_TRACE="$GETENT_TRACE" \
      DOCTOR_FIRST_CALL_TRACE="$FIRST_CALL_TRACE" \
      DOCTOR_OUTPUT_FILE="$OUT_FILE" \
      "$XLINGS_BIN" "$@" ) >"$OUT_FILE" 2>&1 &
  local root_pid=$! rc=0
  while kill -0 "$root_pid" 2>/dev/null; do
    local -a queue=("$root_pid")
    local i=0
    while ((i < ${#queue[@]})); do
      local pid="${queue[$i]}" children=""
      i=$((i + 1))
      if [[ -r "/proc/$pid/task/$pid/children" ]]; then
        children="$(<"/proc/$pid/task/$pid/children")"
      fi
      local child exe
      for child in $children; do
        queue+=("$child")
        exe="$(readlink "/proc/$child/exe" 2>/dev/null || true)"
        if [[ "$exe" == "$XLINGS_BIN" ]] \
            && ! grep -Fxq "$child" "$CHILD_TRACE"; then
          printf '%s\n' "$child" >> "$CHILD_TRACE"
        fi
      done
    done
    sleep 0.002
  done
  wait "$root_pid" || rc=$?
  return "$rc"
}

RUN self init >/dev/null 2>&1 || fail "setup: self init failed"
mkdir -p "$HOME_DIR/data/xim-index-repos"
printf '{}\n' > "$HOME_DIR/data/xim-index-repos/xim-indexrepos.json"
ln -s "$INDEX_DIR" "$HOME_DIR/data/xim-pkgindex"
setup_out="$(RUN --verbose info fixture@1 2>&1)" \
  || fail "setup: local fixture must resolve: $setup_out"

# The target payload contains 500 real dynamic ELFs. scan_payload invokes
# patchelf exactly twice for each one: interpreter, then rpath.
PAYLOAD="$HOME_DIR/data/xpkgs/xim-x-fixture/1.0.0"
mkdir -p "$PAYLOAD/bin"
for i in $(seq 1 500); do
  cp --reflink=auto /bin/true "$PAYLOAD/bin/elf-$i"
done

# A historical glibc payload makes the runtime probe observable without using
# the host's glibc. The fake loader is the recorder; quick mode must not call
# it, and a fixture-scoped deep audit must not widen to it.
GLIBC="$HOME_DIR/data/xpkgs/xim-x-glibc/9"
mkdir -p "$GLIBC/bin" "$GLIBC/lib64"
cat > "$GLIBC/lib64/ld-linux-x86-64.so.2" <<'SH'
#!/bin/sh
printf 'getent\n' >> "$DOCTOR_GETENT_TRACE"
printf 'fixture:x:0:0:fixture:/tmp:/bin/sh\n'
SH
cat > "$GLIBC/bin/getent" <<'SH'
#!/bin/sh
exit 0
SH
chmod +x "$GLIBC/lib64/ld-linux-x86-64.so.2" "$GLIBC/bin/getent"

# A registered program whose payload exists but whose named executable does
# not. The finding stays in quick mode, but resolving its package remedy is a
# deep-only operation and must never recurse through `xlings info`.
python3 - "$HOME_DIR" "$PAYLOAD" <<'PY'
import json, pathlib, sys
home = pathlib.Path(sys.argv[1])
p = home / ".xlings.json"
data = json.loads(p.read_text())
data.setdefault("versions", {})["fixture-tool"] = {
    "filename": "fixture-tool",
    "type": "program",
    "versions": {
        "xim:1.0.0": {
            "kind": "program",
            "path": str(pathlib.Path(sys.argv[2]) / "bin"),
        }
    },
}
p.write_text(json.dumps(data, indent=2))
PY

: > "$PATCHELF_TRACE"
: > "$GETENT_TRACE"
quick_rc=0
RUN_WATCHED self doctor || quick_rc=$?
quick_patchelf="$(wc -l < "$PATCHELF_TRACE")"
quick_getent="$(wc -l < "$GETENT_TRACE")"
quick_child="$(wc -l < "$CHILD_TRACE")"

: > "$PATCHELF_TRACE"
: > "$GETENT_TRACE"
scope_only_rc=0
RUN self doctor --scope fixture@1 >"$OUT_FILE" 2>&1 || scope_only_rc=$?
scope_only_out="$(tr -d '\0' < "$OUT_FILE")"
scope_only_patchelf="$(wc -l < "$PATCHELF_TRACE")"
scope_only_getent="$(wc -l < "$GETENT_TRACE")"

: > "$PATCHELF_TRACE"
: > "$GETENT_TRACE"
rm -f "$FIRST_CALL_TRACE"
deep_rc=0
RUN_WATCHED self doctor --deep --scope fixture@1 || deep_rc=$?
deep_patchelf="$(wc -l < "$PATCHELF_TRACE")"
deep_getent="$(wc -l < "$GETENT_TRACE")"
deep_child="$(wc -l < "$CHILD_TRACE")"
deep_first="$(cat "$FIRST_CALL_TRACE" 2>/dev/null || true)"
deep_out="$(tr -d '\0' < "$OUT_FILE")"

errors=()
[[ "$quick_rc" -eq 1 ]] \
  || errors+=("quick exited $quick_rc, expected the seeded broken-payload finding")
[[ "$quick_patchelf" -eq 0 ]] \
  || errors+=("quick invoked patchelf $quick_patchelf time(s), expected 0")
[[ "$quick_getent" -eq 0 ]] \
  || errors+=("quick invoked getent $quick_getent time(s), expected 0")
[[ "$quick_child" -eq 0 ]] \
  || errors+=("quick spawned child xlings $quick_child time(s), expected 0")
[[ "$scope_only_rc" -eq 2 ]] \
  || errors+=("--scope without --deep exited $scope_only_rc, expected 2")
[[ "$scope_only_patchelf" -eq 0 && "$scope_only_getent" -eq 0 ]] \
  || errors+=("--scope without --deep performed deep work")
grep -q -- "--deep" <<<"$scope_only_out" \
  || errors+=("--scope without --deep did not explain the required depth")
[[ "$deep_rc" -eq 1 ]] \
  || errors+=("deep scope exited $deep_rc, expected the seeded broken-payload finding: ${deep_out//$'\n'/ }")
[[ "$deep_patchelf" -eq 1000 ]] \
  || errors+=("deep scope invoked patchelf $deep_patchelf time(s), expected 1000")
[[ "$deep_getent" -eq 0 ]] \
  || errors+=("deep scope widened into glibc getent ($deep_getent call(s))")
[[ "$deep_child" -eq 0 ]] \
  || errors+=("deep spawned child xlings $deep_child time(s), expected 0")
grep -q "fixture@1" <<<"$deep_out" \
  || errors+=("deep output did not announce scope fixture@1")
[[ "$deep_first" == "scope-announced" ]] \
  || errors+=("deep scope was not visible before the first patchelf call")

# A scope that cannot be proved unique is an input error. It must not silently
# widen into the whole store, because that recreates the default hang under a
# typo precisely when the user was trying to bound it.
: > "$PATCHELF_TRACE"
: > "$GETENT_TRACE"
missing_rc=0
RUN self doctor --deep --scope missing@1 >"$OUT_FILE" 2>&1 || missing_rc=$?
missing_out="$(tr -d '\0' < "$OUT_FILE")"
[[ "$missing_rc" -eq 2 ]] \
  || errors+=("missing scope exited $missing_rc, expected 2")
[[ "$(wc -l < "$PATCHELF_TRACE")" -eq 0 ]] \
  || errors+=("missing scope widened into payload scanning")
[[ "$(wc -l < "$GETENT_TRACE")" -eq 0 ]] \
  || errors+=("missing scope widened into getent probing")
grep -q "missing@1" <<<"$missing_out" \
  || errors+=("missing-scope error did not name missing@1")

# Catalog resolution alone is insufficient proof of an audit scope: the exact
# selected payload must exist locally. A known-but-uninstalled version fails
# closed instead of turning into an empty successful audit or an all-store one.
: > "$PATCHELF_TRACE"
: > "$GETENT_TRACE"
uninstalled_rc=0
RUN self doctor --deep --scope fixture@2 >"$OUT_FILE" 2>&1 \
  || uninstalled_rc=$?
uninstalled_out="$(tr -d '\0' < "$OUT_FILE")"
[[ "$uninstalled_rc" -eq 2 ]] \
  || errors+=("uninstalled scope exited $uninstalled_rc, expected 2")
[[ "$(wc -l < "$PATCHELF_TRACE")" -eq 0 ]] \
  || errors+=("uninstalled scope widened into payload scanning")
[[ "$(wc -l < "$GETENT_TRACE")" -eq 0 ]] \
  || errors+=("uninstalled scope widened into getent probing")
grep -qi "no local payload" <<<"$uninstalled_out" \
  || errors+=("uninstalled-scope error did not explain the missing local payload")
grep -q "fixture@2" <<<"$uninstalled_out" \
  || errors+=("uninstalled-scope error did not name fixture@2")

DUP_INDEX="$RUNTIME_DIR/dup-pkgindex"
cp -r "$INDEX_DIR" "$DUP_INDEX"
rm -f "$DUP_INDEX/.xlings-index-cache.json"
printf 'depth-duplicate-v1\n' > "$DUP_INDEX/.xlings-index-version"
ln -s "$DUP_INDEX" "$HOME_DIR/data/dup"
python3 - "$HOME_DIR" "$DUP_INDEX" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1], ".xlings.json")
data = json.loads(p.read_text())
data["index_repos"].append({"name": "dup", "url": sys.argv[2]})
p.write_text(json.dumps(data, indent=2))
PY
: > "$PATCHELF_TRACE"
: > "$GETENT_TRACE"
ambiguous_rc=0
RUN self doctor --deep --scope fixture@1 >"$OUT_FILE" 2>&1 || ambiguous_rc=$?
ambiguous_out="$(tr -d '\0' < "$OUT_FILE")"
[[ "$ambiguous_rc" -eq 2 ]] \
  || errors+=("ambiguous scope exited $ambiguous_rc, expected 2")
[[ "$(wc -l < "$PATCHELF_TRACE")" -eq 0 ]] \
  || errors+=("ambiguous scope widened into payload scanning")
grep -qi "ambiguous" <<<"$ambiguous_out" \
  || errors+=("ambiguous-scope error did not explain the ambiguity")

# `--fix` retains the historical deep detection surface; dry-run controls
# writes only. One ELF is enough to distinguish implied deep from quick.
FIX_HOME="$RUNTIME_DIR/fix-home"
mkdir -p "$FIX_HOME/subos/default/bin" "$FIX_HOME/data/xpkgs/audit-only/1/bin"
cp "$XLINGS_BIN" "$FIX_HOME/xlings"
cat > "$FIX_HOME/.xlings.json" <<EOF
{"mirror":"GLOBAL","index_repos":[{"name":"xim","url":"$INDEX_DIR"}]}
EOF
DOCTOR_HOME_OVERRIDE="$FIX_HOME" RUN self init >/dev/null 2>&1 \
  || fail "setup: fix home init failed"
cp --reflink=auto /bin/true "$FIX_HOME/data/xpkgs/audit-only/1/bin/elf"

for fix_args in "--fix --dry-run" "--fix"; do
  : > "$PATCHELF_TRACE"
  rm -f "$FIRST_CALL_TRACE"
  fix_rc=0
  # shellcheck disable=SC2086
  DOCTOR_HOME_OVERRIDE="$FIX_HOME" RUN self doctor $fix_args \
    >"$OUT_FILE" 2>&1 || fix_rc=$?
  [[ "$(wc -l < "$PATCHELF_TRACE")" -gt 0 ]] \
    || errors+=("doctor $fix_args did not imply deep detection (rc=$fix_rc)")
done

if ((${#errors[@]})); then
  printf '[project-e2e] FAIL: %s\n' "${errors[@]}" >&2
  exit 1
fi

log "PASS: quick doctor is bounded and deep scope is explicit"
