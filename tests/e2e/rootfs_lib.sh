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

# Keep the published recipe's install/config hooks. Only the candidate resource
# and its latest ref differ; the binary still goes through download + extraction.
rootfs_update_fixture() {
    local dir="$1" home="$2" archive="${ROOTFS_UPDATE_ARCHIVE:-$BUILD_DIR/release.tar.gz}"
    local recipe="$home/data/xim-pkgindex/pkgs/x/xlings.lua" package_name digest
    [[ -f "$recipe" ]] || fail "the root's official xlings recipe is missing: $recipe"
    [[ -f "$archive" ]] || fail "self-update needs the candidate release archive: $archive"
    command -v python3 >/dev/null || fail "self-update fixture requires python3 on the host"
    ROOTFS_CANDIDATE_VERSION="$("$BIN" --version | awk 'NR == 1 { print $2 }')"
    [[ "$ROOTFS_CANDIDATE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] \
        || fail "cannot identify the candidate release version"
    package_name="xlings-$ROOTFS_CANDIDATE_VERSION-linux-$(uname -m)"
    digest="$(tar -xzOf "$archive" "$package_name/bin/xlings" | sha256sum | awk '{ print $1 }')" \
        || fail "candidate archive does not contain $package_name/bin/xlings"
    [[ "$digest" == "$(sha256sum "$BIN" | awk '{ print $1 }')" ]] \
        || fail "candidate archive and XLINGS_BIN differ"
    mkdir -p "$dir/index/pkgs/x"
    cp "$recipe" "$dir/index/pkgs/x/xlings.lua"
    cp "$archive" "$dir/$package_name.tar.gz"
    python3 - "$dir" >"$dir/http.log" 2>&1 <<'PY' &
import functools
import http.server
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(root))
with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
    (root / "port").write_text(str(server.server_port))
    server.serve_forever()
PY
    ROOTFS_HTTP_PID=$!
    local attempt
    for attempt in $(seq 1 100); do
        [[ -s "$dir/port" ]] && break
        kill -0 "$ROOTFS_HTTP_PID" 2>/dev/null || fail "candidate HTTP server exited"
        sleep 0.05
    done
    [[ -s "$dir/port" ]] || fail "candidate HTTP server did not become ready"
    digest="$(sha256sum "$dir/$package_name.tar.gz" | awk '{ print $1 }')"
    ROOTFS_OLD_VERSION="$(python3 - "$dir/index/pkgs/x/xlings.lua" \
        "$ROOTFS_CANDIDATE_VERSION" "$(cat "$dir/port")" "$package_name" "$digest" <<'PY'
import pathlib
import re
import sys

recipe = pathlib.Path(sys.argv[1])
candidate, port, package_name, digest = sys.argv[2:]
text = recipe.read_text()
match = re.search(r'\["latest"\]\s*=\s*\{\s*ref\s*=\s*"([^"]+)"', text)
if not match or match[1] == candidate:
    raise SystemExit("fixture needs a distinct published predecessor")
recipe.write_text(text + '\npackage.xpm.linux["' + candidate + '"] = {\n'
                  + '    url = "http://127.0.0.1:' + port + '/' + package_name + '.tar.gz",\n'
                  + '    sha256 = "' + digest + '",\n'
                  + '}\npackage.xpm.linux["latest"] = { ref = "' + candidate + '" }\n')
print(match[1])
PY
    )" || fail "cannot prepare the official self-update recipe"
}
