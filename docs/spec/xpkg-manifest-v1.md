# xpkg 包描述文件规范 (spec v1)

> 编写日期: 2026-05-17 | 版本: 0.4.36

## 1. 概述

xpkg 包描述文件是一个 Lua 脚本（`.lua`），用于声明软件包的元数据、平台资源矩阵及生命周期钩子。文件顶层必须定义一个名为 `package` 的全局 table，并可选地定义 `install()`、`config()`、`uninstall()` 函数。

文件由 libxpkg 加载器在沙箱环境中执行；`import()` 等 xmake 风格调用在加载阶段为空操作，仅在实际安装阶段生效。

## 2. 必填字段

| 字段   | 类型     | 说明                                           |
|--------|----------|------------------------------------------------|
| `spec` | string   | 规范版本，当前固定为 `"1"`                     |
| `name` | string   | 包名，全小写，用于索引唯一标识                 |
| `type` | string   | 包类型，见第 4 节                              |
| `xpm`  | table    | 平台-版本资源矩阵，见第 5 节                   |

## 3. 可选字段

| 字段          | 类型         | 说明                                         |
|---------------|--------------|----------------------------------------------|
| `namespace`   | string       | 命名空间，如 `"subos"`，用于隔离包名         |
| `description` | string       | 一句话描述                                   |
| `licenses`    | string[]     | SPDX 许可证标识符列表                        |
| `repo`        | string       | 源码仓库 URL                                 |
| `homepage`    | string       | 项目主页 URL                                 |
| `docs`        | string       | 文档 URL                                     |
| `archs`       | string[]     | 支持架构，如 `{"x86_64", "arm64"}`           |
| `status`      | string       | `"dev"` (默认) / `"stable"` / `"deprecated"` |
| `categories`  | string[]     | 分类标签                                     |
| `keywords`    | string[]     | 搜索关键词                                   |
| `xvm_enable`  | boolean      | 是否注册到 xvm 版本管理器（默认 false）      |
| `maintainers` | string[]     | 维护者列表                                   |
| `authors`     | string[]     | 作者列表                                     |
| `programs`    | string[]     | 包提供的可执行文件名                         |

## 4. 包类型 (type)

| 值           | 内部编号 | 用途                                     |
|--------------|----------|------------------------------------------|
| `"package"`  | 0        | 标准软件包（默认）                       |
| `"script"`   | 1        | 脚本工具                                 |
| `"template"` | 2        | 项目模板                                 |
| `"config"`   | 3        | 配置集                                   |
| `"subos"`    | 4        | 子系统沙箱环境                           |

## 5. xpm 平台-版本矩阵

`xpm` 为顶层 table，键为平台名 (`linux` / `macosx` / `windows`)，值为该平台的版本条目 table。

### 5.1 版本条目

每个平台 table 中，字符串键即版本号，对应 table 包含以下可选字段：

| 字段     | 类型           | 说明                                       |
|----------|----------------|--------------------------------------------|
| `url`    | string / table | 下载地址；table 时为镜像表 `{GLOBAL=..., CN=...}` |
| `sha256` | string / nil   | 校验和；nil 表示暂未计算                   |
| `ref`    | string         | 引用另一版本（别名），如 `latest` → 实际版本 |

示例：

```lua
["latest"] = { ref = "20.1.7" },
["20.1.7"] = {
    url = "https://example.com/pkg-20.1.7.tar.xz",
    sha256 = "abcdef...",
},
```

### 5.2 平台级字段

除版本条目外，平台 table 还可包含以下键（非版本条目，加载器会跳过）：

- `deps` — 依赖声明，见第 7 节
- `inherits` — 字符串，继承另一平台的配置
- `exports` — 导出声明（高级，用于 ELF patch）

## 6. 生命周期钩子函数

钩子定义在文件顶层（`package` table 外部），安装器在对应阶段调用。

| 函数           | 调用时机               | 职责                                           |
|----------------|------------------------|------------------------------------------------|
| `install()`    | 下载并解压资源后       | 将文件放置到 `pkginfo.install_dir()`，返回 bool |
| `config()`     | install 成功后         | 注册 xvm shim、设置环境变量等，返回 bool       |
| `uninstall()`  | 用户执行卸载时         | 清除 xvm 注册及自定义配置，返回 bool           |

所有钩子返回 `true` 表示成功，`false` 表示失败。对于 `type = "subos"` 的包，若未定义钩子，安装器使用内置默认实现（创建骨架目录 + xvm 注册）。

钩子中可通过 `import()` 使用以下模块：
- `xim.libxpkg.pkginfo` — 获取安装路径、版本信息
- `xim.libxpkg.xvm` — xvm 版本管理操作
- `xim.libxpkg.log` — 日志输出

## 7. 依赖声明 (deps)

`deps` 位于 `xpm.<platform>` 层级。支持两种形式：

### 7.1 数组形式（旧版，运行时 + 构建时共用）

```lua
deps = { "node", "npm" }
```

所有条目同时作为 runtime 和 build 依赖。

### 7.2 表形式（推荐，显式分离）

```lua
deps = {
    runtime = { "python" },
    build   = { "cmake", "ninja" },
}
```

- `runtime` — 运行时依赖，安装后激活到工作区
- `build` — 构建时依赖，仅在 `install()` 钩子执行期间可用，不激活到工作区

构建依赖在 install 阶段通过 `XLINGS_BUILDDEP_<NAME>_PATH` 环境变量及 PATH 注入暴露给钩子。

## 8. 完整示例

### 8.1 type = "subos"

```lua
package = {
    spec = "1",
    name = "py-demo",
    namespace = "subos",
    description = "Python 开发子系统",
    licenses = {"MIT"},
    type = "subos",
    archs = {"x86_64", "arm64"},

    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
        macosx = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
        windows = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"]  = {},
        },
    }
}
-- subos 类型无需定义钩子，安装器自动处理骨架创建与 xvm 注册
```

### 8.2 type = "package"（含依赖与钩子）

```lua
package = {
    spec = "1",
    name = "llvm",
    description = "LLVM compiler infrastructure and toolchain",
    licenses = {"Apache-2.0 WITH LLVM-exception"},
    type = "package",
    repo = "https://github.com/llvm/llvm-project",
    docs = "https://llvm.org/docs/",
    archs = {"arm64"},
    status = "stable",
    categories = {"compiler", "toolchain"},
    keywords = {"llvm", "clang"},
    xvm_enable = true,

    xpm = {
        macosx = {
            deps = {
                runtime = { "libcxx" },
                build   = { "cmake" },
            },
            ["latest"] = { ref = "20.1.7" },
            ["20.1.7"] = {
                url = "https://example.com/LLVM-20.1.7-macOS-ARM64.tar.xz",
                sha256 = "abcdef1234567890...",
            },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    os.tryrm(pkginfo.install_dir())
    os.mv(pkginfo.install_file():replace(".tar.xz", ""), pkginfo.install_dir())
    return true
end

function config()
    xvm.add(package.name, { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove(package.name)
    return true
end
```
