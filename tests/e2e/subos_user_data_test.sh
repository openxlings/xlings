#!/usr/bin/env bash
# E2E: a subos's user data leaves only when the user says so.
#
# THE RULE (AGENTS.md, "SubOS user data"): what a reinstall can put back any
# flow may delete. A subos's home -- and any file xlings cannot prove it owns --
# only a deletion the USER initiated may remove, after they confirmed it at a
# terminal or with an explicit `-y` / `"yes": true`.
#
# MEASURED ON 2026.9.20.1, each one a case below:
#   - `subos remove X` and NDJSON remove_subos deleted X with its home, no
#     question, exit 0;
#   - `subos new X` over an unregistered X/ adopted it silently and said
#     "created"; the next remove deleted files xlings never made;
#   - doctor's remedy for an unreadable subos config was `subos remove`;
#   - `self clean` collected the packages of a subos whose config it could not
#     read, as if that subos used nothing.
# And nothing recorded any of it, which is why the data loss that prompted
# this could not be attributed. Every deletion now leaves a line.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

HOME_DIR="$(runtime_home_dir subos_user_data_home)"
assert_home_is_isolated "$HOME_DIR"
BIN="$(find_xlings_bin)"
cleanup() { rm -rf "$HOME_DIR" "$HOME_DIR-fakeuser"; }
trap cleanup EXIT
cleanup
mkdir -p "$HOME_DIR"
echo '{"mirror":"CN","lang":"en"}' > "$HOME_DIR/.xlings.json"

# stdin is never a terminal here: "nobody to ask" is the case under test.
X() { ( cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$HOME_DIR" "$BIN" "$@" </dev/null ); }
seed() { mkdir -p "$1/home/u/project"; echo "user work" > "$1/home/u/project/notes.txt"; }
has_data() { [[ -f "$1/home/u/project/notes.txt" ]]; }
LOG="$HOME_DIR/logs/destructive.ndjson"

X self init >/dev/null 2>&1 || fail "self init failed"

# ── AC10/AC11: CLI remove asks; without an answer nothing goes ─────────
X subos new s1 >/dev/null 2>&1 || fail "setup: subos new s1"
seed "$HOME_DIR/subos/s1"
set +e; OUT="$(X subos remove s1 2>&1)"; RC=$?; set -e
[[ $RC -eq 2 ]] || fail "AC10: unconfirmed remove exited $RC, want 2: $OUT"
has_data "$HOME_DIR/subos/s1" || fail "AC10: unconfirmed remove deleted the home"
case "$OUT" in *"home "*"file(s)"*"nothing was removed"*) ;; *) fail "AC11: the refusal does not say what the home holds: $OUT" ;; esac
case "$OUT" in *"-y"*) ;; *) fail "AC11: the refusal does not say how to confirm: $OUT" ;; esac
log "  ok AC10/AC11: 'subos remove' with nobody to ask deletes nothing, says what it would, exit 2"

X subos remove s1 -y >/dev/null 2>&1 || fail "AC10: 'subos remove s1 -y' failed"
[[ ! -d "$HOME_DIR/subos/s1" ]] || fail "AC10: confirmed remove left s1"
grep -q '"op":"subos-remove".*"confirmedBy":"-y"' "$LOG" \
  || grep -q '"confirmedBy":"-y".*"op":"subos-remove"' "$LOG" \
  || fail "AC18: no destructive-log line for the confirmed remove: $(cat "$LOG" 2>/dev/null)"
log "  ok AC10/AC18: with -y it is removed, and the log says how it was confirmed"

# ── AC10 (interface): an agent is told, not obeyed ─────────────────────
X subos new s2 >/dev/null 2>&1 || fail "setup: subos new s2"
seed "$HOME_DIR/subos/s2"
set +e; OUT="$(X interface remove_subos --args '{"name":"s2"}' 2>/dev/null)"; RC=$?; set -e
[[ $RC -eq 2 ]] || fail "AC10: unconfirmed remove_subos exited $RC: $OUT"
has_data "$HOME_DIR/subos/s2" || fail "AC10: unconfirmed remove_subos deleted the home"
case "$OUT" in *'\"yes\": true'*"user"*|*"user"*'\"yes\": true'*) ;; *) fail "AC10: the agent was not told the decision is the user's: $OUT" ;; esac
X interface remove_subos --args '{"name":"s2","yes":true}' >/dev/null 2>&1 \
  || fail "AC10: remove_subos with yes:true failed"
[[ ! -d "$HOME_DIR/subos/s2" ]] || fail "AC10: remove_subos yes:true left s2"
grep -q '"confirmedBy":"yes:true"' "$LOG" || fail "AC18: no log line for the interface remove"
log "  ok AC10: remove_subos without yes informs the agent; with yes:true it removes"

# ── AC12: the one deletion entry point refuses what is not a subos ─────
python3 - "$HOME_DIR/.xlings.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
d.setdefault("subos", {})[".."] = {"dir": ""}
d["subos"]["current"] = {"dir": ""}
json.dump(d, open(p, "w"))
PY
set +e; OUT="$(X subos remove .. -y 2>&1)"; RC=$?; set -e
[[ $RC -ne 0 ]] || fail "AC12: removing '..' succeeded"
[[ -f "$HOME_DIR/.xlings.json" && -d "$HOME_DIR/subos/default" ]] || fail "AC12: '..' reached the home"
set +e; OUT2="$(X subos remove current -y 2>&1)"; RC=$?; set -e
[[ $RC -ne 0 ]] || fail "AC12: removing 'current' succeeded"
[[ -d "$HOME_DIR/subos/default" ]] || fail "AC12: removing 'current' deleted what it points at"
case "$OUT$OUT2" in *"refusing to delete"*) ;; *) fail "AC12: no refusal reason: $OUT $OUT2" ;; esac
python3 - "$HOME_DIR/.xlings.json" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
d["subos"].pop("..", None); d["subos"].pop("current", None)
json.dump(d, open(p, "w"))
PY
log "  ok AC12: '..' and 'current' are refused by the deletion entry point itself"

# ── AC13: never delete through a live mount (Linux) ────────────────────
if [[ "$(uname -s)" == "Linux" ]] && unshare -rm true 2>/dev/null; then
  X subos new m1 >/dev/null 2>&1 || fail "setup: subos new m1"
  SRC="$HOME_DIR-fakeuser/mounted"; mkdir -p "$SRC" "$HOME_DIR/subos/m1/home/mnt"
  echo precious > "$SRC/elsewhere.txt"
  set +e
  OUT="$(unshare -rm bash -c 'mount --bind "$1" "$2" && cd /tmp && env -u XLINGS_PROJECT_DIR XLINGS_HOME="$3" "$4" subos remove m1 -y </dev/null' \
          _ "$SRC" "$HOME_DIR/subos/m1/home/mnt" "$HOME_DIR" "$BIN" 2>&1)"
  RC=$?
  set -e
  [[ -f "$SRC/elsewhere.txt" ]] || fail "AC13: deleting through a bind mount erased the mounted directory"
  [[ $RC -ne 0 ]] || fail "AC13: remove with a live mount under the subos succeeded: $OUT"
  case "$OUT" in *"live mount"*) ;; *) fail "AC13: refusal did not name the mount: $OUT" ;; esac
  X subos remove m1 -y >/dev/null 2>&1 || true
  log "  ok AC13: a live bind mount under the subos stops the deletion"
else
  log "  SKIP AC13: no unprivileged mount namespace here (unshare -rm); not measured"
fi

# ── AC14: an unregistered directory is not silently adopted ────────────
mkdir -p "$HOME_DIR/subos/orphan"; echo '{"workspace":{}}' > "$HOME_DIR/subos/orphan/.xlings.json"
seed "$HOME_DIR/subos/orphan"
set +e; OUT="$(X subos new orphan 2>&1)"; RC=$?; set -e
[[ $RC -eq 2 ]] || fail "AC14: adopting without an answer exited $RC: $OUT"
case "$OUT" in *"already exists and is not a registered subos"*) ;; *) fail "AC14: no explanation: $OUT" ;; esac
OUT="$(X subos new orphan -y 2>&1)" || fail "AC14: 'subos new orphan -y' failed: $OUT"
case "$OUT" in *"adopted"*) ;; *) fail "AC14: an adoption was reported as: $OUT" ;; esac
has_data "$HOME_DIR/subos/orphan" || fail "AC14: adopting changed the home"
log "  ok AC14: an existing directory is adopted only with -y, reported as 'adopted', home intact"

# ── AC15: an unreadable subos config is repaired, not removed ──────────
X subos new broken >/dev/null 2>&1 || fail "setup: subos new broken"
seed "$HOME_DIR/subos/broken"
echo '{ not json' > "$HOME_DIR/subos/broken/.xlings.json"
OUT="$(X self doctor 2>&1 || true)"
case "$OUT" in *"subos remove"*) fail "AC15: doctor still advises 'subos remove' for a bad config: $OUT" ;; esac
X self doctor --fix >/dev/null 2>&1 || true
has_data "$HOME_DIR/subos/broken" || fail "AC15: doctor --fix touched the home"
ls "$HOME_DIR/subos/broken/" -a | grep -q '^\.xlings\.json\.corrupt-' \
  || fail "AC15: the unreadable config was not kept"
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$HOME_DIR/subos/broken/.xlings.json" \
  || fail "AC15: no readable config was written"
log "  ok AC15: --fix keeps the bad config aside, writes a fresh one, home untouched"

# ── AC16: GC refuses when it cannot read what is in use ────────────────
mkdir -p "$HOME_DIR/data/xpkgs/xim-x-gcfake/1.0/bin"; echo x > "$HOME_DIR/data/xpkgs/xim-x-gcfake/1.0/bin/gcfake"
X subos new gcuser >/dev/null 2>&1 || fail "setup: subos new gcuser"
python3 - "$HOME_DIR" <<'PY'
import json, sys
h = sys.argv[1]
p = h + "/.xlings.json"; d = json.load(open(p))
d.setdefault("versions", {})["gcfake"] = {"versions": {"1.0": {"path": h + "/data/xpkgs/xim-x-gcfake/1.0/bin"}}}
json.dump(d, open(p, "w"))
q = h + "/subos/gcuser/.xlings.json"; e = json.load(open(q))
e.setdefault("workspace", {})["gcfake"] = "1.0"
json.dump(e, open(q, "w"))
PY
echo '{ broken' > "$HOME_DIR/subos/gcuser/.xlings.json"
set +e; OUT="$(X self clean 2>&1)"; RC=$?; set -e
[[ $RC -ne 0 ]] || fail "AC16: self clean succeeded with an unreadable subos config: $OUT"
case "$OUT" in *"gc refused"*"gcuser"*) ;; *) fail "AC16: refusal did not name the subos: $OUT" ;; esac
[[ -f "$HOME_DIR/data/xpkgs/xim-x-gcfake/1.0/bin/gcfake" ]] || fail "AC16: a package the unreadable subos uses was collected"
log "  ok AC16: GC refuses and names the subos it could not read; nothing collected"

# ── AC20: `self clean` does not take a whole home for a cache ──────────
FAKE="$HOME_DIR-fakeuser/home"; mkdir -p "$FAKE/.xlings/subos/default" "$FAKE/.xlings/data"
echo '{}' > "$FAKE/.xlings.json"
( cd /tmp && env -u XLINGS_PROJECT_DIR HOME="$FAKE" XLINGS_HOME="$FAKE" "$BIN" self clean </dev/null >/dev/null 2>&1 || true )
[[ -d "$FAKE/.xlings/subos/default" ]] || fail "AC20: self clean with XLINGS_HOME=\$HOME deleted \$HOME/.xlings"
log "  ok AC20: with XLINGS_HOME=\$HOME, self clean keeps \$HOME/.xlings"

log "PASS: subos user data leaves only when the user says so, and every deletion is recorded"
