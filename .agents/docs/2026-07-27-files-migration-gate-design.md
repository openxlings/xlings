# 2026.7.27.1 —— 拆掉 `type="files"` 的兼容闸门，完成生态迁移

**日期**: 2026-07-27
**状态**: 设计，待实施
**前序**: [2026-07-27-sysroot-files-model-design.md](2026-07-27-sysroot-files-model-design.md)（能力设计，已随
2026.7.27.0 发布）、xim-pkgindex `docs/migrations/2026-07-27-files-assets-migration.md`（迁移规范，§2 的闸门本文推翻）

---

## 0. 一句话

`xvm.files` 是 libxpkg 的 **Lua 函数**，在老客户端上就是 `nil` —— recipe 里一句
`if xvm.files then` 即可自我分流。**不需要拆索引、不需要 `min_xlings`、不需要采纳期**，
迁移在本版内完成。

---

## 1. 起点：上一版的结论错了

迁移规范 §2 写着"现在还不能迁移共享索引"，理由是老客户端读到 `type = "files"` 硬失败：

```
error: unsupported registration node kind 'files'
       field: /nodes/3/kind
       nothing was changed
```

这个观测是对的（`src/core/xvm/registration.cppm:283` 的白名单 + `std::unexpected`，
整个注册事务中止）。**但从它推出的结论是错的** —— 它默认了"recipe 只能无条件声明
`files`"。

recipe 是 Lua。它可以先问一句这个客户端认不认识这个能力。

### 证据

| libxpkg | 随 xlings | `function M.files` |
|---|---|---|
| 0.0.46 | ≤ 0.4.69 | **0 处**（实测 `grep -c`） |
| 0.0.47 | ≥ 2026.7.27.0 | `src/xpkg-lua-stdlib.cppm:779` |

libxpkg 是**静态编译进 xlings 二进制**的，不是独立可升级组件。所以
"`xvm.files` 是不是函数"**精确等价于**"这个客户端支不支持 `type="files"`"。
没有版本号解析、没有服务端协商、没有可能对不上的两个真值来源。

### 为什么这比另外三个方案都好

| 方案 | 老客户端要改 | 新客户端要改 | 服务端要改 | 采纳期 | 长期成本 |
|---|---|---|---|---|---|
| **特性探测（本方案）** | ✗ | ✗ | ✗ | **无** | recipe 里一个 `if`，能力普及后删掉 |
| 指针拆 key（`xim-v2`） | ✗ | ✓ 要学会 key 回落 | ✓ 双份索引 | **一个发布周期** | 永久双份发布 |
| 索引加 `min_xlings` | ✗ | ✓ | ✓ | 一个发布周期 | 新增一套版本协商协议 |
| bump `format_version` | — | — | ✓ | — | **老客户端连索引都取不到**，禁止 |

指针拆 key 还有一处我上一轮漏看的坑：新客户端找不到 `xim-v2` 时
`select_manifest` 返回 `nullptr`，**不会回落到 `xim` 产物**，而是掉进
`repo.cppm:588` 的 git clone 兜底 —— 拉到的仍是同一份含 `files` 的 recipe，
闸门等于没有。这个方案的正确性依赖新客户端先改代码，成本比看上去高。

---

## 2. 兼容契约

写进 xpackage 规范，作为**所有**引入新 xvm 节点种类时的标准写法：

```lua
-- 契约：能力探测优先于版本判断。
-- 探测的是"这个客户端有没有这个函数"，不是"这个客户端版本号是多少"。
if xvm.files then
    xvm.files{ src = "include/openssl", dst = "usr/include/openssl" }
else
    -- legacy：保持迁移前的行为，一字不改
    sysroot.install_headers(includedir, get_sys_usr_includedir())
end
```

三条硬规则：

1. **不要用 `xvm.add{type="files"}` 做迁移。** `xvm.add` 在老客户端上存在，
   `type` 会被原样传下去，照样触发注册白名单硬失败。只有 `xvm.files` 这个
   **新增的函数名**才是有效探针。
2. **legacy 分支必须保持原样。** 它是老客户端的唯一路径，任何"顺手优化"都是
   在没有测试覆盖的地方改老用户的行为。
3. **`uninstall` 必须同样分流。** 新分支不再手工删文件（由 provider-scoped
   卸载负责），老分支必须保留手工删除，否则老客户端卸载后 sysroot 留垃圾。

### 探针何时可以删

当索引宣布不再支持 < 2026.7.27.0 的客户端时，`else` 分支连同 `if` 一起删除。
那时才需要 `min_xlings` —— 而那是一次**收缩**支持面的决定，可以从容做，不再
挡在功能交付前面。

---

## 3. 迁移语义

### 3.1 现状：sysroot 放置完全在版本库之外

索引里 26 个 recipe 碰 sysroot，其中 **9 个**真的往里放东西，两种写法：

```lua
-- 写法 A：整个 include 目录（openssl / python / libxml2 / freetype）
sysroot.install_headers(includedir, get_sys_usr_includedir())

-- 写法 B：逐个文件 os.cp（zlib / ca-certificates / linux-headers / glibc / gcc）
if os.isfile(zlib_h) then os.cp(zlib_h, sys_inc) end
```

两种都是 **`config()` 里的纯 Lua 副作用**，版本库里没有任何记录。后果：

- `use` 切版本时**头文件不跟着切** —— 装了两个 openssl，sysroot 里是最后装的那个
- `uninstall` 靠 recipe 手工 `os.tryrm` 对称清理，写漏就是泄漏
- `doctor` 看不见，因为它只看版本库

已确认**没有任何 recipe 向 `xvm.add` 传 `includedir`**（`grep` 全索引，
`includedir` 全是 Lua 局部变量）。所以迁移不会和 `group_header_assets` 的
`entry.includedir` 回落路径（`bindings.cppm:659`）撞车 —— 那条回落是给
历史版本库数据的，不是给 recipe 的。

### 3.2 目标写法

```lua
-- A → 一个 files 条目，dst 带前缀目录
xvm.files{ src = "include/openssl", dst = "usr/include/openssl", binding = tag }

-- B → 每个文件一个条目
xvm.files{ src = "include/zlib.h",  dst = "usr/include/zlib.h",  binding = tag }
xvm.files{ src = "include/zconf.h", dst = "usr/include/zconf.h", binding = tag }
```

`src` 相对 payload 根，`dst` 相对 subos 根，两端都必须相对
（`is_permitted_file_destination` 只放行首段为 `usr` / `etc` / `share` 的相对路径）。
物化走 `place_asset`（`commands.cppm:144`）：rename 原子替换 + equivalent 时跳过，
和库共用同一套替换纪律。

### 3.3 迁移清单

| recipe | 写法 | 条目数 | 备注 |
|---|---|---|---|
| `openssl` | A | 1 | `include/openssl` 整目录 |
| `python` | A | 1 | 仅在 sysroot 存在时 |
| `libxml2` | A | 1 | |
| `freetype` | A | 1 | |
| `zlib` | B | 2 | `zlib.h` + `zconf.h` |
| `ca-certificates` | B | 2 | `etc/` 目标 |
| `linux-headers` | B | 1 | |
| `glibc` | B | 1 | **D 类，需单独实测**（见 §6） |
| `gcc` | B | 1 | 工具链根，切换影响面最大 |

其余 17 个碰 sysroot 的 recipe 只**读**不写，不迁。

---

## 4. 过渡态：老用户升级后会发生什么

这是本版"无感升级"的实质内容。四种组合：

| 装包时的客户端 | 现在的客户端 | sysroot 里的文件 | `use` 能切吗 | 处置 |
|---|---|---|---|---|
| 新 | 新 | 已注册 `files` 节点 | ✅ | 正常 |
| 老 | 老 | 手工拷贝，无记录 | ❌（本来就不能） | 无变化，不是回退 |
| **老** | **新** | **手工拷贝，无记录** | **❌** | **← B2 要解决的** |
| 新 | 老（降级） | 有记录但客户端不认 | ❌ | 不支持，明确不管 |

第三行是唯一的真问题：用户升级了 xlings，但**已装的包是老客户端装的**，它们的
sysroot 文件不在版本库里。此时：

- `use openssl 3.1.5` 会切 bin 和 lib，**头文件纹丝不动** —— 一个比"完全不能切"
  更糟的状态，因为它看起来成功了
- `doctor` 完全看不见

**这不是迁移引入的**（老客户端本来也不能切头文件），但**升级后它变成了不一致**：
同一个包，新装的版本能切、老装的版本不能切。

### B2 的处置

`doctor` 增加一条检查：**payload 里有 include/ 但版本库里这个版本没有任何 `files`
节点，而同一 target 的另一个版本有** → 报 `xvm-sysroot-untracked`，并给出精确指令：

```
ⓘ openssl@3.0.13 的 sysroot 文件早于文件跟踪，切换版本时不会跟随
  修复: xlings install openssl@3.0.13     # 重跑 config()，补登记
```

**为什么是提示而不是 `--fix` 自动修**：补登记需要重跑 recipe 的 `config()`，
那是任意 Lua 代码，可能下载、可能改 payload。`doctor --fix` 的契约是"只做不需要
猜测的修复"（这正是 #412 剪悬空边能自动做、而"发布内部不一致"不能的分界）。
重跑第三方代码不符合这个契约。

---

## 5. `spec` 是 fail-open 的 —— 独立缺陷（B3）

全树只有一处比较：

```cpp
if (pkg->spec == "2" && !pkg->archs.empty()) { ... }   // installer.cppm:1765
```

libxpkg 那边 `p.spec = get_str(...)` 是**裸字符串拷贝，无任何校验**。于是：

- `spec = "3"`（未来） → 比较为 false → **静默按 V1 装**，无报错无警告
- 老客户端读 `spec = "2"` → 同样跳过 arch 门禁

第二条是实打实的隐患：V2 CHANGELOG 写着 "Requires xlings ≥ 0.4.61"，但低于该版本的
客户端不是拒装，而是**跳过 arch 校验按 V1 装** —— 把一个 fail-closed 的安全门禁
降级成静默装错架构。那句 "requires" 在机器上没有任何约束力。

### 处置

引入客户端支持上限常量（当前 `2`），超出则**拒绝该包并明确报错**：

```
✗ foo: xpackage spec "3" is newer than this xlings supports (max 2)
  升级: xlings self update
```

**per-package skip，不中止整个事务** —— 与既有的 arch 门禁（`installer.cppm:937`
的 `log::error` + `continue`）保持一致的失败粒度。

注意这**不能**修复已发布客户端的 fail-open（它们已经在外面了），它设定的是
**从本版往后**的地板。这正是 `spec` 本该有、而 `type="files"` 不得不用特性探测
绕过去的东西 —— 记录在案，作为下一次 spec 演进的前提。

现状盘点：118 个 recipe 全部声明 spec，112 个 `"1"`，6 个 `"2"`，无其它值。

---

## 6. P1.3b：元数据损坏（B1）—— 唯一还留给用户的死路

`~/.xlings.json` 中某条目的 `bindingGroup` 畸形时，`use` 拒绝、`doctor --fix` 修不了，
用户只能手工编辑 `versions.json`。0.4.70 的验收项 A9 因此只算部分满足。

**上次为什么没做**：按计划停止序列化 `bindingIntegrityIssues` 后，#384 的 7 条测试挂了，
**而它们是对的** —— 一条 group 没解析成功的记录，内存里不持有那个 group，不写回
marker 就等于保存时把畸形值整个丢掉，损坏条目被**静默洗白**成健康的无组条目。
比不修更糟：用户从"报错但看得见"变成"没报错但状态错了"。

**正确修法**：为未解析成功的条目**保留原始 JSON**，使 round-trip 无损。有了无损
round-trip，`--reset-metadata` 才能安全地"丢弃 group 元数据、降级为 legacy singleton"，
因为此时"丢弃"是一个显式动作，而不是一次序列化事故。

验收：#384 的 7 条测试**不加修改继续通过**（它们守的是"不许静默丢信息"，
这个约束在新方案下依然成立），另加损坏 → `--fix --reset-metadata` → 可用的
端到端用例。

---

## 7. 发布可靠性（C）

2026.7.27.0 发布时 workflow 报 7/7 success，其中两个 job 什么都没做：

| job | 报告 | 实际 | 后果 |
|---|---|---|---|
| `bump-index` | ✅ | 索引没 bump，`latest` 停在 0.4.69 | **没有用户能 `self update`** |
| `mirror-binaries` | ✅ | 建了 release 但没传产物 | CN 用户 404 |

根因两层：

1. **竞态** —— 两者都只 `needs: [create-release]`，并行跑。`version-check.py`
   写入前要先下载 xlings-res 产物校验 sha256，镜像没传完就没东西可校验，
   `bump_index.sh` 走 "no change" 分支 `exit 0`
2. **静默** —— 外层 `|| echo "... (non-blocking)"` 把非零退出变成绿勾

处置：`bump-index` 加 `needs: [mirror-binaries]`；两处 `|| echo` 保留"不阻塞发布"
的语义，但**必须让 job 结论反映真实结果**（写 summary + 非零 outcome），
否则下次还是靠人肉核对才能发现。

---

## 8. 明确不做的

| 项 | 理由 |
|---|---|
| 索引拆 key / `min_xlings` | §1 表格：特性探测全面占优。收缩支持面时再做 |
| `format_version` bump | fail-closed，老客户端连索引都取不到 |
| 能力表 `xpkg.capabilities` | 新增能力表本身也要探测才能用，等于多一层间接。`if xvm.files then` 已经够 |
| 统一三条物化路径 | 正确性已由 `active` 门禁达成并被 E2E 覆盖；纯结构收益，风险不对等 |
| Materialization ledger | B2 用 doctor 提示覆盖了用户可见的那部分。ledger 是更大的独立改动 |
| P2 group 归一化 | 与本版无关，继续留 0.4.71 |

---

## 9. 任务拓扑

```
A 设计文档（本文）
        │
        ├──────────────┬──────────────┬─────────────┐
        ▼              ▼              ▼             ▼
   B1 P1.3b       B2 sysroot     B3 spec        C release.yml
   元数据恢复      未跟踪检测      闸门            竞态+静默
        │              │              │             │
        └──────────────┴──────┬───────┴─────────────┘
                              ▼
                    E1 一个 PR + CI 全绿 + 合入
                              │
                              ▼
                    E2 发布 2026.7.27.1 + 四仓链
                              │
                              ▼
        ┌─────────────────────┴─────────────────────┐
        ▼                                           ▼
   D1 迁移 9 个 recipe                        D2 规范/迁移文档
   （duck-typed，逐包实测）                    （§2 闸门改写为契约）
```

**D 必须在 E2 之后**：虽然特性探测让 recipe 在老客户端上安全，但迁移后的 recipe
要在**真实发布过的**新客户端上验证，而不是本地构建产物。这和上一版
"xlings 先发、索引后迁"的拓扑一致 —— 变的是 D 不再需要等采纳期。

**本版是 xlings + 索引两仓，不涉及 libxpkg** —— `xvm.files` 0.0.47 已发布且够用，
不需要新字段。四仓链缩短为两仓。

### 验收

| # | 条件 |
|---|---|
| A1 | 老客户端（0.4.69 二进制）装迁移后的 recipe：**成功**，走 legacy 分支，行为与迁移前逐字节一致 |
| A2 | 新客户端装同一 recipe：走 `files` 分支，`use` 切版本时**头文件跟着切** |
| A3 | 老客户端装 → 升级 xlings → `doctor` 报 `xvm-sysroot-untracked` 并给出精确 install 指令 |
| A4 | 畸形 `bindingGroup` → `doctor --fix --reset-metadata` → 恢复可用；#384 的 7 条测试不改照过 |
| A5 | `spec = "3"` 的 fixture → 拒绝该包 + 明确报错，**其余包继续装** |
| A6 | 隔离环境全程：`env -i` + 临时 `HOME`/`XLINGS_HOME`，绝不碰宿主机 xlings home |
| A7 | 发布后用 `verify-ecosystem.sh` 核对产物/镜像/索引，**不看 workflow 颜色** |

---

## 10. 风险

| 风险 | 处置 |
|---|---|
| 迁移后的 recipe 在老客户端上走错分支 | A1 用**真实的 0.4.69 二进制**验，不是靠读代码 |
| `place_asset` 对**目录**（写法 A）的替换语义与文件不同 | 迁移前先针对目录 dst 补单测 + E2E |
| `glibc` / `gcc` 切换影响整个工具链 | 这两个放迁移队列**最后**，各自单独一个 PR，前面 7 个跑通再动 |
| B1 再次挂 #384 的测试 | 那 7 条是**验收条件**不是障碍：改动必须让它们不修改而通过 |
| Windows 路径分隔符 | 断言一律用 `std::filesystem::path` 构造，不写字面量 `/`（已两次踩坑） |
