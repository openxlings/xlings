#!/usr/bin/env bash
# Real-machine simulation of an 0.4.69 -> latest upgrade.
#
# Why this exists
# ---------------
# Every upgrade check so far has been run against a *synthesized* legacy home:
# a state file hand-written to look like what 0.4.69 leaves behind. That keeps
# missing the interesting failures, because the interesting failures come from
# what 0.4.69 actually WROTE -- recipe versions that have since changed, host
# detection that used to be wrong, payload layouts that have since moved.
#
# On 2026-07-28 a real user's home carried an llvm@20.1.7 whose recorded path
# was `/home/<u>/.xlings\data\xpkgs\xim-x-llvm\20.1.7` (backslashes, on Linux)
# with `.exe` aliases -- a Windows payload installed on Linux by an old client.
# The upgraded client could neither reinstall it (payload mismatch) nor remove
# it (removal target outside the owned selection). No synthesized fixture had
# produced that shape, and none would have.
#
# So: install the real 0.4.69 release, install the real packages through it,
# run the real `self update`, then drive the real commands a user would drive
# afterwards. Record every exit code. Do not fix anything here.
#
# This script never touches the host xlings home. Everything lives under
# SIM_ROOT with its own HOME, and XLINGS_HOME is left unset so the client
# derives it from HOME exactly as it does for a real user.
#
# Usage:
#   simulate-legacy-upgrade.sh [phase ...]
#
# Phases (default: all of them, in order):
#   bootstrap   fetch + `self install` the OLD release
#   populate    install the core packages, several at two versions
#   snapshot    record the pre-upgrade state
#   update      `xlings self update`
#   exercise    use / list / remove / install against the pre-upgrade packages
#   report      summarize every recorded step
#
# Phases are checkpointed: a completed phase is skipped on re-run, so the
# multi-GB `populate` is paid once. `SIM_FORCE=1` re-runs regardless.

set -uo pipefail   # deliberately NOT -e: a failing step is the DATA

OLD_VERSION="${OLD_VERSION:-0.4.69}"
SIM_ROOT="${SIM_ROOT:-/tmp/claude-1000/-home-speak-workspace-github-openxlings-xlings/d5d81c3c-1e86-4365-ad2b-a4873c7278a2/scratchpad/legacy-upgrade-sim}"

USER_HOME="$SIM_ROOT/user"
XHOME="$USER_HOME/.xlings"
STAGE="$SIM_ROOT/stage"
CKPT="$SIM_ROOT/checkpoints"
TRANSCRIPT="$SIM_ROOT/transcript.log"
STEPS="$SIM_ROOT/steps.tsv"

mkdir -p "$SIM_ROOT" "$CKPT"

# The host is behind a local proxy; without it every download in the isolated
# env either hangs or silently falls back to a slow direct route, which would
# read as "the upgrade is slow" rather than "the test harness is misconfigured".
PROXY_ENV=()
for v in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY ALL_PROXY; do
  [[ -n "${!v:-}" ]] && PROXY_ENV+=("$v=${!v}")
done

SYSPATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# Every invocation of the simulated client. HOME points at the sim user and
# XLINGS_HOME is deliberately absent: a real user does not set it, and passing
# it would paper over any bug in home derivation.
xl() {
  ( cd "$SIM_ROOT" && env -i \
      HOME="$USER_HOME" \
      PATH="$XHOME/subos/current/bin:$XHOME/bin:$SYSPATH" \
      TERM=dumb \
      "${PROXY_ENV[@]}" \
      "$XHOME/bin/xlings" "$@" )
}

# A shim the client installed, invoked the way a user's shell would invoke it
# (through subos/current/bin) rather than through xlings. 0.4.69 has no `run`
# subcommand, and going through the shim is what actually proves the switch.
shim() {
  local prog="$1"; shift
  ( cd "$SIM_ROOT" && env -i \
      HOME="$USER_HOME" \
      PATH="$XHOME/subos/current/bin:$XHOME/bin:$SYSPATH" \
      TERM=dumb \
      "${PROXY_ENV[@]}" \
      "$XHOME/subos/current/bin/$prog" "$@" )
}

# The client's own recorded version. `xlings version` did not exist in 0.4.69,
# so asking the CLI would report a difference between the two clients that is
# about the CLI surface, not about which binary is installed.
recorded_version() {
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("version","?"))' \
    "$XHOME/.xlings.json" 2>/dev/null || echo "?"
}

now_ms() { date +%s%3N; }

# The CLI emits ANSI unconditionally (NO_COLOR is honoured only by the shell
# profile), and a transcript full of escape codes is unreadable. Raw output is
# still kept verbatim under out/.
strip_ansi() { sed -r 's/\x1B\[[0-9;]*[A-Za-z]//g; s/\x1B\[[0-9;]*m//g'; }

log()  { printf '%s\n' "$*" | tee -a "$TRANSCRIPT"; }
head_() { log ""; log "=============== $* ==============="; }

# run_step <id> <description> -- <command...>
#
# Records rc and wall time for every step whether it passes or fails, because
# the point of the run is the failure list. The output is teed rather than
# swallowed so a hang is visible while it is happening.
run_step() {
  local id="$1" desc="$2"; shift 2
  [[ "${1:-}" == "--" ]] && shift
  local start rc out
  log ""
  log "--- [$id] $desc"
  log "    \$ $*"
  start="$(now_ms)"
  out="$SIM_ROOT/out/$id.log"
  mkdir -p "$SIM_ROOT/out"
  "$@" >"$out" 2>&1
  rc=$?
  local ms=$(( $(now_ms) - start ))
  strip_ansi < "$out" | sed 's/^/    | /' | tee -a "$TRANSCRIPT"
  printf '%s\t%s\t%s\t%s\n' "$id" "$rc" "$ms" "$desc" >> "$STEPS"
  if [[ $rc -eq 0 ]]; then
    log "    => rc=0 (${ms}ms)"
  else
    log "    => rc=$rc (${ms}ms)   <<< BLOCKER"
  fi
  return 0   # never abort the run on a step failure
}

phase_done() { [[ -f "$CKPT/$1" && -z "${SIM_FORCE:-}" ]]; }
mark_done()  { : > "$CKPT/$1"; }

# ---------------------------------------------------------------- bootstrap
phase_bootstrap() {
  head_ "bootstrap: xlings $OLD_VERSION into $XHOME"
  local tarball="xlings-$OLD_VERSION-linux-x86_64.tar.gz"
  mkdir -p "$STAGE" "$USER_HOME"

  if [[ ! -f "$STAGE/$tarball" ]]; then
    run_step bootstrap-download "fetch $tarball" -- \
      gh release download "v$OLD_VERSION" --repo openxlings/xlings \
        --pattern "$tarball" --dir "$STAGE"
  fi
  [[ -f "$STAGE/$tarball" ]] || { log "FATAL: no $tarball"; return 1; }

  rm -rf "$STAGE/pkg"; mkdir -p "$STAGE/pkg"
  run_step bootstrap-extract "extract release package" -- \
    tar -xzf "$STAGE/$tarball" -C "$STAGE/pkg" --strip-components=1

  # `self install` derives the target home from HOME. XLINGS_HOME must stay
  # unset here: with it set, self install has historically written to one
  # place and recorded another.
  run_step bootstrap-self-install "self install (old client)" -- \
    env -i HOME="$USER_HOME" PATH="$SYSPATH" TERM=dumb "${PROXY_ENV[@]}" \
      "$STAGE/pkg/bin/xlings" self install

  log "    installed version: $(recorded_version)"
  mark_done bootstrap
}

# ---------------------------------------------------------------- populate
# Two versions where the package has two, because the single-version case
# cannot exercise `use` and hides every group/binding bug.
POPULATE=(
  "gcc@15.1.0"
  "gcc@16.1.0"
  "llvm@20.1.7"
  "llvm@22.1.8"
  "mcpp"
)

phase_populate() {
  head_ "populate: install core packages with the OLD client"
  for spec in "${POPULATE[@]}"; do
    run_step "install-old-${spec//[@\/]/-}" "old client installs $spec" -- \
      xl install "$spec" -y
  done
  mark_done populate
}

# ---------------------------------------------------------------- snapshot
phase_snapshot() {
  head_ "snapshot: state as written by $OLD_VERSION"
  run_step snap-list "list installed" -- xl list
  cp "$XHOME/.xlings.json" "$SIM_ROOT/state-before.json" 2>/dev/null \
    && log "    saved state-before.json"
  mark_done snapshot
}

# ---------------------------------------------------------------- update
phase_update() {
  head_ "update: xlings self update"
  local before after
  before="$(recorded_version)"
  run_step self-update "self update to latest" -- xl self update
  after="$(recorded_version)"
  log "    version: $before -> $after"
  # The whole point of `self update` is that this changes. It returning 0 is
  # not evidence of anything -- that exact false pass was the field bug fixed
  # in #424 -- so the recorded version is asserted separately.
  if [[ "$before" == "$after" ]]; then
    log "    => VERSION DID NOT CHANGE   <<< BLOCKER"
    printf '%s\t%s\t%s\t%s\n' self-update-effective 1 0 \
      "self update left the recorded version at $before" >> "$STEPS"
  else
    printf '%s\t%s\t%s\t%s\n' self-update-effective 0 0 \
      "recorded version moved $before -> $after" >> "$STEPS"
  fi
  mark_done update
}

# ---------------------------------------------------------------- exercise
# What a user does the day after upgrading. Each is independent: a failure in
# one must not stop the next, or the first blocker hides all the others.
phase_exercise() {
  head_ "exercise: drive the pre-upgrade packages with the NEW client"

  run_step ex-list        "list installed"        -- xl list
  run_step ex-list-all    "list --all"            -- xl list --all
  run_step ex-doctor      "doctor"                -- xl doctor

  # switching between the two installed versions, both directions
  run_step ex-use-gcc-15  "use gcc 15.1.0"        -- xl use gcc 15.1.0
  run_step ex-gcc-v-15    "gcc --version through the shim" -- shim gcc --version
  run_step ex-use-gcc-16  "use gcc 16.1.0"        -- xl use gcc 16.1.0
  run_step ex-gcc-v-16    "gcc --version through the shim" -- shim gcc --version

  run_step ex-use-llvm-20 "use llvm 20.1.7"       -- xl use llvm 20.1.7
  run_step ex-use-llvm-22 "use llvm 22.1.8"       -- xl use llvm 22.1.8

  # reinstall of something the OLD client registered: the field failure
  run_step ex-reinstall-llvm "new client reinstalls llvm (already installed)" -- \
    xl install llvm -y
  run_step ex-reinstall-gcc  "new client reinstalls gcc (already installed)" -- \
    xl install gcc -y
  run_step ex-reinstall-mcpp "new client reinstalls mcpp (already installed)" -- \
    xl install mcpp -y

  # removal of something the OLD client registered: the other field failure
  run_step ex-remove-llvm  "new client removes llvm"  -- xl remove llvm -y
  run_step ex-remove-gcc15 "new client removes gcc@15.1.0" -- xl remove gcc@15.1.0 -y

  # and can it be put back afterwards
  run_step ex-install-back-llvm "install llvm again after removal" -- \
    xl install llvm -y

  run_step ex-doctor-after "doctor after the churn" -- xl doctor
  cp "$XHOME/.xlings.json" "$SIM_ROOT/state-after.json" 2>/dev/null \
    && log "    saved state-after.json"
  mark_done exercise
}

# ---------------------------------------------------------------- report
phase_report() {
  head_ "report"
  [[ -f "$STEPS" ]] || { log "no steps recorded"; return 0; }
  local total fails
  total="$(wc -l < "$STEPS")"
  fails="$(awk -F'\t' '$2 != 0' "$STEPS" | wc -l)"
  log ""
  log "steps: $total   blockers: $fails"
  log ""
  printf '%-28s %5s %10s  %s\n' ID RC MS DESC | tee -a "$TRANSCRIPT"
  awk -F'\t' '{printf "%-28s %5s %10s  %s\n", $1, $2, $3, $4}' "$STEPS" \
    | tee -a "$TRANSCRIPT"
  if [[ "$fails" -gt 0 ]]; then
    log ""
    log "BLOCKERS:"
    awk -F'\t' '$2 != 0 {printf "  [%s] rc=%s  %s\n", $1, $2, $4}' "$STEPS" \
      | tee -a "$TRANSCRIPT"
  fi
}

main() {
  local phases=("$@")
  [[ ${#phases[@]} -eq 0 ]] && \
    phases=(bootstrap populate snapshot update exercise report)
  log "### simulation start  $(date -Is)  root=$SIM_ROOT"
  for p in "${phases[@]}"; do
    if [[ "$p" != report ]] && phase_done "$p"; then
      log ""
      log "=============== $p: already done, skipping (SIM_FORCE=1 to redo)"
      continue
    fi
    "phase_$p"
  done
}

main "$@"
