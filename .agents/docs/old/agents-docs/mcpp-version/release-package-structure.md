# xlings Linux Release 包目录结构分析

> 分析对象：`build/xlings-0.2.0-linux-x86_64.tar.gz`  
> 分析时间：2026-02-24  
> 分析方式：对 tar 包内容与关键文件进行静态审计

---

## 1. 总体结论

- 包根目录为 `xlings-0.2.0-linux-x86_64`
- 结构完整，符合 `tools/linux_release.sh` 的组装逻辑
- `bin/`、`subos/default/bin/`、`xim/`、`config/i18n/`、`data/`、`.xlings.json`、`xmake.lua` 均存在
- 主程序和 xvm 二进制已打入包内，可解压即用
- 本次构建中 **bundled xmake 未成功下载**（构建日志出现 curl timeout），因此 `tools/xmake/bin/` 目录存在但无 `xmake` 文件

---

## 2. 包结构快照

顶层目录：

```text
xlings-0.2.0-linux-x86_64/
├── .xlings.json
├── bin/
├── config/
├── data/
├── subos/
├── tools/
├── xim/
└── xmake.lua
```

关键子目录：

```text
bin/
├── xlings
├── xvm
└── xvm-shim

subos/
├── current -> default
└── default/
    ├── bin/            # shim copies
    ├── generations/
    ├── lib/
    ├── usr/
    └── xvm/

xim/
├── base/
├── config/
├── index/
├── libxpkg/
└── pm/

data/
├── local-indexrepo/
├── runtimedir/
├── xim-index-repos/
└── xpkgs/
```

---

## 3. 内容统计

归档条目统计：

- 目录：27
- 文件：52
- 符号链接：1（`subos/current -> default`）

按顶层目录的文件数：

| 顶层项 | 文件数 |
|---|---:|
| `xim` | 46 |
| `subos` | 14 |
| `data` | 6 |
| `config` | 4 |
| `bin` | 4 |
| `.xlings.json` | 1 |
| `xmake.lua` | 1 |

按顶层项的未压缩大小（字节）：

| 顶层项 | 大小 |
|---|---:|
| `bin` | 4,437,064 |
| `subos` | 1,948,721 |
| `xim` | 128,679 |
| `config` | 8,418 |
| `.xlings.json` | 652 |
| `xmake.lua` | 554 |
| `data` | 3 |

---

## 4. 关键文件核对

存在性检查（应为 `true`）：

- `bin/xlings`：true
- `bin/xvm`：true
- `bin/xvm-shim`：true
- `.xlings.json`：true
- `xmake.lua`：true
- `config/i18n/en.json`：true
- `config/i18n/zh.json`：true
- `data/xim-index-repos/xim-indexrepos.json`：true

本次构建特例：

- `tools/xmake/bin/xmake`：**false**
  - 原因：构建阶段下载 bundled xmake 超时（`curl: (28) SSL connection timeout`）
  - 影响：包内 `xmake xim -P` 依赖系统 `xmake`，但不影响 `bin/xlings` 与 `bin/xvm` 的直接运行

---

## 5. 二进制文件细节

`bin/` 目录：

- `bin/xlings`：1,788,384 bytes
- `bin/xvm`：1,999,200 bytes
- `bin/xvm-shim`：649,480 bytes

`subos/default/bin/` 目录：

- `xlings`：649,480 bytes（shim）
- `xvm`：649,480 bytes（shim）
- `xvm-shim`：649,480 bytes（shim）

说明：

- 包内采用“两层入口”结构：`bin/` 放真实二进制，`subos/default/bin/` 放 shim 入口
- `subos/current` 软链接用于切换当前子环境

---

## 6. 结构符合性评估

与 `tools/linux_release.sh` 预期对比：

- `bin/`、`subos/default/*`、`xim/`、`config/i18n/`、`data/*`、`.xlings.json`、`xmake.lua`：均符合
- `tools/xmake/bin/xmake`：目录存在，文件缺失（网络下载失败导致）

整体结论：

- **结构设计正确且可用**
- 若用于正式发布，建议在 CI 中增加 bundled xmake 下载重试/缓存策略，确保 `tools/xmake/bin/xmake` 稳定包含在包内

