#!/usr/bin/env bash
# Wait for an artifact another job of this run uploads, and fetch it -- for a
# job that builds what it needs alongside that job and wants the artifact only
# to run (xlings-ci-linux.yml's acceptance takes the release build this way).
# Fails as soon as that job ends without it.
#
#   tools/ci/await_artifact.sh <artifact> <job name> <directory> [seconds]
#
# Needs GH_TOKEN (actions: read).
set -euo pipefail
name="$1" job="$2" dest="$3" limit="${4:-3600}"
start=$SECONDS
fetch() { gh run download "$GITHUB_RUN_ID" -R "$GITHUB_REPOSITORY" -n "$name" -D "$dest" >/dev/null 2>&1; }
while (( SECONDS - start < limit )); do
  fetch && { echo "$name: fetched after $(( SECONDS - start ))s"; exit 0; }
  state="$(gh api "repos/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/attempts/${GITHUB_RUN_ATTEMPT:-1}/jobs" \
    --jq ".jobs[] | select(.name == \"$job\") | .status + \" \" + (.conclusion // \"\")" 2>/dev/null || true)"
  if [[ "$state" == completed* ]]; then
    fetch && exit 0
    echo "::error::$job ended (${state#completed }) without $name"
    exit 1
  fi
  sleep 15
done
echo "::error::no $name from $job within ${limit}s"
exit 1
