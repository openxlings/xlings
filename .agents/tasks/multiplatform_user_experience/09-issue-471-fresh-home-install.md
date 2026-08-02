# Task 09: Issue #471 Fresh `XLINGS_HOME` Install

## Finding

Confirmed against the pre-fix binary on 2026-08-03. A first package install
with an explicit, unbootstrapped `XLINGS_HOME` completed payload work and then
exited 1 while writing `subos/default/.xlings.json`. The parent directory did
not exist, so active/installed ownership was not persisted.

## Goal

Make an explicit cold `XLINGS_HOME` a valid package-install target without a
separate `xlings self init` step.

## Done When

- `Config::save_workspace()` creates the selected state file's parent before
  writing it, for global and project SubOS paths.
- a deterministic local-recipe E2E starts with no `subos/`, installs once,
  exits 0, and verifies `subos/default/.xlings.json` contains the exact
  installed and active version.
- the regression runs in Linux, macOS, Windows, and candidate cold-home gates.

Reference: https://github.com/openxlings/xlings/issues/471
