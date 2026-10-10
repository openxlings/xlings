#!/usr/bin/env bash
# net=proxy: names go to the proxy and are resolved there; the host's
# loopback and its network are out of reach -- with the restriction left on.
source "$(dirname "$0")/../lib.sh"
command -v python3 >/dev/null || skip "no python3 for the proxy fixture"
command -v curl >/dev/null || skip "no curl"
require_isolation
python3 "$(dirname "$0")/../socks5.py" "$HM_STATE/proxy.port" "$HM_STATE/proxy.log" &
fixture=$!; trap 'kill $fixture 2>/dev/null' EXIT
for _ in $(seq 50); do [[ -s "$HM_STATE/proxy.port" ]] && break; sleep 0.1; done
port="$(cat "$HM_STATE/proxy.port")"
X subos new hmx >/dev/null 2>&1
step "config: dev, through the proxy" ok -- X subos config hmx --sandbox dev --proxy "socks5h://127.0.0.1:$port"
step "a name goes to the proxy" ok 'through-declared-proxy' -- \
    X subos exec hmx -- curl --max-time 10 -sS http://must-not-resolve.invalid/payload
step "the proxy resolved it, not the host" ok 'must-not-resolve.invalid' -- cat "$HM_STATE/proxy.log"
step "loopback only inside" ok '^lo,$' -- X subos exec hmx -- \
    /bin/sh -c 'tail -n +3 /proc/net/dev | cut -d: -f1 | tr -d " " | tr "\n" ,; echo'
step "the host's loopback is out of reach" nonzero -- X subos exec hmx -- \
    curl --noproxy '*' --connect-timeout 1 --max-time 2 -sS "http://127.0.0.1:$port/"
done_
