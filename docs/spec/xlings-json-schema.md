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
| `lang` | `string` | 界面语言：`"auto"`（默认，跟随系统）/ `"zh"` / `"en"` |
| `uiMode` | `string` | 前端：`"auto"`（默认）/ `"cli"` / `"tui"`。详见 [diagnostics.md](diagnostics.md#7-前端与模式) |
| `theme` | `string` | 配色主题的**路径引用**，或自带主题的名字。详见 [themes.md](themes.md) |
| `tui` | `object` | tui 前端自己的设置。目前只有 `interactive`（布尔，默认 `false`）|
| `hintsSeen` | `array` | 已经展示过的一次性提示 id。由 xlings 维护，不需要手写 |
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
| `fileSrc` / `fileDst` | 仅 `type = "files"`：payload 内的源、subos 根下的目的地，两端都是相对路径 |

`xlings self doctor` 检查的就是这些记录与磁盘是否一致，
`--fix` 会修复不一致的部分 —— 详见[自我管理与修复](../quick-start/self-management.md)。

### 声明式文件资产(`type = "files"`)的生命周期

recipe 用 `xvm.files{src=, dst=, binding=}` 声明一个既不是程序也不是库的
条目（头文件、`.pc`、证书）。它**注册成自己的 target**，名字由 libxpkg
生成（`<pkg>.files.<n>`，`<n>` 是安装时按声明顺序递增的计数器），
绑定到发布上，**不挂在包自己的条目上**。

这条命名规则是契约的一部分,因为它决定了怎么问问题:

> **要枚举一个发布放了哪些资产,必须遍历它的成员**
> (`release_file_placements`)。拿包名去查 `file_placement` 永远是空的
> —— 那不是"没有资产",是问错了对象。2026.7.27.0 到 2026.8.26.1 之间
> 卸载路径就是这么问的,所以它一个资产都没回收过,也一句都没说
> (openxlings/xlings#423)。

两端都必须是相对路径:一个 payload 被多个 subos 共享,
记在它身上的绝对目的地对装它的那个 subos 是对的、对其余每一个都是错的。
`dst` 的顶层只允许 `usr/` `etc/` `share/`,不允许绝对路径、不允许 `..`、
不允许 `bin/`(那是 shim 的地方)。

**客户端保证(2026.8.26.1 起)。** 一个发布不再被本 subos 需要时——
无论是完整卸载(payload 删除)、detach(payload 还被别的 subos 用着)、
重新注册后不再声明该目的地,还是 `use` 切到一个资产集更小的版本——
它声明过的目的地都会被回收。回收规则对四条路径是同一份代码:

| 目的地的状态 | 动作 |
|---|---|
| 没有任何幸存声明 | 删除，并回收只为它存在的空目录（保留 `usr/include` 这类两段路径） |
| 有幸存声明，且在**本 subos active** | **重新指向**那个发布的 payload |
| 有幸存声明，但本 subos 没有 active | 删除 |
| 链接指向 payload store 之外（有人换掉了它） | 保留并告警——那是 `xvm-sysroot-drift`，不由卸载来裁决（仅 POSIX 可判定，Windows 上资产是硬链接/副本，只能由声明决定） |

因此 recipe **不需要**在 `uninstall()` 里手写 `rm` 来镜像自己的声明。

**判据。** 验证回收不要用 `[ -e ]`——它跟随符号链接，payload 删掉之后
一个**仍然存在**的悬空链接会读作"不存在"。用 `-xtype l`；
而要覆盖 detach 那一半（链接不悬空），只能对账：
subos 下指向 payload store 的链接集合 − 本 subos active 声明的 `fileDst`
集合 = 必须为空。

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
| `uiMode` | `string` | 覆盖全局前端设置 |
| `theme` | `string` | 覆盖全局主题；相对路径按**项目根**解析，所以项目可以自带配色 |
| `tui` | `object` | 覆盖全局 tui 设置 |
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
  "runtime": "glibc@2.44",
  "runtime_source": "index",
  "host_glibc": "2.39",
  "envs": {
    "compat.mesa@25.0.0": [
      { "var": "LIBGL_DRIVERS_PATH", "op": "set",     "value": "${pkgdir}/lib/dri" },
      { "var": "XDG_DATA_DIRS",      "op": "prepend", "value": "${pkgdir}/share" }
    ]
  },
  "created_at": "2026-08-05T14:23:11Z",
  "created_by": "xlings 2026.8.9.1"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `schema_version` | `integer` | 当前为 `1` |
| `runtime` | `string` | 运行时 binding `<name>@<version>`，自描述（`glibc@2.44` 即 Linux/glibc）。**（2026.8.17.1+）可缺席**，见下方「已知未知」 |
| `runtime_source` | `string` | **（2026.8.27.2+，可选）** 上面那个 binding **是怎么来的**：`explicit` / `index` / `fallback`。缺失 = 写块的那版 xlings 还没有这个字段，不是「来源未知的第四种」。见下方「binding 的出处」 |
| `host_glibc` | `string` | **（2026.8.9+，可选）** 写入该块时探测到的宿主 glibc 版本（如 `"2.39"`）。缺失 = 未知（旧 manifest、非 glibc 宿主、探测失败），读者必须把未知当"不可证"，不得当 0 比较。供闭环规则 A（`our_glibc >= host_glibc`）判定 |
| `envs` | `object` | 以**声明包的 binding** 为键。包卸载时 xlings 删除整段；recipe 不写清理代码 |
| `created_at` / `created_by` | `string` | **创建**时间与创建者版本。只有真正创建这个 SubOS 的那次运行会写 |
| `described_at` / `described_by` | `string` | **（2026.8.17.1+）** 事后**描述**一个已存在 SubOS 的时间与版本。与 `created_*` **二选一**，两者都缺才是缺陷 |

#### `runtime` 缺席 =「已知未知」（2026.8.17.1+）

三种状态，读者必须区分：

| 盘上的样子 | 含义 |
|---|---|
| 没有 `subos_info` 块 | 这个 SubOS 建于 `subos_info` 存在之前（旧格式） |
| 有块，**没有 `runtime` 键** | **看过了，说不出来** —— 没有任何记录或载荷能回答 |
| 有块，`runtime` 是良构 binding | 声明成立 |

`runtime` 存在时**必须**是 `<name>@<version>`；空字符串不是合法写法（缺席才是）。

**为什么需要这一档。** 2026.8.17.1 之前不变式要求 `runtime` 必须良构，于是一次
没有证据的描述只有两个选择：编一个，或者产出一个 `--fix` 会永远重写的块。
它编了一个 —— 实测某台真机上两个 SubOS 因此声明 `glibc@2.44`，而它们的
`lib/libc.so.6` 指向 2.39 的载荷（openxlings/xlings#547）。
**一个常量和一个真实答案在盘上长得一模一样**，这一档就是为了让它们不一样。

下游读到缺席时应当**降级并说明原因**，而不是替它选一个默认值。

#### binding 的出处：`runtime_source`（2026.8.27.2+）

`runtime` 只说**绑定是什么**，不说**它是怎么被决定的**。这两件事在盘上长得一样，
可信度却完全不同：

| 值 | 含义 | 谁写的 |
|---|---|---|
| `explicit` | 人指定的（`subos new --runtime ...`） | 用户 |
| `index` | 创建时**向索引问出来**的 | 解析 |
| `fallback` | 索引答不出，用了编译期兜底常量 `DEFAULT_RUNTIME_FALLBACK` | 猜 |
| 键缺失 | 写这个块的 xlings 早于本字段 | —— |

**为什么单独记一个字段。** 兜底常量与索引的答案会有很长一段时间是**同一个值**，
于是「机制在工作」和「机制已死但常量恰好正确」在 manifest 里逐字节相同。这个字段
落地至今已经两次成为唯一的判据：一次是绑定值正确而解析从未发生（`fallback`），
一次是版本写错而错误信息只提到载荷目录名。

> ⭐ 一般规则：**新增一个「两个来源产出同一形状」的字段时，一并记下用了哪一个。**

`fallback` 不是错误 —— 离线机器、无网 CI 冷启动都会合法地落到这一档，且行为与
本字段存在之前完全一致。它只是在说：这个值没有被核实过。

#### 创建 vs 描述

`created_*` 与 `described_*` 记的是两件不同的事，写哪一对取决于那次运行在做什么：

- **创建**（`xlings subos new`、全新 home 的 `self init`）→ `created_*`
- **描述**（`self doctor --fix`、`xlings install` 补块、升级路径）→ `described_*`

重铸一个已有的块时，**真实的 `created_*` 会被带过去** —— 一次描述可以补上
`described_*`，但不该抹掉「这个 SubOS 曾经被谁在什么时候创建」。

（此前所有回填都写 `created_at`，于是同一台机器上两个不同的 SubOS 带着**逐字节
相同**的创建时间：那是它们被描述的同一秒，不是被创建的同一秒。）

**`runtime` 是创建期属性，默认值变更只影响新建 SubOS。** 绑定持久化在每个
SubOS 自己的 manifest 里；修复路径（`self doctor --fix`、块重铸）**保留合法的
已记录 binding**。**（2026.8.17.1+）** binding 缺失/畸形时不再直接落回默认值，
而是依次问：SubOS 的 workspace 记录的活跃 runtime → `lib*/libc.so.6` 符号链接
指向的载荷（唯一一个**观测**而非记录的来源）→ 都答不上来就**不写这个键**。
依赖解析侧由
pin-to-active 保证：已激活的 glibc 版本在满足约束时压过索引最新，所以已有
SubOS 不会因默认值或索引 `latest` 的变化被拉上新 runtime。

**创建路径上的默认值来自索引，不是常量（2026.8.27.2+）。** 创建一个 SubOS 时
先问索引这个运行时包的当前版本，问到就记 `runtime_source: index`；只有问不出来
才落到编译期常量 `DEFAULT_RUNTIME_FALLBACK` 并记 `fallback`。

**（2026.8.27.4+）`self init` 会先把索引变得可用，再决定。** 它创建的
`subos/default` 往往诞生在一台索引从未同步过的机器上 —— 那正是每个新用户的第一步。
若在那一刻只读盘，问必然答不出，于是常量会成为新用户**实际拿到**的值。所以这条
路径（也只有这条）在决定之前允许一次索引同步：

| 情形 | 行为 |
|---|---|
| 本地已有索引（不论新旧） | 直接用，**不联网** |
| 本地没有索引，可联网 | 同步一次，再解析 |
| 本地没有索引，且离线/同步失败 | 落到常量，记 `fallback` —— **与此前完全一致** |

其余创建入口（`subos new`、fork、rebuild）只读本地索引：它们跑得频繁，而那时索引
通常已经在了。**只有「答案即将被永久写下、而且此前没有任何机会取到索引」的那一处
值得一次网络往返。**

`envs` 的值由包在 `config()` 里通过 `subos.env{}` 写入（见 xim-pkgindex 的
xpackage-spec V2）。**值必须使用占位符** —— `${pkgdir}` / `${subosdir}` /
`${home}` / `${xlings_home}` —— 写死绝对路径会让这份描述只在写它的机器上成立。
占位符在进入 SubOS 时展开。

`envs` 为空时保留 `{}`,不省略键：缺失和空是两种写法、同一个意思,会让每个读者都要处理两遍。

进入 SubOS(`xlings subos use`)时这些变量被注入进程；**用户自己已 export 的值优先**,
`set` 不覆盖它,`prepend` 仍然与它拼接。`xlings self doctor` 检查这一段的完整性、
与已装包的一致性、占位符可解性,以及是否有多个包争抢同一变量。

#### `op` 的确切语义

只有两个 op。**`set` 是有条件的 —— 它并不"设置"**:

| `op` | 语义 |
|------|------|
| `set` | **仅当该变量不存在时**才导出。变量已存在(哪怕值为空)则原样保留 —— 用户的值赢。**不是覆盖** |
| `prepend` | 前置到已有值上,用平台的路径分隔符拼接;变量不存在时等于直接赋值 |

**这个名字骗过了人。** 一个叫 `set` 却在变量存在时什么都不做的 op,先后让
`xim-pkgindex#565` 的报告者和 `xlings#508` 的作者都判断错了方向 —— 后者据此
提了"缺少条件式 op"的 issue,而条件式一直就在那里。改名会让任何用新 op 名的
recipe 在所有已发布的旧客户端上直接安装失败(未知 op 在安装期即 `EnvDeclMalformed`),
所以名字保留,语义写在这里。

**"不存在" vs "值为空":空值算已设置,用户的值赢。**

`export FOO=` 是用户做出的选择,不是"没设置"。四个后端必须一致:

| 后端 | 生成/执行 |
|------|-----------|
| POSIX | `: "${FOO=value}"; export FOO;` —— **不是 `:=`**,冒号形式会连空值一起覆盖 |
| fish | `if not set -q FOO; set -gx FOO …; end` |
| pwsh | `if ($null -eq $env:FOO) { … }` —— **不是 `-not`**,`-not` 对空串为真 |
| 进程内 | `if (!utils::env_is_set(var))` —— **不是 `existing.empty()`**,后者分不出未设置与空 |

这一条此前四个后端里有三个是反的:只有 fish 保留空值,POSIX / pwsh / 进程内
都会覆盖掉它。同一个声明在不同 shell 下给出不同的环境,而且不出声。

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
