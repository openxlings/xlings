# shellcheck shell=bash
# The host matrix's scenario library (Luban OS design part 2 §5).
#
# A scenario runs as an ORDINARY user -- no password-less sudo -- on a host
# whose facts (its LSM, its bwrap, its kernel) are the thing under test. It
# records each step as one JSON line in $HM_EVIDENCE/<scenario>.ndjson and
# exits:
#   0  pass        1  fail        4  skip (said why)
#   3  needs the one-time setup: it wrote the command it was told to run to
#      $HM_STATE/admin-request; the orchestrator (the machine's administrator)
#      runs it and starts the scenario again with HM_SETUP_DONE=1.
set -uo pipefail

: "${HM_EVIDENCE:?}" "${HM_STATE:?}"
SCENARIO="$(basename "$0" .sh)"
EVIDENCE="$HM_EVIDENCE/$SCENARIO.ndjson"
export XLINGS_HOME="${XLINGS_HOME:-$HOME/.xlings}"
export XLINGS_NON_INTERACTIVE=1
X() { "$XLINGS_HOME/bin/xlings" "$@"; }
L() { "$XLINGS_HOME/bin/luban" "$@"; }

json_str() {   # a JSON string literal of $1
    local s="$1"
    s="${s//\\/\\\\}"; s="${s//\"/\\\"}"; s="${s//$'\n'/\\n}"; s="${s//$'\r'/}"; s="${s//$'\t'/\\t}"
    printf '"%s"' "$(printf '%s' "$s" | tr -d '\000-\010\013\014\016-\037')"
}

record() {     # step, ok(true|false), rc, output
    printf '{"scenario":%s,"step":%s,"ok":%s,"rc":%s,"out":%s}\n' "$(json_str "$SCENARIO")" \
        "$(json_str "$1")" "$2" "$3" "$(json_str "$(printf '%s' "$4" | tail -c 2000)")" >> "$EVIDENCE"
    if [[ "$2" == true ]]; then printf '  ok    %s\n' "$1"; else printf '  FAIL  %s (exit %s)\n%s\n' "$1" "$3" "$(printf '%s' "$4" | tail -20 | sed 's/^/        | /')"; fi
}

FAILED=0
# step <description> <expected exit or "ok"|"nonzero"> [regex the output must match] -- cmd...
step() {
    local desc="$1" want="$2" match=""
    shift 2
    if [[ "$1" != "--" ]]; then match="$1"; shift; fi
    shift
    local out rc
    out="$("$@" 2>&1)"; rc=$?
    local good=false
    case "$want" in
        ok) [[ $rc -eq 0 ]] && good=true ;;
        nonzero) [[ $rc -ne 0 ]] && good=true ;;
        *) [[ $rc -eq $want ]] && good=true ;;
    esac
    if [[ "$good" == true && -n "$match" ]] && ! grep -Eq -- "$match" <<<"$out"; then good=false; fi
    record "$desc" "$good" "$rc" "$out"
    [[ "$good" == true ]] || FAILED=1
    LAST_OUT="$out"; LAST_RC=$rc
    [[ "$good" == true ]]
}

# check <description> -- test...: a condition on what earlier steps saw.
check() {
    local desc="$1"; shift 2
    if "$@"; then record "$desc" true 0 ""; else record "$desc" false 1 ""; FAILED=1; fi
}

skip() { record "skip: $1" true 0 ""; printf '  skip  %s\n' "$1"; exit 4; }
done_() { exit "$FAILED"; }

# The one-time setup, the way a person meets it: in agent mode the command
# never waits -- it exits 2 and names what to run. That command goes to the
# administrator; the scenario runs again after it.
needs_setup() {   # the output that named it
    local command
    command="$(grep -Eo '(xlings self doctor --isolation --fix|luban setup)' <<<"$1" | head -1)"
    [[ -n "$command" ]] || return 1
    printf 'isolation-setup %s\n' "$command" > "$HM_STATE/admin-request"
    record "asks for the one-time setup: $command" true 2 "$1"
    exit 3
}

# Facts of this host, once per run (the orchestrator calls it).
host_facts() {
    local f="$HM_EVIDENCE/host.json"
    local restrict userns clone label kernel os
    restrict="$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null || echo absent)"
    userns="$(cat /proc/sys/user/max_user_namespaces 2>/dev/null || echo absent)"
    clone="$(cat /proc/sys/kernel/unprivileged_userns_clone 2>/dev/null || echo absent)"
    label="$(cat /sys/fs/selinux/enforce 2>/dev/null || echo absent)"
    kernel="$(uname -r)"
    os="$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
    printf '{"os":%s,"kernel":%s,"apparmor_restrict_unprivileged_userns":%s,"max_user_namespaces":%s,"unprivileged_userns_clone":%s,"selinux_enforce":%s,"kvm":%s,"glibc":%s}\n' \
        "$(json_str "$os")" "$(json_str "$kernel")" "$(json_str "$restrict")" "$(json_str "$userns")" \
        "$(json_str "$clone")" "$(json_str "$label")" "$([[ -e /dev/kvm ]] && echo true || echo false)" \
        "$(json_str "$(ldd --version 2>&1 | head -1)")" > "$f"
}

# Isolation, as this host has it. Not available and fixable by the one-time
# setup: ask for it (exit 3). Not available otherwise: skip, saying why.
require_isolation() {
    [[ -n "$(ls "$XLINGS_HOME"/data/xpkgs/xim-x-bwrap/*/bin/bwrap 2>/dev/null)" || -x /usr/lib/xlings/bwrap ]] \
        || X install -y bwrap >/dev/null 2>&1
    local out rc
    out="$(X self doctor --isolation 2>&1)"; rc=$?
    if [[ $rc -eq 0 ]]; then record "isolation: the doctor is satisfied" true 0 "$out"; return 0; fi
    if [[ -z "${HM_SETUP_DONE:-}" ]] && grep -q 'self doctor --isolation --fix' <<<"$out"; then needs_setup "$out"; fi
    if [[ -n "${HM_SETUP_DONE:-}" ]]; then record "isolation after the one-time setup" false "$rc" "$out"; exit 1; fi
    record "isolation unavailable here (not something the setup changes)" true "$rc" "$out"
    exit 4
}
