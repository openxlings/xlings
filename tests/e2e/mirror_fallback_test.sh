#!/usr/bin/env bash
# E2E: verify the github mirror fallback wiring.
#
# Strategy: instead of mocking http servers (which is fragile under CI
# port allocation), inspect the verbose log. xlings emits a
# "[mirror] ..." log line whenever fallback succeeds, and the debug-level
# attempt log enumerates each URL in the candidate list. We can read
# those to prove:
#   1. With XLINGS_MIRROR_FALLBACK=off  → only the original URL is in
#      the attempt list (1 candidate).
#   2. With XLINGS_MIRROR_FALLBACK=auto → multiple candidates appear
#      including a known mirror host (ghfast / kkgithub / etc).
#
# We run this against a github URL that DOES NOT NEED to reach the
# network — the candidate-list construction happens before any network
# I/O, and we abort with cancellation before the HTTP request fires by
# pointing at a non-existent localhost port.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/mirror_fallback"
LOG="$RUNTIME_DIR/run.log"

source "$ROOT_DIR/tests/e2e/project_test_lib.sh"
BIN_SRC="$(find_xlings_bin)"

fail() { echo "FAIL: $*" >&2; exit 1; }
log()  { echo "[mirror-e2e] $*"; }

[[ -x "$BIN_SRC" ]] || fail "built xlings binary not found at $BIN_SRC"

# --- Setup portable home ---
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR/portable/bin" "$RUNTIME_DIR/portable/data"
cp "$BIN_SRC" "$RUNTIME_DIR/portable/bin/xlings"

cat > "$RUNTIME_DIR/portable/.xlings.json" <<EOF
{
  "version": "0.4.8",
  "mirror": "GLOBAL",
  "activeSubos": "default",
  "subos": { "default": { "dir": "" } }
}
EOF

XLINGS_HOME="$RUNTIME_DIR/portable" \
  "$RUNTIME_DIR/portable/bin/xlings" self init >/dev/null 2>&1 \
  || fail "self init failed"

# ──────────────────────────────────────────────────────────────────
# Scenario 1: XLINGS_MIRROR_FALLBACK=off
#   Trigger an index-repo clone that will fail. The candidate list
#   should contain ONLY the original URL.
# ──────────────────────────────────────────────────────────────────
log "S1: XLINGS_MIRROR_FALLBACK=off → single-URL candidate list"

# Point xim-indexrepos at a URL that doesn't exist; we don't care about
# the clone succeeding — we only inspect the attempt log. We must
# rewrite the JSON before each scenario because sync_all_repos prunes
# failed entries on save.
mkdir -p "$RUNTIME_DIR/portable/data/xim-index-repos"
write_indexrepos() {
  cat > "$RUNTIME_DIR/portable/data/xim-index-repos/xim-indexrepos.json" <<EOF
{ "test-repo": "$1" }
EOF
}

GH_BAD_URL="https://github.com/openxlings/this-repo-deliberately-does-not-exist-xlings-mirror-test.git"

write_indexrepos "$GH_BAD_URL"

set +e
XLINGS_HOME="$RUNTIME_DIR/portable" XLINGS_MIRROR_FALLBACK=off \
  XLINGS_NON_INTERACTIVE=1 \
  "$RUNTIME_DIR/portable/bin/xlings" --verbose update >"$LOG" 2>&1
rc=$?
set -e

# --verbose enables debug, so "cloning index repo attempt N/M" must appear.
attempts_off=$(grep -cE 'cloning index repo attempt [0-9]+/[0-9]+:' "$LOG" || true)
total_off=$(grep -oE 'attempt [0-9]+/[0-9]+:' "$LOG" | head -1 | grep -oE '/[0-9]+' | tr -d '/' || true)
log "  → $attempts_off attempt log lines, total candidates declared: ${total_off:-0}"
[[ -n "$total_off" ]] || fail "S1: no 'attempt' log lines found in --verbose output"
[[ "$total_off" == "1" ]] || \
  fail "S1: expected exactly 1 candidate with mirror_fallback=off, got $total_off"

# ──────────────────────────────────────────────────────────────────
# Scenario 2: XLINGS_MIRROR_FALLBACK=auto (default)
#   The same broken URL should now produce multiple candidates,
#   one per registered mirror that supports git.
# ──────────────────────────────────────────────────────────────────
log "S2: XLINGS_MIRROR_FALLBACK=auto → multi-URL candidate list"
write_indexrepos "$GH_BAD_URL"

set +e
XLINGS_HOME="$RUNTIME_DIR/portable" XLINGS_MIRROR_FALLBACK=auto \
  XLINGS_NON_INTERACTIVE=1 \
  "$RUNTIME_DIR/portable/bin/xlings" --verbose update >"$LOG" 2>&1
rc=$?
set -e

total_auto=$(grep -oE 'attempt [0-9]+/[0-9]+:' "$LOG" | head -1 | grep -oE '/[0-9]+' | tr -d '/' || true)
log "  → total candidates declared: ${total_auto:-0}"
[[ -n "$total_auto" ]] || fail "S2: no 'attempt' log lines in --verbose output"
[[ "$total_auto" -gt 1 ]] || \
  fail "S2: expected >1 candidate with mirror_fallback=auto, got $total_auto"

# At least one candidate must reference a known mirror host. We don't
# assert on a specific name — the registry might rotate priority — but
# at least one of the built-in mirrors should be present.
if ! grep -qE '(ghfast\.top|ghproxy\.net|kkgithub\.com|cdn\.jsdelivr\.net)' "$LOG"; then
  fail "S2: no built-in mirror host appeared in candidate list"
fi
log "  → built-in mirror host found in candidate list"

# ──────────────────────────────────────────────────────────────────
# Scenario 3: gitee URL is NOT expanded
#   Non-GitHub URLs must pass through unmodified.
# ──────────────────────────────────────────────────────────────────
log "S3: gitee URL → passthrough (1 candidate)"
write_indexrepos "https://gitee.com/d2learn/this-repo-deliberately-does-not-exist-xlings-mirror-test.git"

set +e
XLINGS_HOME="$RUNTIME_DIR/portable" XLINGS_MIRROR_FALLBACK=auto \
  XLINGS_NON_INTERACTIVE=1 \
  "$RUNTIME_DIR/portable/bin/xlings" --verbose update >"$LOG" 2>&1
rc=$?
set -e

total_gitee=$(grep -oE 'attempt [0-9]+/[0-9]+:' "$LOG" | head -1 | grep -oE '/[0-9]+' | tr -d '/' || true)
log "  → total candidates for gitee URL: ${total_gitee:-0}"
[[ "$total_gitee" == "1" ]] || \
  fail "S3: gitee URL should not be mirror-expanded, got $total_gitee candidates"

# ──────────────────────────────────────────────────────────────────
# Scenario 4: user-supplied github-mirrors.json fully replaces defaults
# ──────────────────────────────────────────────────────────────────
log "S4: user-supplied github-mirrors.json overrides defaults"

cat > "$RUNTIME_DIR/portable/data/github-mirrors.json" <<EOF
{
  "version": 1,
  "mirrors": [
    {
      "name": "user-only-mirror",
      "form": "prefix",
      "host": "user-only.example.invalid",
      "supports": ["release", "raw", "archive", "git"],
      "priority": 1
    }
  ]
}
EOF

write_indexrepos "$GH_BAD_URL"

set +e
XLINGS_HOME="$RUNTIME_DIR/portable" XLINGS_MIRROR_FALLBACK=auto \
  XLINGS_NON_INTERACTIVE=1 \
  "$RUNTIME_DIR/portable/bin/xlings" --verbose update >"$LOG" 2>&1
set -e

total_user=$(grep -oE 'attempt [0-9]+/[0-9]+:' "$LOG" | head -1 | grep -oE '/[0-9]+' | tr -d '/' || true)
log "  → total candidates with user-only override: ${total_user:-0}"
# Expect exactly 2: original + the single user-defined mirror.
[[ "$total_user" == "2" ]] || \
  fail "S4: user override should yield 2 candidates (orig + 1 mirror), got $total_user"
grep -q "user-only.example.invalid" "$LOG" \
  || fail "S4: user-only mirror host not in candidate list"
# Verify the built-in mirrors are NOT in the list — full replacement.
if grep -qE '(ghfast\.top|ghproxy\.net|kkgithub\.com)' "$LOG"; then
  fail "S4: built-in mirror leaked through despite user override"
fi
log "  → built-in defaults correctly fully replaced"

log "PASS: mirror fallback wiring verified across off/auto/non-github/user-override"
