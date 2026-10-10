#!/usr/bin/env bash
# The build under test is this user's: xlings and luban, one version.
source "$(dirname "$0")/../lib.sh"
step "xlings runs" ok -- X --version
v="$(grep -Eo '[0-9]{4}\.[0-9]+\.[0-9]+\.[0-9]+' <<<"$LAST_OUT" | head -1)"
step "luban is the same version ($v)" ok "$v" -- L --version
step "luban says where it is" ok -- L
step "an index answers" ok -- X search luban
done_
