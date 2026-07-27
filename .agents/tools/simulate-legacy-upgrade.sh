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
#   heal        `xlings self doctor --fix` — the migration pass
#   exercise    use / list / remove / install against the pre-upgrade packages
#   report      summarize every recorded step
#
# Phases are checkpointed: a completed phase is skipped on re-run, so the
# multi-GB `populate` is paid once. `SIM_FORCE=1` re-runs regardless.

set -uo pipefail   # deliberately NOT -e: a failing step is the DATA

OLD_VERSION="${OLD_VERSION:-0.4.69}"
SIM_ROOT="${SIM_ROOT:-${TMPDIR:-/tmp}/legacy-upgrade-sim}"

# ---------------------------------------------------------------- platform
#
# The three release platforms differ in more than the asset name, so each
# difference is named once here rather than sprinkled through the phases.
case "$(uname -s)" in
  Linux)                     SIM_OS=linux   ;;
  Darwin)                    SIM_OS=macosx  ;;
  MINGW*|MSYS*|CYGWIN*)      SIM_OS=windows ;;
  *) echo "unsupported host: $(uname -s)" >&2; exit 2 ;;
esac

case "$SIM_OS" in
  linux)   SIM_ARCH="$(uname -m)"; [[ "$SIM_ARCH" == aarch64 ]] || SIM_ARCH=x86_64 ;;
  macosx)  SIM_ARCH=arm64  ;;   # the only macOS asset published
  windows) SIM_ARCH=x86_64 ;;
esac

SIM_ASSET_EXT=$([[ "$SIM_OS" == windows ]] && echo zip || echo tar.gz)
SIM_EXE=$([[ "$SIM_OS" == windows ]] && echo .exe || echo "")

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
#
# Seeded with a harmless entry rather than left empty: macOS ships bash 3.2,
# where expanding an empty array under `set -u` is itself an "unbound
# variable" error. That took out every step on the macOS runner at 26ms each.
PROXY_ENV=("XLINGS_SIM=1")
for v in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY ALL_PROXY; do
  [[ -n "${!v:-}" ]] && PROXY_ENV+=("$v=${!v}")
done

SYSPATH="${SIM_SYSPATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}"

# Run something with the simulated user's environment.
#
# On POSIX the environment is wiped (`env -i`): the point is that the client
# derives everything from HOME, and an inherited XLINGS_HOME or PATH entry
# would paper over exactly the bugs being looked for. XLINGS_HOME is never
# passed -- a real user does not set it.
#
# Windows cannot take that: a scrubbed environment loses SYSTEMROOT, COMSPEC
# and the rest, and processes fail to start for reasons that have nothing to
# do with xlings. There the environment is inherited and only the variables
# that decide the home are overridden -- including USERPROFILE, which is what
# the Windows build reads rather than HOME.
run_isolated() {
  if [[ "$SIM_OS" == windows ]]; then
    ( cd "$SIM_ROOT" && env -u XLINGS_HOME -u XLINGS_BIN \
        HOME="$USER_HOME" USERPROFILE="$USER_HOME" \
        PATH="$XHOME/subos/current/bin:$XHOME/bin:$PATH" \
        "$@" )
  else
    ( cd "$SIM_ROOT" && env -i \
        HOME="$USER_HOME" \
        PATH="$XHOME/subos/current/bin:$XHOME/bin:$SYSPATH" \
        TERM=dumb \
        "${PROXY_ENV[@]}" \
        "$@" )
  fi
}

# Every invocation of the simulated client.
xl() { run_isolated "$XHOME/bin/xlings$SIM_EXE" "$@"; }

# A shim the client installed, invoked the way a user's shell would invoke it
# (through subos/current/bin) rather than through xlings. 0.4.69 has no `run`
# subcommand, and going through the shim is what actually proves the switch.
shim() {
  local prog="$1"; shift
  run_isolated "$XHOME/subos/current/bin/$prog$SIM_EXE" "$@"
}

# The client's own recorded version. `xlings version` did not exist in 0.4.69,
# so asking the CLI would report a difference between the two clients that is
# about the CLI surface, not about which binary is installed.
recorded_version() {
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("version","?"))' \
    "$XHOME/.xlings.json" 2>/dev/null || echo "?"
}

# `date +%s%3N` is a GNU extension: on macOS it prints the literal "%3N" and
# every duration comes out as a nonsense integer rather than failing.
now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

# The CLI emits ANSI unconditionally (NO_COLOR is honoured only by the shell
# profile), and a transcript full of escape codes is unreadable. Progress bars
# also redraw with CR, so those are unwrapped too or a whole install collapses
# onto one unreadable line. Raw output is kept verbatim under out/.
strip_ansi() { sed -E $'s/\x1B\\[[0-9;]*[A-Za-z]//g' | tr '\r' '\n'; }

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

# Like run_step, but a non-zero exit is EXPECTED and is not a blocker.
#
# `self doctor` exits non-zero while any finding remains, and on a real home
# plenty remain that the repair ladder is not responsible for -- active-group
# incoherence between two providers that both claim `cc` accounts for ~190 on
# its own. Filing those as blockers would inflate the count the report exists
# to communicate. The exit code is still recorded, in the description.
run_step_info() {
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
  printf '%s\t%s\t%s\t%s\n' "$id" 0 "$ms" "$desc (exit $rc, informational)" \
    >> "$STEPS"
  log "    => rc=$rc (${ms}ms)   [informational]"
  return 0
}

phase_done() { [[ -f "$CKPT/$1" && -z "${SIM_FORCE:-}" ]]; }
mark_done()  { : > "$CKPT/$1"; }

# ---------------------------------------------------------------- bootstrap
phase_bootstrap() {
  head_ "bootstrap: xlings $OLD_VERSION into $XHOME"
  local asset="xlings-$OLD_VERSION-$SIM_OS-$SIM_ARCH.$SIM_ASSET_EXT"
  mkdir -p "$STAGE" "$USER_HOME"

  if [[ ! -f "$STAGE/$asset" ]]; then
    run_step bootstrap-download "fetch $asset" -- \
      gh release download "v$OLD_VERSION" --repo openxlings/xlings \
        --pattern "$asset" --dir "$STAGE"
  fi
  [[ -f "$STAGE/$asset" ]] || { log "FATAL: no $asset"; return 1; }

  rm -rf "$STAGE/pkg" "$STAGE/raw"; mkdir -p "$STAGE/pkg"
  local pkgdir="$STAGE/pkg"
  if [[ "$SIM_OS" == windows ]]; then
    # unzip has no --strip-components. Use the extracted top-level directory
    # in place rather than moving its contents: `mv "$top"/*` silently skips
    # dotfiles, and the release package's marker is `.xlings.json` -- without
    # it `self install` reports "cannot detect source package directory".
    run_step bootstrap-extract "extract release package" -- \
      unzip -q "$STAGE/$asset" -d "$STAGE/raw"
    pkgdir="$(find "$STAGE/raw" -mindepth 1 -maxdepth 1 -type d | head -1)"
    [[ -n "$pkgdir" ]] || { log "FATAL: no top-level dir in $asset"; return 1; }
  else
    run_step bootstrap-extract "extract release package" -- \
      tar -xzf "$STAGE/$asset" -C "$STAGE/pkg" --strip-components=1
  fi
  [[ -f "$pkgdir/.xlings.json" ]] \
    || log "WARNING: $pkgdir has no .xlings.json; self install will not find the package"

  # `self install` derives the target home from HOME. XLINGS_HOME must stay
  # unset here: with it set, self install has historically written to one
  # place and recorded another.
  run_step bootstrap-self-install "self install (old client)" -- \
    run_isolated "$pkgdir/bin/xlings$SIM_EXE" self install

  # Stop here rather than let every later phase report rc=127. A broken
  # bootstrap produced 19 "blockers" on the macOS and Windows runners, all of
  # them the harness failing to start -- which is exactly the kind of noise
  # that makes a findings table worth ignoring.
  if [[ ! -x "$XHOME/bin/xlings$SIM_EXE" ]]; then
    log "FATAL: bootstrap did not produce $XHOME/bin/xlings$SIM_EXE"
    log "       the remaining phases would only report the harness failing"
    return 1
  fi
  log "    installed version: $(recorded_version)"
  mark_done bootstrap
}

# ---------------------------------------------------------------- populate
#
# Two versions of at least one package on every platform: the single-version
# case cannot exercise `use` and hides every group/binding bug. llvm is the
# one package published for all three, so it carries that role everywhere.
#
# gcc is linux-only in the index (the windows table has one version that
# delegates to mingw, and there is no macosx table at all), so asking for it
# elsewhere would record an index gap as an upgrade blocker.
case "$SIM_OS" in
  linux)
    POPULATE=(gcc@15.1.0 gcc@16.1.0
              llvm@20.1.7 llvm@22.1.8
              node@20.19.0 node@22.17.1
              cmake ninja xmake jq ripgrep fd bat mcpp) ;;
  macosx|windows)
    POPULATE=(llvm@20.1.7 llvm@22.1.8
              node@20.19.0 node@22.17.1
              cmake ninja xmake jq ripgrep fd bat mcpp) ;;
esac
# Override for a quick run: SIM_PACKAGES="mcpp llvm@20.1.7"
[[ -n "${SIM_PACKAGES:-}" ]] && read -r -a POPULATE <<< "$SIM_PACKAGES"

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
binary_fingerprint() {
  local b="$XHOME/bin/xlings$SIM_EXE"
  [[ -f "$b" ]] || { printf 'absent'; return; }
  # sha256sum on Linux, shasum on macOS; either is fine, we only compare.
  { sha256sum "$b" 2>/dev/null || shasum -a 256 "$b" 2>/dev/null; } \
    | awk '{print $1}'
}

phase_update() {
  head_ "update: xlings self update"
  local binBefore binAfter recBefore recAfter
  binBefore="$(binary_fingerprint)"
  recBefore="$(recorded_version)"
  run_step self-update "self update to latest" -- xl self update
  binAfter="$(binary_fingerprint)"
  recAfter="$(recorded_version)"
  log "    binary:   ${binBefore:0:12} -> ${binAfter:0:12}"
  log "    recorded: $recBefore -> $recAfter"

  # What `self update` is responsible for is REPLACING THE BINARY. Its exit
  # code is not evidence of that -- a 0.4.69 client answered 404 from a CN
  # mirror and still reported success (#424) -- so the binary itself is
  # fingerprinted before and after.
  [[ "$binBefore" != "$binAfter" && "$binAfter" != "absent" ]]
  assert_step self-update-replaced-binary \
    "self update replaced the client binary" $?

  # The recorded version staying put is DESIGNED, not a defect, and asserting
  # otherwise would file a blocker against correct behaviour -- a findings
  # table with a known-false row in it is a table people learn to skip.
  #
  # `self update` runs the OLD binary. It cannot know whether the new one will
  # find anything to migrate, so it does not claim the home is reconciled.
  # `self doctor --fix` stamps the field when the migration actually happens,
  # and the mismatch in between is what the new client reads to know it should
  # say so. See .agents/docs/2026-07-28-self-repair-design.md §4.
  [[ "$recBefore" == "$recAfter" ]]
  assert_step self-update-leaves-marker \
    "recorded version deliberately still $recAfter (only --fix stamps it)" $?
  mark_done update
}

# assert_step <id> <description> <condition-rc>
#
# A recorded finding that is not the exit code of a command.
#
# The first run of this harness trusted rc and reported 23/26 green. Three of
# those greens were `xlings remove llvm` (rc=0, 166ms), `install llvm` after
# it (rc=0, 333ms) and a reinstall (rc=0, 277ms) -- durations that cannot
# contain the work they claim. rc=0 from this CLI means "nothing raised", not
# "the thing happened", so every state change gets asserted separately.
assert_step() {
  local id="$1" desc="$2" ok="$3"
  printf '%s\t%s\t%s\t%s\n' "$id" "$ok" 0 "$desc" >> "$STEPS"
  if [[ "$ok" -eq 0 ]]; then log "    [assert] $id: ok"
  else log "    [assert] $id: FAILED -- $desc   <<< BLOCKER"; fi
}

# Is <pkg>[@<version>] listed as installed right now?
listed() {
  local needle="$1"
  xl list 2>/dev/null | strip_ansi | grep -qF "$needle"
}

# ---------------------------------------------------------------- heal
#
# `xlings self doctor --fix` — the migration pass. Runs in the NEW client,
# because `self update` is the old binary replacing itself and cannot do this.
#
# SIM_CANDIDATE_BIN lets a PR test its own build before the change is
# published: the phase runs that binary instead of whatever `self update`
# fetched. Without it, the released client is what gets measured.
heal_bin() {
  if [[ -n "${SIM_CANDIDATE_BIN:-}" ]]; then printf '%s' "$SIM_CANDIDATE_BIN"
  else printf '%s' "$XHOME/bin/xlings$SIM_EXE"; fi
}

# How many payload findings doctor reports right now. Read from the summary
# line rather than counted from the findings, because the summary is what the
# exit code is computed from.
broken_count() {
  run_isolated "$(heal_bin)" self doctor 2>&1 \
    | strip_ansi | grep -aoE 'broken payloads +[0-9]+' \
    | grep -oE '[0-9]+' | head -1
}

phase_heal() {
  head_ "heal: self doctor --fix migrates what the old client registered"
  log "    client under test: $(heal_bin)"

  local before after1 after2 stamped
  before="$(broken_count)"; before="${before:-0}"
  log "    payload findings before: $before"

  run_step_info heal-fix-1 "self doctor --fix (first pass)" -- \
    run_isolated "$(heal_bin)" self doctor --fix
  after1="$(broken_count)"; after1="${after1:-0}"
  log "    payload findings after pass 1: $after1"

  # The migration marker. `self update` never wrote it, which is what made a
  # "you are behind" hint impossible to build; --fix stamping it is what makes
  # the hint stop once the migration has actually happened.
  stamped="$(recorded_version)"
  log "    recorded client version: $stamped"
  [[ "$stamped" != "v0.4.69" && "$stamped" != "?" ]]
  assert_step heal-stamped \
    "--fix stamped the home with the running client (was v0.4.69, now $stamped)" $?

  # Not "went down": the summary counts findings the ladder does not own
  # (active-group incoherence, ~190 on a real home), so on an already-migrated
  # home the honest requirement is that --fix never makes things worse. The
  # quality signal is heal-no-failures plus heal-idempotent below.
  [[ "$after1" -le "$before" ]]
  assert_step heal-not-worse \
    "--fix did not increase findings ($before -> $after1)" $?

  # Idempotence: a second pass must not find work. A ladder that repairs the
  # same thing every run is not converging, it is looping.
  run_step_info heal-fix-2 "self doctor --fix (second pass)" -- \
    run_isolated "$(heal_bin)" self doctor --fix
  after2="$(broken_count)"; after2="${after2:-0}"
  log "    payload findings after pass 2: $after2"

  [[ "$after2" -le "$after1" ]]
  assert_step heal-idempotent \
    "second --fix did not increase findings ($after1 -> $after2)" $?

  grep -aq "repair failed" "$SIM_ROOT/out/heal-fix-2.log" 2>/dev/null
  [[ $? -ne 0 ]]
  assert_step heal-no-failures "second --fix reported no repair failures" $?

  mark_done heal
}

# ---------------------------------------------------------------- exercise
# What a user does the day after upgrading. Each is independent: a failure in
# one must not stop the next, or the first blocker hides all the others.
phase_exercise() {
  head_ "exercise: drive the pre-upgrade packages with the NEW client"

  run_step ex-list        "list installed"  -- xl list
  run_step ex-list-all    "list --all"      -- xl list --all
  # `doctor` is a subcommand of `self`, not a top-level command.
  run_step_info ex-doctor "self doctor"     -- xl self doctor

  # Switching between two installed versions, both directions. The switch is
  # confirmed through the shim's own --version output, not through `use`'s
  # exit code: `use` reporting success while the shim still resolves the old
  # payload is precisely the failure worth catching.
  if [[ "$SIM_OS" == linux ]]; then
    run_step ex-use-gcc-15 "use gcc 15.1.0" -- xl use gcc 15.1.0
    run_step ex-gcc-v-15   "gcc --version through the shim" -- shim gcc --version
    grep -q "15\.1\.0" "$SIM_ROOT/out/ex-gcc-v-15.log" 2>/dev/null
    assert_step ex-gcc-is-15 "shim reports 15.1.0 after use gcc 15.1.0" $?

    run_step ex-use-gcc-16 "use gcc 16.1.0" -- xl use gcc 16.1.0
    run_step ex-gcc-v-16   "gcc --version through the shim" -- shim gcc --version
    grep -q "16\.1\.0" "$SIM_ROOT/out/ex-gcc-v-16.log" 2>/dev/null
    assert_step ex-gcc-is-16 "shim reports 16.1.0 after use gcc 16.1.0" $?
  fi

  run_step ex-use-llvm-20 "use llvm 20.1.7" -- xl use llvm 20.1.7
  run_step ex-use-llvm-22 "use llvm 22.1.8" -- xl use llvm 22.1.8

  # Reinstall of something the OLD client registered. Whether this re-runs the
  # config hook or short-circuits on "already installed" is itself the finding.
  run_step ex-reinstall-llvm "new client reinstalls llvm" -- xl install llvm -y
  run_step ex-reinstall-mcpp "new client reinstalls mcpp" -- xl install mcpp -y
  [[ "$SIM_OS" == linux ]] && \
    run_step ex-reinstall-gcc "new client reinstalls gcc" -- xl install gcc -y

  # Removal of something the OLD client registered, then putting it back.
  # `remove <bare-name>` with two versions installed has to pick one; which
  # one, and whether the other survives, is asserted rather than assumed.
  run_step ex-remove-llvm "new client removes llvm" -- xl remove llvm -y
  ! listed "llvm@22.1.8"
  assert_step ex-llvm-22-gone "llvm@22.1.8 no longer listed after remove llvm" $?
  listed "llvm@20.1.7"
  assert_step ex-llvm-20-kept "llvm@20.1.7 still listed (remove took one version)" $?

  run_step ex-install-back-llvm "install llvm again after removal" -- xl install llvm -y
  listed "llvm@22.1.8"
  assert_step ex-llvm-22-back "llvm@22.1.8 listed again after install llvm" $?

  if [[ "$SIM_OS" == linux ]]; then
    run_step ex-remove-gcc15 "new client removes gcc@15.1.0" -- xl remove gcc@15.1.0 -y
    ! listed "gcc@15.1.0"
    assert_step ex-gcc-15-gone "gcc@15.1.0 no longer listed after remove" $?
    listed "gcc@16.1.0"
    assert_step ex-gcc-16-kept "gcc@16.1.0 survives removal of the other version" $?
  fi

  run_step_info ex-doctor-after "self doctor after the churn" -- xl self doctor
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

  # A blocker is this harness's OUTPUT, not its failure: the job stays green
  # and publishes the table, because a run that is red every time gets muted
  # and then nobody reads the findings it exists to produce.
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    {
      printf '## upgrade simulation — %s/%s\n\n' "$SIM_OS" "$SIM_ARCH"
      printf '%s -> latest · %s steps · **%s blockers**\n\n' \
        "$OLD_VERSION" "$total" "$fails"
      printf '| step | rc | ms | what |\n|---|---:|---:|---|\n'
      awk -F'\t' '{printf "| `%s` | %s | %s | %s |\n", $1, $2, $3, $4}' "$STEPS"
    } >> "$GITHUB_STEP_SUMMARY"
  fi
}

main() {
  local phases=("$@")
  [[ ${#phases[@]} -eq 0 ]] && \
    phases=(bootstrap populate snapshot update heal exercise report)
  log "### simulation start  $SIM_OS/$SIM_ARCH  root=$SIM_ROOT"
  for p in "${phases[@]}"; do
    if [[ "$p" != report ]] && phase_done "$p"; then
      log ""
      log "=============== $p: already done, skipping (SIM_FORCE=1 to redo)"
      continue
    fi
    # A phase that reports itself unusable (bootstrap with no client binary)
    # short-circuits to the report. Everything after it would measure the
    # harness, not the upgrade.
    if ! "phase_$p" && [[ "$p" != report ]]; then
      log ""
      log "=============== $p failed; skipping to report"
      phase_report
      return 1
    fi
  done
}

main "$@"
