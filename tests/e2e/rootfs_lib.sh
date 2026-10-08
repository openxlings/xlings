# Shared by the root-projection scenarios (SubOS design part 2): a home whose
# entry is the static xlings under test, a Luban root made from the published
# editions, and the checks every scenario makes on what a root contains.
#
# Sourced, not run. Expects project_test_lib.sh first.

# A home of its own whose entry is $BIN (the release build: static, so an
# exported image's xlings runs with no host under it).
rootfs_home() {
    local dir="$1"
    # The caller creates a new private test home; never empty an existing home.
    [[ ! -e "$dir" ]] || fail "test home already exists: $dir"
    mkdir -p "$dir"
    mkdir -p "$dir/bin"
    cp "$BIN" "$dir/bin/xlings"
    # XLINGS_TEST_INDEX: an index to use instead of the published one (a
    # local checkout while its PR is open).
    if [[ -n "${XLINGS_TEST_INDEX:-}" ]]; then
        printf '{"mirror":"%s","index_repos":[{"name":"xim","url":"%s"}]}\n' \
            "${XLINGS_TEST_MIRROR:-GLOBAL}" "$XLINGS_TEST_INDEX" > "$dir/.xlings.json"
    else
        printf '{"mirror":"%s"}\n' "${XLINGS_TEST_MIRROR:-GLOBAL}" > "$dir/.xlings.json"
    fi
    RH="$dir"
    X self init >/dev/null 2>&1 || fail "self init of $dir failed"
}

# xlings against $RH, from a neutral directory, with nothing inherited that
# names another home or SubOS.
X() {
    ( cd /tmp && env -u XLINGS_ACTIVE_SUBOS -u XLINGS_PROJECT_DIR -u XLINGS_SUBOS_MODE \
        -u XLINGS_BROKER_SOCKET -u XLINGS_SESSION_FD XLINGS_HOME="$RH" \
        "$RH/bin/xlings" "$@" )
}

# What a root's /usr holds, as one line per link -- with the SubOS's own name
# taken out (an exported instance is its image's `default`). Every
# presentation of the same declaration must print the same lines.
#   $1: a command prefix that runs `sh -c` inside the root
tree_lines_script='cd /usr && for f in bin/* lib/*; do printf "%s %s\n" "$f" "$(readlink "$f")"; done | sed -E "s|/subos/[^/]+/|/subos/*/|g"'

# Retry what talks to the network: a mirror's bad minute is not the code's.
retry() {
    local n=0
    until "$@"; do
        n=$((n + 1))
        [[ $n -ge 3 ]] && return 1
        sleep 5
    done
}

# `subos new <name> --rootfs --from <edition>`, retried from scratch: a
# mirror's bad minute leaves a half-made root, which the next try removes.
rootfs_new() {
    local name="$1" from="$2" log="$3" n=0
    until X subos new "$name" --rootfs --from "$from" >"$log" 2>&1; do
        n=$((n + 1))
        [[ $n -ge 3 ]] && { tail -30 "$log"; return 1; }
        X subos remove -y "$name" >/dev/null 2>&1 || true
        sleep 5
    done
}
