# xlings: Official mcpp-index Migration Plan

> 状态: complete
> 分支: `codex/use-official-mcpp-index`
> PR: https://github.com/openxlings/xlings/pull/314
> 目标: xlings 不再维护项目本地 mcpp 索引，直接使用默认官方 mcpp-index，并保持 `mcpp.toml` 简洁可维护。

## 当前问题

当前 `mcpp.toml` 直接包含大量第三方 C 库 configure 宏，并通过本地索引声明 `xlings.libarchive`。这能构建，但不是理想生态:

- xlings 承担了 libarchive/xz/zstd/zlib 等包的维护责任。
- 宏污染根 manifest，后续其他项目无法复用。
- 默认 mcpp-index 不能独立满足 xlings 构建。

## 目标形态

```toml
[indices]
# 不再需要项目本地 xlings mcpp index。

[dependencies]
ftxui = "6.1.9"

[dependencies.compat]
libarchive = "3.8.7"

[dependencies.mcpplibs]
cmdline = "0.0.2"
xpkg = "0.0.41"
tinyhttps = "0.2.3"
capi.lua = "0.0.3"
```

官方包索引采用 `compat.libarchive`，因此 xlings 使用 `[dependencies.compat]` 显式声明归档库，避免项目维护自定义 index。

## xlings 侧待办

- [x] 等 mcpp 0.0.35 发布并本地安装。
- [x] mcpp-index PR #17 提供 `compat.libarchive` 及其传递依赖。
- [x] xim-pkgindex PR #259 已合入,`xim:mcpp` bootstrap 版本升到 `0.0.35`。
- [x] 删除 `mcpp/pkgs/*`。
- [x] 删除 `mcpp/include/*` 中仅为第三方包服务的配置头。
- [x] 精简 `mcpp.toml` 的 `build.cflags`，只保留 xlings 自身语义。
- [x] 更新 `mcpp.lock`。
- [x] 本地验证 glibc target。
- [x] 本地验证 musl static target。
- [x] 创建 xlings PR draft。
- [x] CI 通过后合入并发布新版本。

## 验证命令

```bash
mcpp self doctor
mcpp build
mcpp build --target x86_64-linux-musl
target/x86_64-linux-gnu/*/bin/xlings --version
target/x86_64-linux-musl/*/bin/xlings --version
```

## Checkpoints

- [x] 文档 checkpoint commit。
- [x] mcpp 0.0.35 本地联调通过。
- [x] 官方 mcpp-index 联调通过。
- [x] xlings PR draft 创建: https://github.com/openxlings/xlings/pull/314
- [x] CI 每 120s 检查一次直到完成。
- [x] 合入后发布 xlings 新版本。

## 当前 CI 状态

旧 run 的三平台 CI 失败在 `mcpp build` 的 `compat.libarchive@3.8.7`
下载阶段。根因是 GitHub runner 当时使用的官方 mcpp-index `main` 尚未包含
`compat.libarchive`。

截至 2026-05-30:

- mcpp PR #88 已合入并发布 `v0.0.35`。
- mcpplibs/mcpp-index PR #17 已合入 main。
- openxlings/xim-pkgindex PR #259 已合入 main,CI/release 现在 pin 到
  `cf3d0fa64e8be120c3c703c8702f294f271026ad`,包含 `xim:mcpp@0.0.35`。
- xlings CI/release 在安装 mcpp 后会清理缓存中的默认 `mcpplibs` index checkout,
  避免 actions/cache 恢复旧 index 后看不到刚合入的 `compat.*` 包。

## Final Outcome

- PR: https://github.com/openxlings/xlings/pull/314
- Merge commit: `1c301aa37cb752334ecccd59efc6a7c842140cbf`
- Release: https://github.com/openxlings/xlings/releases/tag/v0.4.46
- Release run: https://github.com/openxlings/xlings/actions/runs/26690474975
- CI evidence:
  - `xlings-ci-linux`: success.
  - `xlings-ci-macos`: success.
  - `xlings-ci-windows`: success.
  - Linux `E2E-00: mcpp builds xlings from source`: passed using `mcpp 0.0.35` and official `compat.libarchive`.
- Follow-up index/mirror:
  - `xlings-res/xlings` mirror `0.4.46` published.
  - `openxlings/xim-pkgindex` PR #260 updated `xlings latest` to `0.4.46`.

## Follow-up: mcpp 0.0.36 Bootstrap

Final local verification exposed a mixed-cache edge case in mcpp 0.0.35: an
existing `~/.mcpp/registry/data` with other xlings index clones but without
`mcpplibs` could be treated as fresh, so `compat.libarchive` was not found until
the user ran `mcpp index update` manually.

- mcpp PR #89 fixed default `mcpplibs` freshness and released `v0.0.36`.
- openxlings/xim-pkgindex PR #261 updated `xim:mcpp` latest to `0.0.36`.
- xlings now pins CI/release bootstrap index ref
  `fba1ce953589df609c862d0a895fc9d6ff5eb6de` and requires `mcpp 0.0.36` in
  `.xlings.json`.
- xlings version is bumped to `0.4.47` for this bootstrap/runtime hardening
  release.
- xlings PR: https://github.com/openxlings/xlings/pull/315
- Commit: `075215d chore(release): bootstrap mcpp 0.0.36`
- Local verification:
  - `mcpp --version` -> `mcpp 0.0.36 -`
  - `mcpp build` -> `Finished release [optimized]`
  - `target/x86_64-linux-gnu/d14fbbf7aeceb894/bin/xlings --version` -> `xlings 0.4.47`
