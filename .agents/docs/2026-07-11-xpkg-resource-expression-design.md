# xpm 资源表达与兼容设计

> 日期：2026-07-11
> 修订：2026-07-12
> 状态：已实现，待随 xlings 发布
> 适用版本：libxpkg 0.0.44+
> 范围：libxpkg 解析/compat、xlings 安装器、官方 xim-pkgindex

## 1. 结论

不重新设计 `xpm`。平台和版本两层结构保持不变：

```lua
xpm = {
    linux = {
        ["1.0.0"] = {
            url = "https://example.test/foo-1.0.0-linux-x86_64.tar.gz",
            sha256 = "<sha256>",
        },
    },
}
```

只增加可选默认来源：

```lua
xpm.source = "xlings-res"
```

或：

```lua
xpm.source = "https://github.com/acme/foo/releases/download/${version}/foo-${version}-${os}-${arch}.tar.gz"
```

版本项、mirror、`ref`、显式 URL 和多架构结构仍使用原模型。`source` 只减少重复，不建立第二套 versions/targets DSL。

## 2. 设计目标

需要同时满足：

1. 官方 xlings-res 不再为每个平台、每个版本重复写 `"XLINGS_RES"`。
2. 同一 URL 模板的多个版本不再重复 URL。
3. 单 hash、per-arch hash、架构别名和 mirror 继续可用。
4. 不规则版本可用显式 URL 覆盖默认来源。
5. 旧包继续工作；兼容逻辑只存在于 libxpkg。
6. xlings 不再执行第二套 Lua 资源解析器。

## 3. 权威职责边界

```text
libxpkg loader
  读取 package/xpm，注入目标 platform/arch 兼容上下文
        ↓
libxpkg compat::resolve_resource
  ref、source 优先级、arch、hash、template、mirror 归一化
        ↓
xlings installer
  选择资源服务器、生成 xlings-res URL/fallback、形成下载任务
        ↓
xlings downloader
  mirror 尝试、SHA256、事务缓存、sidecar
```

libxpkg 是唯一解析、兼容和资源归一化入口。xlings 只保留产品配置相关策略，例如用户选择的资源服务器和 fallback 列表。

## 4. source 语法

### 4.1 根级 source

作用于所有平台：

```lua
xpm = {
    source = "xlings-res",

    linux = {
        ["1.0.0"] = {},
    },
    macosx = {
        ["1.0.0"] = {},
    },
}
```

### 4.2 平台级 source

覆盖根级 source：

```lua
xpm = {
    source = "xlings-res",

    windows = {
        source = "https://vendor.test/foo/${version}/foo-${arch_alias}.zip",
        ["1.0.0"] = {
            arch_alias = { x86_64 = "win64" },
            sha256 = { x86_64 = "<sha256>" },
        },
    },
}
```

### 4.3 source 值

仅支持两类字符串：

- `"xlings-res"`：由 xlings 根据资源服务器配置生成 URL。
- URL template：普通 HTTP(S) URL，可包含占位符。

libxpkg 对 `xlings-res` 不硬编码服务器地址；它返回 `SourceKind::XlingsRes`，由 xlings 生成主 URL 和 fallback。

## 5. 版本项

使用默认 source 且不声明 hash：

```lua
["1.0.0"] = {}
```

单 hash：

```lua
["1.0.0"] = { sha256 = "<sha256>" }
```

per-arch hash：

```lua
["1.0.0"] = {
    sha256 = {
        x86_64 = "<sha256-x86_64>",
        aarch64 = "<sha256-aarch64>",
    },
}
```

显式覆盖默认 source：

```lua
["1.0.1"] = {
    url = "https://special.test/foo-1.0.1.tar.gz",
    sha256 = "<sha256>",
}
```

版本别名：

```lua
["latest"] = { ref = "1.0.1" }
```

`ref` 支持多跳；目标缺失或形成环时 fail closed。

## 6. URL template

支持：

| 占位符 | 值 |
|---|---|
| `${name}` | package name |
| `${version}` | 跟随 ref 后的最终版本 |
| `${os}` | `linux` / `macosx` / `windows` |
| `${arch}` | 规范化架构，如 `x86_64` / `aarch64` |
| `${arch_alias}` | 当前版本的架构别名；缺省等于 `${arch}` |
| `${ext}` | Windows 为 `zip`，其他为 `tar.gz`，调用者可覆盖 |

示例：

```lua
xpm = {
    source = "https://github.com/acme/foo/releases/download/${version}/foo-${version}-${os}-${arch_alias}.${ext}",

    linux = {
        ["1.0.0"] = {
            arch_alias = {
                x86_64 = "amd64",
                aarch64 = "arm64",
            },
            sha256 = {
                x86_64 = "<sha256-amd64>",
                aarch64 = "<sha256-arm64>",
            },
        },
    },
}
```

template 同样应用于 mirror URL。

## 7. 当前支持的资源写法

### A. 字符串 URL

```lua
["1.0.0"] = "https://example.test/foo.tar.gz"
```

### B. 旧 XLINGS_RES sentinel

```lua
["1.0.0"] = "XLINGS_RES"
```

继续兼容，但新包推荐使用 `xpm.source = "xlings-res"`。

### C. URL + SHA256

```lua
["1.0.0"] = { url = "https://example.test/foo.tar.gz", sha256 = "<sha256>" }
```

### D. mirror table

```lua
["1.0.0"] = {
    url = {
        GLOBAL = "https://global.test/foo.tar.gz",
        CN = "https://cn.test/foo.tar.gz",
    },
    sha256 = "<sha256>",
}
```

### E. URL template + per-arch SHA256

```lua
["1.0.0"] = {
    url = "https://example.test/foo-${version}-${arch_alias}.tar.gz",
    arch_alias = { x86_64 = "amd64", aarch64 = "arm64" },
    sha256 = { x86_64 = "<x86-hash>", aarch64 = "<arm-hash>" },
}
```

### F. per-arch resource map

```lua
["1.0.0"] = {
    x86_64 = { url = "https://example.test/foo-amd64.tar.gz", sha256 = "<x86-hash>" },
    aarch64 = { url = "https://example.test/foo-arm64.tar.gz", sha256 = "<arm-hash>" },
}
```

### G. `res = true`

```lua
["1.0.0"] = {
    res = true,
    sha256 = { x86_64 = "<x86-hash>", aarch64 = "<arm-hash>" },
}
```

这是已发布的历史 V2 输入，libxpkg 继续兼容；新包不推荐使用，推荐上提为 `xpm.source = "xlings-res"`。

## 8. 归一化优先级

```text
1. 定位 platform/version。
2. 跟随 ref 到最终版本，并检测 missing target/cycle。
3. 若存在 per-arch resource map，严格选择 host arch。
4. 选择单 hash 或 host arch 对应 hash。
5. 版本显式 URL 优先。
6. 平台 source 次之。
7. 根级 source 最后。
8. 展开主 URL 和 mirrors 的 template。
```

以下情况 fail closed：

- 平台或版本不存在。
- ref 目标不存在或形成环。
- per-arch resource map 非空但缺少 host arch。
- per-arch SHA256 表非空但缺少 host arch。
- 最终既没有 URL 也没有 source。

hash 完全缺失仍允许下载，以兼容旧包；这不等于完整性已验证。

## 9. 两个最佳范例

### 9.1 官方 xlings-res

```lua
package = {
    spec = "2",
    name = "mcpp",
    archs = { "x86_64", "aarch64" },

    xpm = {
        source = "xlings-res",

        linux = {
            ["latest"] = { ref = "0.0.87" },
            ["0.0.87"] = {
                sha256 = {
                    x86_64 = "<linux-x86_64-sha256>",
                    aarch64 = "<linux-aarch64-sha256>",
                },
            },
        },

        macosx = {
            ["latest"] = { ref = "0.0.87" },
            ["0.0.87"] = {
                sha256 = {
                    x86_64 = "<macosx-x86_64-sha256>",
                    aarch64 = "<macosx-aarch64-sha256>",
                },
            },
        },

        windows = {
            ["latest"] = { ref = "0.0.87" },
            ["0.0.87"] = {
                sha256 = { x86_64 = "<windows-x86_64-sha256>" },
            },
        },
    },
}
```

### 9.2 用户自定义 release + mirror + 特殊版本覆盖

```lua
package = {
    spec = "2",
    name = "foo",
    archs = { "x86_64", "aarch64" },

    xpm = {
        source = "https://github.com/acme/foo/releases/download/v${version}/foo-${os}-${arch_alias}.${ext}",

        linux = {
            ["latest"] = { ref = "2.1.0" },
            ["2.1.0"] = {
                arch_alias = { x86_64 = "amd64", aarch64 = "arm64" },
                sha256 = { x86_64 = "<linux-amd64>", aarch64 = "<linux-arm64>" },
            },
            ["2.0.0"] = {
                url = {
                    GLOBAL = "https://legacy.test/foo-2.0.0-linux.tar.gz",
                    CN = "https://cn.test/foo-2.0.0-linux.tar.gz",
                },
                sha256 = "<legacy-sha256>",
            },
        },

        windows = {
            source = "https://downloads.acme.test/foo/${version}/foo-${arch_alias}.zip",
            ["2.1.0"] = {
                arch_alias = { x86_64 = "win64" },
                sha256 = { x86_64 = "<windows-win64>" },
            },
        },
    },
}
```

## 10. Loader 平台兼容

旧包可能在 Lua 顶层使用：

```lua
if is_host("linux") then
    package.xpm.linux = { ... }
end
```

libxpkg 0.0.44 增加 `LoaderContext { platform, arch }`，统一提供：

- `is_host()` / `is_plat()`
- `is_arch()`
- `os.host()` / `os.arch()`
- `_RUNTIME.platform` / `_RUNTIME.arch`

xlings 安装阶段通过该上下文加载包，因此删除旧的 `load_platform_entries_()` 不会牺牲这类历史兼容性。

## 11. 老客户端与官方索引迁移

新增字段对新客户端是可选元数据，但部分旧 libxpkg/xlings loader 会把根级 `source` 当作平台或把平台级 `source` 当作版本。因此不能仅凭“字段是追加的”推断旧客户端安全。

官方索引迁移遵循：

1. 历史 `"XLINGS_RES"` 条目不重写，老客户端继续读取。
2. 新版本先补权威 SHA256；没有完整 hash 时不伪造。
3. 实际运行当前稳定旧客户端和修复版解析/安装同一 fixture。
4. 只有旧客户端不会误解析 `source`，才可在其可见索引轨道加入推荐写法。
5. 如果旧客户端不兼容，利用版本化 index artifact/pointer 保留旧快照；新语法只进入要求新 xlings 的索引轨道，或暂时继续发布旧 sentinel + hash 结构。

索引 artifact/pointer 能提供回滚和旧快照，但不能替代解析兼容测试。

## 12. 实现与验证

- libxpkg 0.0.43：实现 `source`、compat 归一化和旧资源模型测试。
- libxpkg 0.0.44：增加平台 loader context 和 per-arch fail-closed。
- xlings：删除 `load_platform_entries_()`、本地 template 展开器和第二套 Lua sandbox；安装器只调用 libxpkg。
- tinyhttps 0.2.9：返回实际/期望字节、最终 URL、ETag 和 Last-Modified，供事务缓存 sidecar 使用。

最终发布门禁以 issue #356 实施文档的 PR、CI、release、索引和 E2E 台账为准。
