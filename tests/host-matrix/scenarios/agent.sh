#!/usr/bin/env bash
# agent-private on this host: a persona of its own (stable, not the host's),
# the proxy as the only network, no nested user namespaces.
source "$(dirname "$0")/../lib.sh"
command -v python3 >/dev/null || skip "no python3 for the proxy fixture"
X info xim:agent-private 2>/dev/null | grep -q '2026\.' || skip "the index has no date-versioned agent-private yet"
require_isolation
python3 "$(dirname "$0")/../socks5.py" "$HM_STATE/aproxy.port" "$HM_STATE/aproxy.log" &
fixture=$!; trap 'kill $fixture 2>/dev/null' EXIT
for _ in $(seq 50); do [[ -s "$HM_STATE/aproxy.port" ]] && break; sleep 0.1; done
port="$(cat "$HM_STATE/aproxy.port")"
step "luban new hma tiny" ok -- L new hma tiny
step "policy agent-private" ok -- L config hma policy xim:agent-private
step "its proxy" ok -- L config hma proxy "socks5h://127.0.0.1:$port"
probe='echo HOST=$(hostname); echo MID=$(cat /etc/machine-id); tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d " " | tr "\n" ,; echo'
step "enters" ok 'HOST=' -- L run hma -- /bin/sh -c "$probe"
first="$LAST_OUT"
step "the same persona next time" ok -- L run hma -- /bin/sh -c "$probe"
persona() { grep -E '^(HOST|MID)=' <<<"$1"; }
check "the persona is stable" -- test "$(persona "$first")" = "$(persona "$LAST_OUT")"
check "the host name is not the host's" -- test "$(grep '^HOST=' <<<"$first")" != "HOST=$(hostname)"
check "loopback only" -- grep -q '^lo,$' <<<"$first"
step "no nested user namespaces" ok 'nested=no' -- L run hma -- /bin/sh -c 'if unshare -U true 2>/dev/null; then echo nested=yes; else echo nested=no; fi'
done_
