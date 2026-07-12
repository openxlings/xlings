# xpkg 资源解析与兼容设计

> 编写日期: 2026-07-12 | 版本: 0.4.63

## 1. 设计结论

xlings 不重新设计 `xpm` 的平台/版本矩阵，只增加可选默认来源 `xpm.source`。libxpkg
是解析、compat 和资源归一化的唯一入口；xlings 安装器只负责资源服务器策略、下载和缓存。

```lua
xpm = {
    source = "xlings-res", -- 或 URL template
    linux = {
        ["latest"] = { ref = "1.0.0" },
        ["1.0.0"] = {
            sha256 = { x86_64 = "<x86-hash>", aarch64 = "<arm-hash>" },
        },
    },
}
```

## 2. source 与优先级

`source` 可放在 `xpm` 根级或平台级。平台级覆盖根级；版本项显式 `url` 覆盖默认
source。支持：

- `"xlings-res"`：按用户 `GLOBAL`/`CN` 资源服务器配置生成官方 URL。
- HTTP(S) URL template：支持 `${name}`、`${version}`、`${os}`、`${arch}`、
  `${arch_alias}` 和 `${ext}`。

资源归一化顺序为：跟随 `ref`、选择 per-arch map、读取显式 URL、读取平台/root
source、展开模板、选择 mirror。多架构 hash 缺失时 fail closed；完全没有 hash 仍为
兼容旧包而允许下载，但不会被视为密码学验证。

## 3. 兼容输入

以下形式继续支持：

- 字符串 URL；
- `"XLINGS_RES"`；
- `{ url = ..., sha256 = ..., ref = ... }`；
- URL mirror table；
- per-arch resource map；
- URL template + per-arch SHA256；
- 历史 `res = true + sha256`。

新配方推荐 `xpm.source = "xlings-res"` 或 URL template；`res = true` 和裸
`"XLINGS_RES"` 仅作为历史兼容写法。

## 4. 索引与发布要求

官方二进制的每个受支持平台/架构必须有权威 SHA256。`xim-pkgindex` 的版本检查器从
release sidecar/manifest 生成配方，缺失资源、sidecar 或 hash 时 fail closed。GitHub RES
与 GitCode RES 必须逐字节一致；`xim-index` 索引工件和软件包二进制是两类独立资源。

## 5. 相关规范

- [xpkg manifest v1 与资源扩展](../spec/xpkg-manifest-v1.md)
- [包索引生态](package-index-ecosystem.md)
- [索引分发](index-distribution.md)
- [完整实施台账](../../.agents/docs/2026-07-11-issue-356-partial-download-cache-fix-plan.md)
