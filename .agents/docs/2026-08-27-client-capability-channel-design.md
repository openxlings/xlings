# 客户端能力通道:让 recipe 能问"这个客户端会做什么"

> 状态:**已否决,不实现**(2026-08-27)。决定与依据见文末 §9。
> 保留这份文档是因为它记录了**为什么不做**,以及什么条件会让这个判断翻过来 ——
> 下一次有人问"要不要给 recipe 一个能力通道",答案和量化依据在这里。
> 触发:#423 修好之后,索引里 4 个 recipe 的手写清理在新客户端上成了死代码,
> 而 recipe **没有任何办法**知道自己跑在哪个客户端上。
> 上游:`2026-08-26-declared-file-assets-removal-design.md`、
> `2026-08-26-release-2026.8.26.1-notes.md`。
> 代码位置对应 xlings `148526e` / libxpkg `0.0.57` / xim-pkgindex `7a02d947`。

---

## 0. 一句话

要分开两件事,混在一起就会把一个**过期品**固化进 API:

| | 性质 | 寿命 |
|---|---|---|
| **通道** —— xlings 告诉 recipe 它能做什么 | **通用**,现在完全没有 | 永久 |
| **第一个名字** `files-reclaim` | **workaround**,标记的是"某个 bug 修没修" | 下限越过 2026.8.26.1 就删 |

只加名字不加通道 = 每修一个 bug 加一个布尔字段,发一次 libxpkg。
只加通道不加名字 = 通道没有第一个客户,不知道它对不对。

---

## 1. 现在 recipe 手上有什么(实测)

`_RUNTIME` 里 C++ 侧注入的全部字段(`xpkg-executor.cppm:632-712`):

```
pkg_name  version  platform  arch  install_file  install_dir  run_dir
xpkg_dir  bin_dir  project_data_dir  subos_sysrootdir  pkgindex_dir
deps_list  runtime_deps_list  build_deps_list  dependency_store_roots
deps_exports  resolved_deps  self_exports
```

`version` 是**包自己的版本**。**没有客户端版本,没有任何能力标记。**

### `xvm.files` 能当探针是碰巧

```lua
if not xvm.files then return false end        -- sysroot.lua:86
```

它成立只因为 `xvm.files` 恰好是个**函数**,缺了就是 nil。任何不是函数的
能力——"卸载会不会回收"、"某个字段会不会被读"、"某种节点类型认不认"——
都**没法这样问**。

`docs/V2/xpackage-spec.md` 写的是 "probe capability, never version",
但只提供了函数这一种可探测形状。#423 是第二次撞上,第一次是 `xvm.files` 自己。

### 唯一的替代路子,以及为什么不走

`system.exec("xlings --version")` 能拿到版本。不走,三个理由:

1. 与 spec 直接冲突("never version")。
2. 卸载时 PATH 里未必有那个 shim,subos 里未必解析得到 —— 一次 fork 换一个
   不可靠的答案。
3. 版本比较要在 Lua 里做四段日期号(`2026.8.26.1` vs `2026.8.22.4`),
   每个 recipe 抄一份。

---

## 2. 设计

### 2.1 通道:一组字符串,不是一个字段一个布尔

```cpp
// libxpkg: xpkg-executor.cppm, struct ExecutionContext
std::vector<std::string> capabilities;
```

```
_RUNTIME.capabilities = { "files-reclaim", ... }
```

```lua
-- libxpkg: src/lua-stdlib/xim/libxpkg/system.lua
function M.supports(name)
    for _, c in ipairs((_RUNTIME and _RUNTIME.capabilities) or {}) do
        if c == name then return true end
    end
    return false
end
```

**为什么是字符串集合而不是布尔字段。** 加一个布尔字段要发一次 libxpkg
(定义字段)+ 一次 xlings(填它)。加一个字符串**只改 xlings** ——
`ExecutionContext` 是个由 xlings 填的普通 struct,libxpkg 只定义一次通道。
这条差别就是"通用"和"每次都得走三个仓库"的分界。

### 2.2 为什么放在 `system` 而不是 `xvm`,以及为什么不叫 `has`

- **能力是客户端的属性**,不是版本管理器的。`system.*` 已经是读 `_RUNTIME`
  的地方(`rundir` / `bindir` / `subos_sysrootdir` 全在那)。放 `xvm` 会暗示
  它只覆盖 xvm 的事,而第一个名字之后大概率会有非 xvm 的能力。
- `xvm.has("…")` 在这个命名空间里读起来像**"有没有这个 target"** ——
  `xvm` 的其余 API 全是关于 target 和版本的。
- `system.supports("files-reclaim")` 主谓宾明确:谁支持、支持什么。

### 2.3 由 xlings 声明,不是 libxpkg 自称

libxpkg 只提供通道;**名字由 xlings push**。

"卸载会不会回收"是**二进制的属性**。libxpkg 是被链进去的库 ——
如果它在 `xvm.lua` 里写个常量自称支持,那么一个老 xlings 链上新 libxpkg
时这个常量就在说谎。让 xlings 填,说谎就不可能。

### 2.4 **一个写入者** —— 本方案最容易出事的地方

xlings 里有 **4 处**各自 `ExecutionContext ctx;` 然后逐字段填:

| 位置 | 干什么 |
|---|---|
| `src/core/xim/installer.cpp:2384` | 安装(config/install 钩子) |
| `src/core/xim/installer.cpp:3240` | **卸载**(uninstall 钩子) |
| `src/core/cmdprocessor.cpp:275` | `xlings run <script>` |
| `src/cli.cpp:1533` | 同上,另一条入口 |

其中 `subos_sysrootdir` 有两种填法:前两处走
`configure_xpkg_execution_artifact_paths_`(`installer.cpp:796`),
后两处**各自内联写一遍**。

**这正是这个代码库反复被咬的形状。** 如果能力只在 3 处填、漏了第 4 处,
那第 4 处跑的 recipe 会看到"客户端不支持",于是走回退分支 ——
**多做一次清理,不报错,输出一样**。又一次"没发生过和成功了长得一样"。

所以本方案**不是**"在 4 个地方加一行",而是:

```cpp
// xlings 侧,唯一入口
mcpplibs::xpkg::ExecutionContext xim::make_execution_context();
```

4 处全部改成用它;能力列表在它里面 push 一次。谁再加第 5 处入口,
只要用这个工厂就自动带上。**能力不能有第二个写入者。**

### 2.5 探针必须写成 `type(...) == "function"`

老客户端上 `system.supports` 是 nil,`if system.supports("x")` 会
**报错**(attempt to call a nil value),不是返回假。

而且 loader 沙箱(解析 `package = {...}` 用的那个,`xpkg-loader.cppm:42`)
的 `import()` 返回一个**带 metatable 的 proxy**,索引它什么都是真值 ——
`if system.supports then` 在那里恒真。钩子不在 loader 里跑,但这个陷阱
在这个仓库出现过(`subos.env` 那次)。

所以 recipe 侧唯一正确的写法:

```lua
local function client_reclaims()
    return type(system.supports) == "function"
       and system.supports("files-reclaim")
end
```

这段**放进 `libs/sysroot.lua`**,recipe 只调它,不各写一遍。

---

## 3. 第一个名字:`files-reclaim`,以及它的到期日

**含义(要写死在 spec 里,否则语义会漂):**

> 客户端在**每一条交出发布的路径**上回收 `xvm.files` 声明的资产:
> 完整卸载、detach、重新注册后不再声明的目的地、`use` 切到资产集更小的版本。

xlings ≥ 2026.8.26.1 push 它。实测依据(glib,274 头 + 5 pc + 15 lib,
单包 subos,`uninstall()` 里不做任何手工清理):

```
2026.8.26.1   卸后 0 条      完整卸载 294→0,detach 本 subos 294→0 且另一个 subos 完好
2026.8.22.4   卸后 279 条
```

**到期条件,必须和名字写在一起:**

> 最低支持客户端越过 2026.8.26.1 之后,删掉这个名字、删掉 `system.supports`
> 对它的所有调用、删掉 4 处被 guard 的清理。
> 删之前重跑上面那个测量,**用 `find -xtype l`,不要用 `[ -e ]`**。

一个注定永远为真的标记不是能力,是穿了能力外衣的版本号。不写到期条件,
下一个人会把它当永久 API。

---

## 4. 索引侧

`libs/sysroot.lua` 加一个共享谓词(不是 4 份拷贝):

```lua
function sysroot.client_reclaims_files()
    return type(system.supports) == "function"
       and system.supports("files-reclaim")
end
```

4 个**无条件**清理的 recipe 各包一层:

| recipe | 现在清理的东西 |
|---|---|
| `glib` | `usr/include/glib-2.0` + 5 个 `.pc` |
| `freetype` | `usr/include/freetype2` + `freetype2.pc` |
| `libselinux` | `usr/include/selinux` + `libselinux.pc` |
| `util-linux` | `libmount`/`blkid`/`uuid` + 3 个 `.pc` |

```lua
if not sysroot.client_reclaims_files() then
    <现在这段>
end
```

另外 4 个(`libxml2` `openssl` `ca-certificates` `zlib`)是
`if not xvm.files` 门 —— **那是 pre-2026.7.27.0 的回退,不是 #423 的补丁**,
不动。(它们在 2026.7.27.0 → 2026.8.22.4 之间一直在泄漏,因为它们信了
"随发布回收"这句当时不成立的话;#693 已经把注释改对。)

---

## 5. 发布顺序与判据

| 步 | 仓库 | 内容 | 判据 |
|---|---|---|---|
| 1 | libxpkg | `ExecutionContext::capabilities` + 注入 `_RUNTIME` + `system.supports` | 单测:注入了名字 → `supports` 为真;没注入 → 为假且**不报错** |
| 2 | libxpkg | 发版 0.0.58 | — |
| 3 | xlings | `make_execution_context()` 工厂,4 处收敛;push `"files-reclaim"`;bump `xpkg = "0.0.58"` | e2e:**四条入口**跑的 recipe 都看得到能力(见下) |
| 4 | xlings | 发版 2026.8.27.1 | 8 assets + sha256 + 索引 latest + GitCode 镜像 |
| 5 | xim-pkgindex | `sysroot.client_reclaims_files()` + 4 处 guard | 见下 |

### 判据,逐条可证伪

```
A. 老客户端(2026.8.22.4)+ 新索引:装 glib → 卸 → 剩余 0
   (走回退分支;这条一旦破,就是老用户开始泄漏)
B. 新客户端(2026.8.27.1)+ 新索引:装 glib → 卸 → 剩余 0
   且那段 rm 未执行(用 log 或在 guard 里放一次 debug 输出确认,
   不能只看结果 —— 结果一样正是这个 bug 家族的特征)
C. 四条 ExecutionContext 入口都带能力:安装 / 卸载 / `xlings run` 两条
   (漏一处的表现是"多清理一次",无声,所以必须逐条测)
D. 探针在老客户端上不报错:`type(system.supports) == "function"` 为假
```

**B 的第二半是这份方案里最需要认真对待的一条。** 有没有跳过那段 `rm`,
从结果上看不出来 —— 两边都是 0。必须有独立证据。

---

## 6. 风险

- **漏填第 4 个入口。** 缓解:工厂(§2.4)。这不是纪律问题,是设计问题 ——
  现在这 4 处已经在 `subos_sysrootdir` 上分成了两种填法。
- **能力名义漂移。** `files-reclaim` 说的是"四条路径都回收"。如果将来某条
  路径回归了,标记还在,recipe 就会跳过本该做的清理。缓解:名字的定义写进
  spec,并且 E2E-94(已有)就是那四条路径的守卫。
- **老 xlings 链新 libxpkg。** 由 §2.3 结构性排除(名字由 xlings push)。
- **三仓库发布链断在中间 —— 这条是安全的,而且是设计出来的。**
  谓词在任何缺失情况下都返回**假**:`system.supports` 不存在返回假,
  存在但列表里没这个名字也返回假。假 = 走回退 = 照旧清理。
  所以无论"先合索引"还是"先发 xlings",中间态都是**保守清理**,不是泄漏。
  **未知一律按"客户端不会回收"处理** —— 这是这份方案里唯一一处
  必须钉死的默认值,写反了(未知按"会回收"处理)就变成静默泄漏。

---

## 7. 不做什么

- **不做版本判断。** 沙箱里没有客户端版本;`system.exec("xlings --version")`
  与 spec 冲突且不可靠(§1)。
- **不给每个能力加一个布尔字段。** 那样每次都要发 libxpkg(§2.1)。
- **不动那 4 个 `if not xvm.files` 的 recipe。** 那是另一条时间线的回退。
- **不删任何清理代码。** 本方案只让新客户端**跳过**;删除是下限到达后的
  另一次改动,判据已写在 §3。

---

## 8. 值得先问的一个问题

如果 #423 是最后一次"客户端行为变了、索引要兼容一段",那么今天合进去的状态
(无条件 `rm` + 注释写清删除条件)本身就是对的:每个客户端上行为都正确,
新客户端只多跑一段无害的死代码。**这份方案的收益是:**

1. 新客户端不再对**共享目录**做 `rm -rf`(`usr/include/glib-2.0` 是逐文件
   声明的,粗粒度删除是 X11 那类冲突的同款隐患);
2. `xvm.files` 从"靠注释描述的机制"变成**自述的机制**;
3. 下一次同类问题不用把这段讨论重来一遍。

**代价是三个仓库一次发布链,以及一个注定要删的名字。**
如果判断 1/2/3 不值这个代价,不做也是一个站得住的决定 ——
那就把 §3 的到期条件当成唯一的行动项,等下限到了直接删清理代码。


---

## 9. 决定:不做通道,改为直接删掉手写清理

### 9.1 压垮它的那条实测

§8 说这份方案的第一条收益是"新客户端不再对**共享目录**做 `rm -rf`"。
量了一下,**对这 4 个包是零** —— 它们 `rm -rf` 的 6 个目录,今天没有任何
别的包往里放东西:

```
usr/include/glib-2.0     ['xim:glib']
usr/include/freetype2    ['xim:freetype']
usr/include/selinux      ['xim:libselinux']
usr/include/libmount     ['xim:util-linux']
usr/include/blkid        ['xim:util-linux']
usr/include/uuid         ['xim:util-linux']
```

(`.pc` 同理:全索引里同一路径被多个 provider 声明的只有一处,
就是已经修掉的 `usr/include/scsi`。)

这些目录名是包自己独占的命名空间,不像 `X11/` 或 `scsi/`。所以收益 1 归零,
剩下的收益 2、3 都以"**还有下一次**"为前提 —— 而那是假设,不是事实。

### 9.2 实际选的路:直接删

xim-pkgindex#694 把那 4 处无条件清理删掉了。理由是项目现状:
**用户量小、处于早期、升级频繁**,可以默认用户在最新版本上。

老客户端的代价量过,并且**不是 break**:

- 悬空链接不会被编译器选中,原本能用的东西没有一样停止工作,重装直接覆盖;
- 升级后可恢复且**会自己报出来** —— 2026.8.26.1 的 doctor 看得见
  (旧的只扫一层深,实测报 0),`--fix` 归零;
- 这条防线本来就不完整:39 个声明式资产 recipe 里只有 8 个有手写清理,
  X11/graphics 栈那 26 个一个都没有。删掉只是把既有情况从 31 个包扩到 35 个,
  不是引入新的故障类别。

### 9.3 什么条件会让这个判断翻过来

**出现第三次**"客户端行为变了、索引要兼容一段"。那时候通道有两个真实客户
(`xvm.files` 是第一个,`files-reclaim` 是第二个),再建它就有依据,
而不是为一个假设付三个仓库的发布链。

在那之前,§2 的设计(字符串集合 / `system.supports` / 由 xlings 声明 /
**一个写入者的工厂** / `type(...) == "function"` 探针)原样保留 ——
真要建的时候,这些结论不用重新推一遍。§2.4 那条(4 个
`ExecutionContext` 建造点必须收敛)与通道无关也成立,它本身就是一个
待办:现在 `subos_sysrootdir` 已经在那 4 处分成了两种填法。
