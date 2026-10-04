# xlings tests

Design: `.agents/docs/2026-10-05-subos-architecture-design.md` §24.

## Layout

| Path | What | Runs with |
|---|---|---|
| `tests/unit/test_*.cpp` | gtest unit tests (and testkit's own tests) | `mcpp test` |
| `tests/e2e/test_*.cpp` | end-to-end tests in C++: drive the built binary through `modules/testkit` | `mcpp test` |
| `tests/e2e/*_test.sh`, `*.ps1` | legacy end-to-end scripts | `tests/e2e/run_all.sh`, or `xdev test --suite e2e-shell` |
| `tests/scripts/` | contract scripts (Python / shell) | `xdev test --suite contract-scripts` |
| `tests/suites.toml` | the legacy suites xdev knows | `xdev test --suite <name>` |
| `tests/fresh-install/`, `tests/candidate-install/` | install-path suites | their workflows |

New tests are written in C++. A script leaves `tests/suites.toml` when it is
ported.

## Writing an end-to-end test

```cpp
#include <gtest/gtest.h>
import xlings.testkit;          // before the header (mcpp scans imports in the source)
#include "xlings/xtest.hpp"
import std;

namespace tk = xlings::testkit;

XTEST(SubosExec, ExitCodeIsTheCommands,
      .area = "subos", .covers = {"EXIT-CMD"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    auto home = tk::Home::isolated("exec");      // temp dir, never under $HOME
    auto r = home.xlings({"subos", "new", "box"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
}
```

- `Home::isolated()` gives a fresh home; every run starts from an environment
  the test names (`Home::env()` plus what it adds) — nothing is inherited.
- `requires_` names capabilities (`xdev doctor` lists them). Missing on a
  developer machine: the test is skipped and says why. Missing on a CI lane
  that declared it in `XDEV_LANE_CAPS`: the test fails.
- `covers` names requirement IDs; `proves = "isolation"` marks a test that
  runs a real sandbox (a fake provider only proves the flow).
- A failed test keeps its home and copies its config/logs/state to
  `target/xtest-artifacts/<Suite.Name>/`.

## xdev

```bash
mcpp build && mcpp build -p xdev
XDEV=$(find target -path '*/bin/xdev/xdev' | head -1)

$XDEV doctor                              # what this machine can test
$XDEV test                                # all C++ tests, one report
$XDEV test test_testkit                   # a pattern, passed to mcpp test
$XDEV test --no-mcpp --suite contract-scripts
$XDEV report                              # re-render target/xdev/run
```

The report (terminal, `target/xdev/run/report.{md,json}`, and the GitHub step
summary in CI) lists pass/fail/skip for test binaries, XTEST cases and legacy
scripts, failures with their output, skips grouped by reason, the slowest
tests and the lane's capabilities.

## Local runs

- Run from a clean environment: a shell inside a subos exports
  `XLINGS_ACTIVE_SUBOS`, which some older unit tests still inherit.
- Inside China set `XLINGS_TEST_MIRROR=CN` (AGENTS.md: an unreachable mirror
  looks like the command under test hanging).
- Network is opt-in: `XDEV_NETWORK=1`.
