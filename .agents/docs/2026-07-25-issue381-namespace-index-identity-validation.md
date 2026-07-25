# Issue #381 — namespace 索引身份发布验证

**日期**: 2026-07-25  
**状态**: Released and verified  
**设计**:
[2026-07-25-issue381-namespace-index-identity-design.md](2026-07-25-issue381-namespace-index-identity-design.md)

## 1. 最终语义

六项 review 决策均已实现，其中两个需要特别明确：

1. 同一 effective namespace 与短名组成的完整身份若由两个 descriptor 声明，
   `build_index` 必须失败，并在错误中同时报告两个 descriptor 路径；
2. 裸依赖维持既有语义，不隐式继承声明者 namespace。存在多个候选时返回稳定的
   ambiguity，由 descriptor 显式填写 `namespace:name`。

其余行为为：

- canonical identity 使用 `namespace:name`；
- 同一仓库允许不同 namespace 下的同名包共存；
- 裸名多候选统一报 ambiguity；
- cache 直接升级为 v2，不读取 v1；
- project/global 与 primary/sub-index 的既有优先级不变。

## 2. 发布链

| 组件 | 版本/提交 | PR 与验证 |
|---|---|---|
| libxpkg | 0.0.46 / `96207e99` | [PR #29](https://github.com/openxlings/libxpkg/pull/29)，[CI 30147007758](https://github.com/openxlings/libxpkg/actions/runs/30147007758) |
| mcpp-index xpkg entry | `e7b83d98` | [PR #118](https://github.com/mcpplibs/mcpp-index/pull/118)，[CI 30147173078](https://github.com/mcpplibs/mcpp-index/actions/runs/30147173078)，[publish 30147351607](https://github.com/mcpplibs/mcpp-index/actions/runs/30147351607) |
| xlings | 0.4.69 / `14959d60` | [PR #382](https://github.com/openxlings/xlings/pull/382)，[release 30148839482](https://github.com/openxlings/xlings/actions/runs/30148839482) |
| xim-pkgindex | `dbe859b4` | [PR #414](https://github.com/openxlings/xim-pkgindex/pull/414)，8 项 PR 检查通过，[artifact 30149456519](https://github.com/openxlings/xim-pkgindex/actions/runs/30149456519) |
| mcpp-index identity cleanup | `2ede9bdb` | [PR #119](https://github.com/mcpplibs/mcpp-index/pull/119)，[CI 30149776906](https://github.com/mcpplibs/mcpp-index/actions/runs/30149776906)，[publish 30149929841](https://github.com/mcpplibs/mcpp-index/actions/runs/30149929841) |

所有 PR 均在检查通过后 squash 合入。

## 3. 产物完整性

libxpkg 0.0.46 source archive：

```text
6ffbc16108458d47f377ec6046dcf4c67f9a1d3815fc59ea331e39f31906dba0
```

mcpp-index `e7b83d9` artifact：

```text
9dec86ddb018c0b7cbaf55263902ebcb1335167991bccea27619877757a57511
```

清理历史重复身份后的 mcpp-index `2ede9bd` artifact：

```text
06186e11fe1176fabbf6b9653f01eb5ad6ef3118b9697d63155e81af887ec5d2
```

xlings 0.4.69 四平台 release：

```text
001da0cc4f736c64ce55433a62e88f680f17503e01149550dd81c283445f6760  linux-aarch64
ad9b5057120de35fe604ed6684f532a5ba10abac933aefc5ac924759872263b4  linux-x86_64
91dccc072aecbc91f1a4c67ec7c1011dd390fcf33c7dce7ec214c80237eaf6cb  macosx-arm64
17737aa0f30a42391d8f8a1fb5b787c28362d44efced8bc5ef0972ec3e56ed05  windows-x86_64
```

验证结果：

- GitHub Release 的四个归档与四个 `.sha256` sidecar 全部匹配；
- tag `v0.4.69` 精确指向 `14959d60`，release 非 draft、非 prerelease；
- GitHub 与 GitCode 的 xlings 四平台归档完整下载后 SHA256 逐项一致；
- release CI 无法上传的四个 GitCode 大文件已用本地 `tools/mirror-latest.sh
  xlings` + `gtc` 补齐，双源 16 个 URL 全部通过实际下载验证；
- xim-index 四个 tar artifact 在 GitHub/GitCode 逐字节一致；
- mcpp-index `2ede9bd` artifact 在 GitHub/GitCode 逐字节一致，且只包含 canonical
  `pkgs/m/mcpplibs.capi.lua.lua`。

## 4. xlings 验证

本地验证：

- `mcpp test`：11 个 test program，0 failed；主单测 234 项；
- namespace identity 新单测：2/2；
- namespace E2E 覆盖冷构建、cache v2 复用、v1 失效、损坏 cache 重建、显式
  alpha/beta 查询、裸名稳定 ambiguity、显式依赖、裸依赖不继承 namespace、重复完整
  身份双路径错误、default namespace 变更失效；
- GCC 15 musl 静态 release 构建通过。

PR #382 的 Linux x86_64、Linux aarch64/QEMU、Linux E2E、root、macOS、Windows
检查全部通过。Linux E2E 明确执行了 `index_same_name_namespace_test.sh`。

## 5. 最新公共生态验证

所有命令均使用临时 `HOME`、`XLINGS_HOME` 或 `MCPP_HOME`，未使用本地 dependency
override。

1. 从公开的 main quick installer 安装指定 `v0.4.69`：
   - 同时识别 GitHub/GitCode 0.4.69；
   - 实际选择 GitCode 下载；
   - `xlings --version` 输出 `xlings 0.4.69`；
   - self install 从公共索引安装 `patchelf 0.18.0`。
2. `xlings update`：
   - 主索引获取 `xim-index-dbe859b.tar.gz`；
   - awesome/scode/d2x 获取 0.4.69 artifact。
3. 普通包生命周期：
   - 安装 `ninja 1.12.1`；
   - `ninja --version` 输出 `1.12.1`；
   - 删除后 shim 不再存在。
4. 使用公开 0.4.69 二进制重新执行 namespace identity E2E，全部通过。
5. 用公开 xlings 安装 `mcpp 0.0.105`，再从全新 `MCPP_HOME` 执行：

   ```bash
   mcpp build --workspace --no-cache
   ```

   实际获取 `mcpp-index 2ede9bd`、GCC 16.1.0 和 `mcpplibs.xpkg 0.0.46`，完整下载
   xlings 依赖并在 41.15 秒内完成 release build。

### 5.1 真实验证发现并修复的索引问题

首次公共 mcpp 构建没有被忽略：它发现 mcpp-index 中两个字节相同的历史 descriptor
同时解析为 `mcpplibs.capi:mcpplibs.capi.lua`，新规则按规范拒绝构建。

PR #119 删除非 canonical 的重复文件后：

- lint、CN mirror、Linux/macOS/Windows workspace 五项 CI 全部通过；
- 新 artifact `2ede9bd` 发布至 GitHub/GitCode；
- 使用另一个全新 `MCPP_HOME` 重跑同一命令成功。

因此最终结论基于修复后的公共发布链和冷环境重跑，不是基于本地构建或缓存推断。
