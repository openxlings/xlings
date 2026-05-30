# xlings Tests

This directory contains unit tests and end-to-end usability tests for `xlings`.

## Unit Tests

Script: `tests/unit/test_main.cpp` (51 gtest tests, 11 suites)

| Suite | Tests | Coverage |
|-------|-------|----------|
| I18nTest | 7 | 多语言翻译 |
| LogTest | 4 | 日志级别与文件输出 |
| UtilsTest | 5 | 字符串工具 |
| CmdlineTest | 4 | CLI 解析 |
| UiTest | 2 | UI 组件 |
| XimTypesTest | 4 | PlanNode, InstallPlan 等类型 |
| XimIndexTest | 9 | 索引构建/搜索/版本匹配/加载 |
| XimResolverTest | 4 | 依赖解析/拓扑排序/循环检测 |
| XimDownloaderTest | 3 | 下载任务/归档解压 |
| XimInstallerTest | 4 | 安装编排/错误处理 |
| XimCommandsTest | 5 | 命令执行 (search/list/info) |

### Run

```bash
mcpp build
mcpp test
```

Note: XimIndex/Resolver/Installer/Commands tests require the `xim-pkgindex` repo at a known path. They will `GTEST_SKIP` if not found.

## E2E Linux Usability Test

Script: `tests/e2e/linux_usability_test.sh`

What it validates in an isolated extracted release package:

- basic CLI availability (`xlings -h`, `xlings config`, `xvm --version`)
- `xlings info <target>` mapping to `xvm info`
- `subos` lifecycle (`new`, `use`, `list`, `info`, `remove`)
- `subos new` fails when `config/xvm` is missing (package incomplete)
- new `subos` short aliases (`ls`, `i`, `rm`)
- backward compatibility for existing `subos` commands
- self-maintenance dry-run command
- optional network install scenario (`xlings install d2x`)

### Run

```bash
# auto-detect latest build/xlings-*-linux-x86_64.tar.gz
bash tests/e2e/linux_usability_test.sh

# specify archive explicitly
bash tests/e2e/linux_usability_test.sh build/xlings-0.4.0-linux-x86_64.tar.gz
```

### Network Scenario

By default network-dependent installation checks are enabled.

```bash
SKIP_NETWORK_TESTS=1 bash tests/e2e/linux_usability_test.sh
```

Same toggle works for macOS and Windows scripts.

## E2E Project Scenarios

Scenario configs:

- `tests/e2e/scenarios/local_repo/.xlings.json`
- `tests/e2e/scenarios/global_fallback/.xlings.json`
- `tests/e2e/scenarios/xlings_res_cn/.xlings.json`
- `tests/e2e/scenarios/xlings_res_project_override/.xlings.json`
- `tests/e2e/scenarios/xlings_res_region_map/.xlings.json`
- `tests/e2e/scenarios/xlings_res_multi_server/.xlings.json`

Scenario scripts:

- `tests/e2e/project_local_repo_test.sh`
- `tests/e2e/project_global_fallback_test.sh`
- `tests/e2e/project_xlings_res_test.sh`
- `tests/e2e/project_xlings_res_override_test.sh`
- `tests/e2e/project_xlings_res_region_map_test.sh`
- `tests/e2e/project_xlings_res_multi_server_test.sh`
- `tests/e2e/subos_payload_refcount_test.sh`
- `tests/e2e/release_self_install_test.sh`
- `tests/e2e/release_quick_install_test.sh`
- `tests/e2e/release_subos_smoke_test.sh`

Aggregate scripts:

- `tests/e2e/project_data_routing_test.sh` runs the local-repo + global-fallback routing checks
- `tests/e2e/project_e2e_test.sh` runs all fixed project scenarios

Runtime homes:

- `tests/e2e/runtime/`

Each scenario script creates and reuses its own isolated `XLINGS_HOME` under this directory, so E2E runs never touch the real machine-global environment.

What they validate:

- project-local index repo routing into `.xlings/data`
- global fallback routing into isolated `XLINGS_HOME/data`
- single-command multi-version installs for the same package
- `XLINGS_RES` mirror resolution and install execution
- project-level `XLINGS_RES` string override
- project-level `XLINGS_RES` region-object override
- multi-server resource selection that falls back from an unreachable endpoint to the fastest reachable one
- real shim execution after `xlings use`
- shared payload reuse across multiple subos, with final payload GC only after the last reference is removed
- release archive `self install` into an isolated user home
- quick-install bootstrap into an isolated user home
- extracted release package basic commands, subos lifecycle, d2x reuse, and RPATH verification

### Run

```bash
bash tests/e2e/project_data_routing_test.sh
bash tests/e2e/project_e2e_test.sh
bash tests/e2e/release_self_install_test.sh build/release.tar.gz
bash tests/e2e/release_quick_install_test.sh
bash tests/e2e/release_subos_smoke_test.sh build/release.tar.gz
```

## E2E Bootstrap Home

Script: `tests/e2e/bootstrap_home_test.sh`

What it validates:

- a bootstrap package root with only `.xlings.json` and `bin/xlings` is auto-detected as `XLINGS_HOME`
- `xlings self init` materializes `data/`, `subos/`, and `config/shell/` in place
- `xlings self install` copies that bootstrap home into an installed `~/.xlings`-style root
- portable and installed homes share the same top-level structure

### Run

```bash
bash tests/e2e/bootstrap_home_test.sh
```

## Install-time shims

CI verifies that after `xlings self install`, `subos/default/bin` contains all required shims (xlings, xvm, xvm-shim, xim, xsubos, xself). Shims are created at install time, not in the package, to reduce archive size.

## E2E macOS Usability Test

Script: `tests/e2e/macos_usability_test.sh`

```bash
bash tests/e2e/macos_usability_test.sh
SKIP_NETWORK_TESTS=0 bash tests/e2e/macos_usability_test.sh
```

## E2E Windows Usability Test

Script: `tests/e2e/windows_usability_test.ps1`

```powershell
powershell -ExecutionPolicy Bypass -File tests/e2e/windows_usability_test.ps1
$env:SKIP_NETWORK_TESTS = "0"
powershell -ExecutionPolicy Bypass -File tests/e2e/windows_usability_test.ps1
```
