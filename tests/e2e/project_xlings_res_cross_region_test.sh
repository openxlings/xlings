#!/usr/bin/env bash
# A region whose own resource servers are all unusable must still install.
#
# The regional buckets in `XLINGS_RES` are a routing *preference* -- every
# xlings-res host mirrors the same releases -- but the fallback list was built
# from the current region's bucket alone. With the shipped defaults that
# bucket holds exactly one host per region, so for a CN client the fallback
# list was always empty and a single missing asset was a flat HTTP 404 with a
# working copy one hop away on GitHub.
#
# That is not hypothetical: `2026.7.30.1` shipped with its four tarballs
# missing from GitCode (the release runner cannot push large assets across the
# border, so that mirror step is manual) and CN users could not install for
# three hours.
#
# The scenario points CN at a dead host and GLOBAL at the real one. Before the
# cross-region fallback this install had nowhere to go.
#
# Refs: .agents/docs/2026-07-30-cli-determinism-and-followup-plan.md §3.1
set -euo pipefail

SCENARIO_NAME="xlings_res_cross_region" \
HOME_NAME="xlings_res_cross_region_home" \
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_xlings_res_test.sh"
