# xlings: Official mcpp-index Migration Plan

> 状态: pending on mcpp 0.0.35 and mcpp-index packages
> 分支: `codex/use-official-mcpp-index`
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
libarchive = "3.8.7"

[dependencies.mcpplibs]
cmdline = "0.0.2"
xpkg = "0.0.41"
tinyhttps = "0.2.3"
capi.lua = "0.0.3"
```

最终命名以 mcpp-index PR 为准；若官方包保留 `compat.libarchive`，则 xlings 应使用 mcpp 推荐的最简写法或显式 compat 命名空间。

## xlings 侧待办

- [ ] 等 mcpp 0.0.35 发布并本地安装。
- [ ] 等 mcpp-index 提供 `compat.libarchive` 及其传递依赖。
- [ ] 删除 `mcpp/pkgs/*`。
- [ ] 删除 `mcpp/include/*` 中仅为第三方包服务的配置头。
- [ ] 精简 `mcpp.toml` 的 `build.cflags`，只保留 xlings 自身语义。
- [ ] 更新 `mcpp.lock`。
- [ ] 本地验证 glibc target。
- [ ] 本地验证 musl static target。
- [ ] 创建 xlings PR，CI 通过后合入并发布新版本。

## 验证命令

```bash
mcpp self doctor
mcpp build
mcpp build --target x86_64-linux-musl
target/x86_64-linux-gnu/*/bin/xlings --version
target/x86_64-linux-musl/*/bin/xlings --version
```

## Checkpoints

- [ ] 文档 checkpoint commit。
- [ ] mcpp 0.0.35 本地联调通过。
- [ ] 官方 mcpp-index 联调通过。
- [ ] xlings PR draft 创建。
- [ ] CI 每 120s 检查一次直到完成。
- [ ] 合入后发布 xlings 新版本。

