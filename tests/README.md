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

## A window (the Luban OS app)

A GUI is tested in two halves.

**Automated, headless** (`tests/e2e/test_luban_os.cpp`). The app renders for
real under SDL's offscreen driver, and `luban-os --script <file>` drives it
between frames with input injected as SDL delivers a person's:

- `key Down`, `key Ctrl+Alt` (held in order, released in reverse), `type text`
- `click <anchor>`: a named control, clicked once it holds still
- `resize W H`, `close`
- `wait <view>`, `waitfor <path> <value>`, `expect <path> <value>`: the
  state, where every named control was drawn included
- `see [<n>s] <text>`: until the text is shown (a console's lines, a job, a
  notice)
- `commit <text>` (as an input method commits it), `clipboard <text>` (the
  host's clipboard), `drop <file>` (dropped on the window)
- `screenshot <file>` (`.ppm` to read, `.bmp` to look at)

`--desktop-exec <program>` shows a program speaking opensal.wire on its
stdio as a desktop, so a desktop's drawing and input are tested without a
machine; xdesk runs that way with a small Wayland client (Python, the plain
wire) that fills its window with one colour and reports the text and the
clipboard it was given. The tests run under SDL's software renderer, the one
a machine without a GPU has. They check:

- what can be decided: every view reached by keys and pointer;
- the highlight following the choice;
- every control on screen, apart from the others and big enough to hit, at
  three sizes in English and Chinese;
- a desktop's pixels, its input in its own coordinates, and its size;
- what moves changing between frames;
- **a first-time user, for real** (`linux-luban-os-first-use`, the release
  client, network): the quick start makes a system, its console opens and runs
  what is typed; an exported system is imported and has what it carried. On
  macOS and Windows the window's own tests run, and the quick start says what
  that computer can do before anything is tried.

**By eye, key frames.** Proportion, hierarchy, density and finish are judged
by a person. The tests keep every key frame in
`target/xtest-artifacts/luban-os/` (CI: the `luban-os-screenshots` artifact),
and `python3 tools/luban_os_keyframes.py` puts them on one page. Look at it
before a change to the window is merged.

## Luban, as a person meets it (fresh install)

`tests/fresh-install/luban.sh` holds Luban's suites, run by
`.github/workflows/xlings-ci-luban-fresh.yml` against the RELEASED client after
every release, daily, and on dispatch (`index_ref` to try an unmerged index):

| suite | hosts | what a person does |
|---|---|---|
| `luban-console` | ubuntu-24.04, -arm, 22.04 | nano, tiny and core made; a session typed into, `--temp`, `--persist home`; C++23 with core's gcc; a package in and rolled back; upgrade's plan; `rm` |
| `luban-window` | ubuntu-24.04 | the window installed; the quick start; the desktop editions (sandbox, KVM machine) |
| `luban-archive` | ubuntu-24.04, -arm | `.luban` out and in, a directory root that runs alone, a tar |
| `luban-machine` | ubuntu-24.04 (KVM) | the environment as a machine (following the hint for what it needs), an exported image booted |
| `luban-upgrade` | ubuntu-24.04 | an environment made by the release before, used after `xlings self update` |
| `luban-host` | macOS, Windows | what a computer that cannot run Luban systems says |

Run one locally against a build of your own (a static xlings, luban and
luban-init in a throwaway home's `bin/`): source `lib.sh` and `luban.sh` and
call the suite. They check what is said and what happens, not internals.

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

## Parallel runs

Building the test binaries costs more than running them, so the tests are
split in four ways that combine:

| | what | how |
|---|---|---|
| family | the binaries one `mcpp test --no-run <prefix>` builds together, in parallel, on the library built once (`unit/test_subos_*`) | always, when xdev builds |
| jobs | tests running at once on one machine; a test's `.resources` keeps conflicting ones apart | `-j N` |
| shard | one machine's share; it takes whole families, so it builds only what it runs | `--shard i/n` (`ci plan --shards n` prints the plan) |
| reuse | one build feeding many machines: the static release build is the artifact every system lane runs | the release job, `--no-build` |

`tests/lanes.toml` holds CI's choice per lane (shards, jobs, the build's
profile); `xdev test --lane-config unit --shard 2/4` runs one machine's share
of it, and the workflow's matrix comes from the same file
(`tools/ci/lane_matrix.py`). Changing how CI spends machines is an edit there.

```bash
$XDEV test -j 8                           # this machine, 8 at a time
$XDEV test --lane-config unit --shard 1/4 # what CI's first unit machine runs
```

The Linux round (`xlings-ci-linux.yml`): `plan` (lints, the matrices) starts
`release` (the one release build), the `unit` shards, `acceptance` (checks,
desktop payloads, then the static gates on the release build as soon as it is
attached) and, on main, the `asan` shards; the system lanes start from the
release build, and `report` reads every lane.

## Local runs

- Run from a clean environment: a shell inside a subos exports
  `XLINGS_ACTIVE_SUBOS`, which some older unit tests still inherit.
- Inside China set `XLINGS_TEST_MIRROR=CN` (AGENTS.md: an unreachable mirror
  looks like the command under test hanging).
- Network is opt-in: `XDEV_NETWORK=1`.


`xdev report --requirements tests/requirements.toml --fail-uncovered` 检查需求的测试声明。
报告另外列出执行证据，跳过的测试不能证明需求已验证。发布验收使用 `--fail-unverified`
检查合并后的各车道记录；隔离需求还要求 `proves=isolation`。JSON 报告中的
`requirements.<id>.declared_by` 与 `passed_by` 分别给出两种证据。
`xdev test --out` 必须选择空目录，避免覆盖已有现场或用户文件。
