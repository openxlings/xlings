> 编写日期: 2026-05-17 | 版本: 0.4.36

# .xlings.json 配置文件规范

## 概述

`.xlings.json` 是 xlings 的统一配置文件，采用 JSON 格式。根据放置位置不同，承担全局配置、项目配置或 SubOS 工作区配置的职责。

## 文件位置

| 位置 | 路径 | 用途 |
|------|------|------|
| 全局配置 | `~/.xlings/.xlings.json` | 用户级默认设置、活跃 SubOS、镜像源 |
| 项目配置 | `<project>/.xlings.json` | 项目依赖声明、项目级 SubOS 绑定 |
| SubOS 工作区 | `~/.xlings/subos/<name>/.xlings.json` | SubOS 内已安装工具及版本状态 |

配置加载优先级：项目配置中的 `mirror`、`lang` 会覆盖全局值。

## 全局配置字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `mirror` | `string` | 镜像标识，影响默认 index 仓库 URL 及资源服务器选择 |
| `lang` | `string` | 界面语言（如 `"zh"`、`"en"`） |
| `activeSubos` | `string` | 当前激活的全局 SubOS 名称 |
| `versions` | `object` | 全局版本数据库，键为**目标名**（不一定是包名），值见下方"versions 条目格式" |
| `version` | `string` | 建立/最后体检这个 home 的 xlings 版本。`self doctor --fix` 成功后写入，用来判断包记录是否还是旧客户端的格式 |
| `index_repos` | `array` | 包索引仓库列表，每项含 `name` 和 `url` |
| `XLINGS_RES` | `object \| string \| array` | 资源服务器配置（见下方说明） |

### versions 条目格式

`versions` 的键是 **xvm 目标名**，也就是可以被 `xlings use` 切换的名字 —— 它不一定
等于包名。一个包可以注册多个目标：llvm 注册出 `clang`、`lld`、`llvm-ar` 等等，
它们都是目标，但索引里只有 `llvm` 这一个包。

```json
"versions": {
  "python": {
    "type": "program",
    "filename": "python3",
    "versions": {
      "3.12.0":       { "path": ".../xim-x-python/3.12.0/bin" },
      "local:3.11.0": { "path": ".../local-x-python/3.11.0/bin" }
    }
  }
}
```

| 字段 | 说明 |
|------|------|
| `type` | `program` / `lib` / `files` / `group` |
| `filename` | payload 里真实的文件名，和目标名不同时使用 |
| `versions` | 键是**版本键**，值是该版本的记录 |
| `bindings` | 遗留的成对绑定边（旧客户端写的），新客户端写 `bindingGroup` |

**版本键里带命名空间。** 键写作 `<namespace>:<version>`（如 `local:3.11.0`），
没有命名空间就是裸版本号。注意它和命令行坐标的顺序是**相反的**：

| 场合 | 写法 | 例子 |
|------|------|------|
| 版本键（本文件内） | `ns:version` | `local:0.0.27` |
| 命令行坐标 | `ns:package@version` | `local:mcpp@0.0.27` |
| `xlings use` 的版本参数 | `ns:version` | `xlings use mcpp local:0.0.27` |

版本记录里常见的字段：

| 字段 | 说明 |
|------|------|
| `path` | payload 目录，布局是 `data/xpkgs/<ns>-x-<package>/<version>/...` |
| `alias` | 转发到的真实命令；绝对路径表示指向 payload 之外 |
| `envs` | 该版本激活时注入的环境变量 |
| `bindingGroup` | 所属发布组：`provider` / `providerVersion` / `rootTarget` 等 |
| `bindingMembers` | 组根上记录的成员列表 |

`xlings self doctor` 检查的就是这些记录与磁盘是否一致，
`--fix` 会修复不一致的部分 —— 详见[自我管理与修复](../quick-start/self-management.md)。

### index_repos 格式

```json
"index_repos": [
  { "name": "ros2", "url": "https://github.com/example/ros2-index" },
  {
    "name": "mcpplibs",
    "url": "https://github.com/mcpp-community/mcpp-index.git",
    "artifact": "https://github.com/xlings-res/mcpp-index",
    "source": "auto"
  }
]
```

默认索引仓库始终保留；用户定义的仓库为追加关系。

每项字段(0.4.68+ 支持 `artifact` / `source`,均可选):

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | `string` | 索引命名空间(必填) |
| `url` | `string` | Git 远程地址或本地路径(必填;artifact 模式下作为回退) |
| `artifact` | `string \| object` | artifact 来源 base:GitHub/GitCode 仓库 URL、静态 HTTP 目录、本地目录/`file://`;或区域对象 `{"GLOBAL":..,"CN":..}` |
| `source` | `string` | `auto`(默认,artifact 优先、git 回退)\| `artifact`(只走 artifact)\| `git`(强制 git) |

### XLINGS_RES 格式

支持三种写法：

```json
// 对象形式：按镜像名分组
"XLINGS_RES": {
  "DEFAULT": ["https://res1.example.com", "https://res2.example.com"],
  "cn": "https://cn-mirror.example.com"
}

// 字符串形式：设为默认服务器
"XLINGS_RES": "https://res.example.com"

// 数组形式：设为默认服务器列表
"XLINGS_RES": ["https://res1.example.com", "https://res2.example.com"]
```

兼容旧字段名：`resource_servers`、`resource_server`、`res_servers`。

## 项目配置字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `workspace` | `object` | 项目依赖的工具及版本（支持跨平台语法） |
| `projectScope` | `boolean` | 设为 `false` 时跳过项目模式激活，仅作为依赖清单 |
| `subos` | `string` | 项目绑定的命名 SubOS 名称 |
| `mirror` | `string` | 覆盖全局镜像设置 |
| `lang` | `string` | 覆盖全局语言设置 |
| `index_repos` | `array` | 项目级索引仓库（格式同全局） |
| `XLINGS_RES` | `object \| string \| array` | 项目级资源服务器（格式同全局） |

备注：`projectSubos` 为 `subos` 的旧名，仍可识别。

### workspace 跨平台版本语法

每个工具的值支持两种形式：

```json
"workspace": {
  "gcc": "16.1.0",
  "clang": {
    "linux": "19.1.0",
    "macosx": "19.1.0",
    "windows": "18.1.8",
    "default": "19.1.0"
  }
}
```

- **简单形式**：`"tool": "version"` — 所有平台使用同一版本。
- **平台条件形式**：值为对象，键为平台名（`linux`、`macosx`、`windows`），运行时解析当前平台对应版本。可选 `default` 键作为兜底。

## SubOS 工作区字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `workspace` | `object` | 已安装工具的版本状态（见下方格式） |
| `subos_info` | `object` | SubOS 自身的描述：运行时与环境声明（见下方） |
| `storage` | `string` | 存储模式：`"shared"`、`"tmpfs"`、`"image"` |
| `imageSize` | `string` | 当 storage 为 `image` 时的磁盘映像大小（如 `"4G"`） |

### subos_info（2026.8.5+）

`workspace` 记录的是这个 SubOS **装了什么**；`subos_info` 记录的是它**是什么** ——
二进制针对哪个运行时构建，以及进入它的进程需要哪些环境变量。

```json
"subos_info": {
  "schema_version": 1,
  "runtime": "glibc@2.39",
  "envs": {
    "compat.mesa@25.0.0": [
      { "var": "LIBGL_DRIVERS_PATH", "op": "set",     "value": "${pkgdir}/lib/dri" },
      { "var": "XDG_DATA_DIRS",      "op": "prepend", "value": "${pkgdir}/share" }
    ]
  },
  "created_at": "2026-08-05T14:23:11Z",
  "created_by": "xlings 2026.8.5.1"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `schema_version` | `integer` | 当前为 `1` |
| `runtime` | `string` | 运行时 binding `<name>@<version>`，自描述（`glibc@2.39` 即 Linux/glibc）。由 `xlings subos new --runtime` 指定 |
| `envs` | `object` | 以**声明包的 binding** 为键。包卸载时 xlings 删除整段；recipe 不写清理代码 |
| `created_at` / `created_by` | `string` | 创建时间与创建者版本 |

`envs` 的值由包在 `config()` 里通过 `subos.env{}` 写入（见 xim-pkgindex 的
xpackage-spec V2）。**值必须使用占位符** —— `${pkgdir}` / `${subosdir}` /
`${home}` / `${xlings_home}` —— 写死绝对路径会让这份描述只在写它的机器上成立。
占位符在进入 SubOS 时展开。

`envs` 为空时保留 `{}`,不省略键：缺失和空是两种写法、同一个意思,会让每个读者都要处理两遍。

进入 SubOS(`xlings subos use`)时这些变量被注入进程；**用户自己已 export 的值优先**,
`set` 不覆盖它,`prepend` 仍然与它拼接。`xlings self doctor` 检查这一段的完整性、
与已装包的一致性、占位符可解性,以及是否有多个包争抢同一变量。

### workspace 条目格式（SubOS）

SubOS 工作区中每个工具支持三种值形式：

```json
"workspace": {
  "python": "3.12.0",
  "gcc": {
    "active": "16.1.0",
    "installed": ["16.1.0", "15.1.0"]
  }
}
```

- **字符串形式**（旧版）：仅记录活跃版本，无已安装列表。
- **对象形式**（0.4.19+）：`active` 为当前使用版本，`installed` 为已安装版本数组。
- **平台条件形式**：同项目 workspace 语法，作为兼容性兜底。

区分对象形式与平台条件形式的方法：存在 `active` 或 `installed` 键即为对象形式。

## 完整示例

### 全局配置 (`~/.xlings/.xlings.json`)

```json
{
  "mirror": "cn",
  "lang": "zh",
  "activeSubos": "dev",
  "version": "2026.7.29.0",
  "versions": {
    "python": {
      "type": "program",
      "versions": {
        "3.12.0": { "path": "/home/me/.xlings/data/xpkgs/xim-x-python/3.12.0/bin" }
      }
    }
  },
  "index_repos": [
    { "name": "ros2", "url": "https://github.com/example/ros2-pkgindex" }
  ],
  "XLINGS_RES": {
    "cn": "https://cn-res.xlings.org"
  }
}
```

### 项目配置 (`<project>/.xlings.json`)

```json
{
  "workspace": {
    "gcc": { "linux": "16.1.0", "macosx": "15.1.0" },
    "python": "3.12.0",
    "cmake": "3.30.0"
  },
  "subos": "myproject-env"
}
```

### SubOS 工作区 (`~/.xlings/subos/dev/.xlings.json`)

```json
{
  "storage": "image",
  "imageSize": "4G",
  "subos_info": {
    "schema_version": 1,
    "runtime": "glibc@2.39",
    "envs": {},
    "created_at": "2026-08-05T14:23:11Z",
    "created_by": "xlings 2026.8.5.1"
  },
  "workspace": {
    "gcc": {
      "active": "16.1.0",
      "installed": ["16.1.0", "14.2.0"]
    },
    "python": {
      "active": "3.12.0",
      "installed": ["3.12.0", "3.11.9"]
    }
  }
}
```
