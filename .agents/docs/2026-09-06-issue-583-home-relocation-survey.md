# issue #583 排查 —— home 搬家之后,谁在说谎

> 起点:[#583](https://github.com/openxlings/xlings/issues/583),报告人 @yspbwx2010。
> 报告人测的是 **2026.8.30.2** 且声明「未在 2026.9.3.2 复测」。
> 本次基线 `a5cf36e`(发布版 **2026.9.3.2**),用 `~/.xlings/bin/xlings`(= 2026.9.3.2)
> 在隔离 home 上**全部复现**,并且实测结果比报告里写的更严重。
> 状态:**仅排查,未改代码**。

## 0. 一句话

搬家本身不是 bug —— **记录里 100% 是绝对路径**才是。
而 `self doctor --fix` 会把一个「东西全在、只有路径过期」的 home,
变成一个「包被注销、shim 被删、头文件农场被删、库农场仍然坏着」的 home,
然后打印 **`status OK — workspace, shims, and payloads are all consistent`,exit 0**。

| 真实问题 | 实际回答的代理问题 | 后果 |
|---|---|---|
| 这个 payload 还在吗 | 记录里那条**绝对路径**存在吗 | 载荷就在新根下 → 判「gone」→ 注销 86 个包 |
| 这个 home 搬过家吗 | (没有任何代码问过) | 四种形态无人重写 |
| subos 里有没有断链 | `usr`/`etc`/`share` 下有没有断链 | `<subos>/lib` 从不扫描;**没搬过家**的真实 home 现存 6 条断链,doctor 报 0 |
| `--fix` 之后 home 好了吗 | 剩余 finding 数是不是 0 | 把问题删掉也算 0 → `OK` + exit 0 |

同一形状 memory 里已经有名字:[[silent-success pattern]]、[[proxy predicate, discarded half]]、
[[reporter/repairer predicate drift]]。这次的新意在于 **prune 的注释把安全性论证写得很完整,
但那个论证有一个未声明的前提**(见 §3)。

---

## 1. 复现(2026.9.3.2,全新隔离 home)

构造一个最小 home,里面按各自**真实写者的写法**放四类链接,然后 `mv`,再 `--fix`。
完整脚本见 §附录,可直接重跑。

```
--- 搬家前 ---
subos/default/lib/libdemo.so           resolves
subos/default/usr/include/demo.h       resolves
subos/default/bin/demo                 resolves

--- mv homeC homeD 之后,--fix 之前 ---
subos/default/lib/libdemo.so           DANGLING
subos/default/usr/include/demo.h       DANGLING
subos/default/bin/demo                 resolves      ← shim 是相对链接,活下来了
```

`XLINGS_HOME=<新根> xlings self doctor`(不带 `--fix`)确实报了:

```
✗ broken payload [active]  demo@1.0.0 path <旧根>/data/xpkgs/xim-x-demo/1.0.0 missing
  • no remedy              no package in any index provides this entry; `--fix` will drop the registration
```

然后 `--fix`:

```
· dangling link removed  @xlings/subos/default/usr/include/demo.h
· dropped                demo@1.0.0 — its payload is gone and nothing can restore it
· stale shims removed    demo
▸ status                 OK — workspace, shims, and payloads are all consistent
▸ healed                 1
▸ pruned                 1
EXIT=0
```

事后实测:

| 对象 | `--fix` 之后 | 事实 |
|---|---|---|
| `subos/default/lib/libdemo.so` | **仍然 DANGLING** | 从未被报告、从未被扫描 |
| `subos/default/usr/include/demo.h` | **已删除** | 重装才能恢复 |
| `subos/default/bin/demo` | **已删除** | 相对 shim 本来是好的,因为注册被 prune 了才连带删 |
| `versions` / `workspace` | **`{}` / `{}`** | 注册全没了 |
| `data/xpkgs/xim-x-demo/1.0.0/{bin,lib}` | **完好** | 载荷一直在新根下 |
| 退出码 | **0** | 并且 `status OK` |

> 也就是说:`--fix` 之前这个 home 只要改路径就能救;`--fix` 之后要重装。
> 而且它**不返回非零**,CI / 脚本无从察觉。

---

## 2. 四种形态,逐个定位到写者(真实 home 实测)

测量对象是我这台的 `~/.xlings`(未搬家,1865 个 target / 2805 条版本记录 / 94 个 subos)。

| # | 形态 | 写者 | 形式 | 真实 home 实测 | doctor 看得见吗 |
|---|---|---|---|---|---|
| 1 | versions DB `path` | `xvm/registration.cpp:787` `data.path = node.path`(recipe 传进来的 `install_dir`) | **绝对** | `path` 2798 条含 home 绝对路径;alias 26;envs 19;**`${XLINGS_HOME}` 出现 0 次** | 看得见,而且**会打印旧路径**,但结论是「gone」 |
| 2 | sysroot 库农场 | `xim/installer.cpp:2084` `create_symlink(source, dst)`,`dst` 在 `<subos>/lib`(`installer.cpp:1764`,`Config::libDir` = `subosDir/"lib"`,`config.cpp:484`) | **绝对** | `subos/default/lib` 431 条链接,**431 条绝对**;全部 subos 合计 3042 条 | **完全看不见**(见下) |
| 3 | ELF `PT_INTERP` + `RPATH/RUNPATH` | 索引侧 `elfpatch`(`installer.cpp:2966` `apply_elfpatch_auto()`) | **绝对** | 抽样 88 个 payload 可执行文件:**85 个 PT_INTERP 指向 home、85 个 RPATH 指向 home、0 个用 `$ORIGIN`** | elfcheck 判的是「loader 与 libc 同源」,不是「路径还在不在」 |
| 4 | 文本 | glibc 载荷自带的 linker script / `bin/ldd` 的 `TEXTDOMAINDIR`;`installer.cpp:3012` 写的 `.xlings-resolution.json` | **绝对** | — | 无人扫描 |

### 形态 2 是一个独立缺陷,与搬家无关

断链扫描在 `doctor.cpp:2069`:

```cpp
for (const auto& scanRoot : sysrootRoots) {
    for (const auto& sub : {"usr", "etc", "share"}) {   // ← 没有 lib / lib64
```

上面那段注释很详细地解释了「为什么从四个一级目录改成整棵树」,但**改的是深度,不是广度**:
`<subos>/lib` 和 `<subos>/lib64` 从头到尾不在集合里,`doctor.cpp` 全文没有出现过 `libDir`。

实测(这台机器,**从未搬过家**):

```
links under subos/*/lib*: 3042   dangling right now: 6
  default/lib/libfreetype.so.6 -> .../xim-x-freetype/2.13.2/lib/x86_64-linux-musl/libfreetype.so.6
  ...
xlings self doctor  →  dangling 相关 finding: 0 条
```

所以报告人说的「51 条 lib 链接 `--fix` 之后还是 51 条」,真因不是「repair 不重指」,
而是**它们根本没进过 detect**。这条应该单独开 issue。

### 形态 4 可以降级

`.xlings-resolution.json` 只有一个消费者:`xim/commands.cpp:246`,一个**展示**命令。
它过期只会打印错的路径,不影响任何功能。报告里把它和另外三种并列,权重偏高了。

---

## 3. 为什么 `--fix` 会毁掉一个可修复的 home

链路四步,每一步单独看都合理:

1. **detect** — `doctor.cpp:1333` 「L4: payload directory must exist」:
   `fs::is_directory(xvm::expand_path(vdata.path, homeStr))`。
   记录里是旧根的绝对路径,`expand_path` 没有占位符可展开,原样返回 → 不存在 → `BrokenPayload`。
2. **ladder** — `repair_payloads_` 尝试重装。搬家的 home 上这意味着**把整个 home 重新下载一遍**;
   任何一个装不回来的(离线、索引里没有、local: 来源)就留在 findings 里。
3. **prune** — `doctor.cpp:2965` `prune_dead_registrations_`,注释写得很完整:

   > *the payload is gone, or present with nothing runnable in it… There is nothing to lose that has not already been lost.*
   > *the repair ladder ran and the finding is still there.*

   两个条件都成立,但它们共享一个**未声明的前提**:「payload is gone」是从
   *记录里的那条路径* 推出来的。搬家之后这个前提为假 —— 载荷就在 `<新根>/data/xpkgs/<store>/<version>`,
   一次 `is_directory` 就能证伪。谓词问的是「这条字符串指向的目录在不在」,
   当成了「这个包的载荷在不在」。
4. **报告** — `count_` 归零 → `after.issues() == 0` → `status OK`,exit 0。
   删掉问题和解决问题在输出上不可区分。

> 附带一个观察:8 次 `deep audit pass` 跑在一个只有 1 个 payload 的 home 上
> (`1 payload(s) examined, 1 unchanged since the last pass` × 7)。功能上无害,但值得看一眼。

---

## 4. 第二个 bug:只读 home 上 `--fix` 直接 SIGABRT(已定位到行)

报告人只写了现象。实测复现并用 gdb 拿到了栈:

```
$ chmod -R a-w <home>; XLINGS_HOME=<home> xlings self doctor --fix
... 完整报告正常打印 ...
terminate called after throwing an instance of 'std::runtime_error'
  what():  Failed to write file: <home>/.xlings.json
EXIT=134   (SIGABRT, dumped core)
```

```
#5  platform::write_file_atomic        modules/platform/src/platform.cpp:403
#6  platform::write_string_to_file     modules/platform/src/platform.cpp:436
#7  Config::record_client_version      src/core/config.cpp:1303
#8  xself::cmd_doctor                  src/core/xself/doctor.cpp:4048
#9  xself::run                         src/core/xself.cpp:182
#10 cli::run                           src/cli.cpp:1525
#11 main                               src/main.cpp:92
```

**两个独立根因:**

1. **写入无保护,而且和邻居不一致。**
   `Config::record_client_version`(`config.cpp:1286–1304`)读的时候有 `try/catch`,
   写的时候没有。同一次 `--fix` 里,`doctor.cpp:2343` 写 subos manifest 的那处**是**包了
   `try/catch` 的,所以同一个只读 home 上它优雅地报了
   `✗ subos manifest could not write …`。同一个失败,两种命运。
2. **顶层 catch 覆盖不到 `self`。**
   `cli.cpp:1804` 那段注释说它接住「any uncaught std::exception … Convert to a logged error」,
   但 `try` 从 `cli.cpp:1812` 才开始,而 `self` / `subos` / `profile` 在
   **`cli.cpp:1525` 就提前分派了**,在 `try` 外面。
   于是这三棵子树里任何逃逸异常都是 `std::terminate`,不是那条友好的 `internal error:`。

**触发条件实测**(同一个只读 home):

| 命令 | 结果 |
|---|---|
| `self doctor` | exit 1,正常 |
| `self doctor --deep` | exit 1,正常 |
| `self doctor --fix --dry-run` | exit 1,正常 |
| `self doctor --fix` | **exit 134,SIGABRT** |

原因是非 fix / dry-run 在 `doctor.cpp:3877` / `3888` 就 return 了,走不到 4048 的打戳。

**这条与搬家无关**:任何只读挂载 / 沙盒 / 权限不足的 home 上 `self doctor --fix` 都会这样。
可以独立修、独立测。

---

## 5. 反证:这个 home 其实已经有一半是可搬的

| 层 | 形式 | 实测 |
|---|---|---|
| shim(`subos/*/bin/*`) | **相对** `../../../bin/xlings` | 90/90 相对,0 绝对 |
| subos env(`subos_info.envs`) | **占位符** `${subosdir}` | 94 个 subos 状态文件:占位符 30 处,绝对 1 处 |
| self init 的链接 | **优先相对**(`xself/init.cpp:88`,失败才退回绝对) | — |
| versions DB `path` | **绝对** | home 状态文件里绝对路径 2853 处,`${XLINGS_HOME}` **0 处** |

读侧 `xvm::expand_path`(`db.cpp:699`)一直存在,**每次读都会展开 `${XLINGS_HOME}`**。
也就是说:**读者早就支持可搬形式了,只是没有任何写者产出它。**
和 [[exports offered, never consumed]] 是同一个形状 —— 机制齐备、零调用方。

---

## 6. 要修的话动什么(每档都带陷阱)

### A. 止血(不改格式,风险最低)

1. **prune 前问一句「新根下在不在」。**
   报告人提议「比较 recorded root 和 current root」—— 但 **`.xlings.json` 里没有 home 字段**
   (键只有 activeSubos / hintsSeen / index_repos / knownProjects / lang / mirror / repo /
   subos / theme / tui / version / versions / xim),没有 recorded root 可比。
   **也不需要**:`xvm/owner.cpp:11` 的 `coordinate_from_payload_path` 已经能从**任意前缀**的路径里
   右往左解析出 `<store>/<version>`,和 `Config::paths().dataDir/"xpkgs"` 一拼就是新路径。
   命中 → 改写记录(并且顺手就能说出「这个 home 从 X 搬到了 Y」);不命中 → 才 prune。
2. **`record_client_version` 补 `try/catch`**;更彻底的是把 `self`/`subos`/`profile` 的提前分派
   挪进顶层 `try`,一次关掉一整类(§4.2)。
3. **断链扫描加 `lib` / `lib64`**(`doctor.cpp:2069`)。
   ⚠️ **陷阱:这条不能单独上。** 现在的 repair 动作是 `fs::remove`(`doctor.cpp:2413`),只删不重指。
   在搬家的 home 上单独加广度 = 把 431 条链接删掉,比现状更糟。必须和 A1 的重写一起上。

### B. 让新装的包可搬(改格式)

- 写侧在 `registration.cpp:787` 把 home 前缀折叠成 `${XLINGS_HOME}`。
- ⚠️ **陷阱:`VData::path` 有约 10 处不过 `expand_path` 的裸读者** ——
  `bindings.cpp:501/536`(拼符号链接的 source)、`installer.cpp:108`(`resolved.path`,
  正是形态 2 那个 `create_symlink` 的源)、`owner.cpp:169`、
  `db.cpp:154/163/209`(`normalized_payload_path`)。只改写者会让 sysroot 链接指向字面量
  `${XLINGS_HOME}/...`。
- 更安全的形状:**磁盘上存占位符,`db.cpp:816` 反序列化时就展开成绝对路径**,
  内存里的 `VData::path` 语义完全不变 —— 这样十个裸读者一个都不用动。
  注意方向:这是「写时归一 + 读时展开」,与 [[read/write invariant asymmetry]] 里
  那个「写归一、读不归一」的坑正好相反,不会伤到老 home。
- 老 home 需要一次性重写,挂在 `--fix` 的 migration 上(那里已经有 `.xlings.json:version` 打戳机制)。

### C. ELF 层:这一层不可能纯靠相对路径解决

- **`PT_INTERP` 内核按字面量加载,不做 `$ORIGIN` 展开** —— 解释器路径**必须**是绝对的。
  所以搬家后只有两条路:重写每个 ELF 的 interp,或在旧路径留一个符号链接
  (报告人的 workaround 正是后者,也是唯一零成本的)。
- **`RPATH/RUNPATH` 由 ld.so 展开,可以用 `$ORIGIN`**,但实测 85/85 是绝对、0 个 `$ORIGIN`。
  改成 `$ORIGIN` 可行,但要一并考虑 [[DT_RPATH vs DT_RUNPATH transitivity]](RUNPATH 不传递)
  和 [[elfpatch.set rpath ignored]](`set()` 的 rpath 参数从不被读)。
- **现实建议:不要承诺「home 可任意搬家」。** 承诺「搬家后 `doctor --fix` 能修回可用」,
  ELF 层用 patchelf 重写(`xself/install.cpp:899` 本来就装 patchelf),
  或者提供一条显式的 `xlings self relocate <old-root>`。
  显式命令比「doctor 自动猜」更好:它有一个可以被证伪的输入(旧根),而 doctor 只能推断。

---

## 7. 与 #458 / #415 的关系

- **#415**(已关)是 interp 写死在**构建机**路径;#583 是**同一台机器**上因为 home 移动而失配。
  同一个字段的两种失配,#415 的关闭不覆盖这一种。
- **#458** 是 doctor 看不见 **payload 里烘焙**的 subos 路径;本文形态 2/3/4 是 doctor 看不见
  **payload 外面**的路径。合起来是同一句话:
  **doctor 的世界观 = 注册表 + `usr`/`etc`/`share`;磁盘上其余部分它都不看。**

---

## 8. 我推翻 / 修正了报告里的哪些说法

1. ❌ **「None of those findings names a path or mentions the home root」**
   —— `BrokenPayload` finding **是**打印旧绝对路径的(§1 实测输出)。
   屏幕上信息本来就有,缺的是**解释**。所以 UX 侧的修复可以非常小:
   一句「这个前缀不是当前 home 根」就够了。报告人看到的无路径 finding 是 `binding state` 那几条,
   属于另一个问题(他自己也标了「may not be caused by the move at all」—— 这个判断是对的)。
2. ❌ **「teach --fix to notice that the recorded root differs from the current root」**
   —— 没有 recorded root 可比;但**不需要**它(§6 A1)。
3. ⬆️ **「at minimum, doctor should … refuse to deregister」** —— 实测比这更严重:
   它还删了 shim 和头文件链接,并且 **exit 0 + `status OK`**。
   所以「最低限度」应该定在:**在无法解释 payload 为何缺失时不要 prune**,
   而不只是「搬家时不要 prune」。
4. 🔍 **「51 条 lib 链接没被重指」的归因** —— 真因是**扫描不覆盖 `<subos>/lib`**,
   与搬家无关的独立缺陷。证据:这台没搬过家的机器现在就有 6 条断链,doctor 报 0(§2)。
5. ✅ 报告人「未在 2026.9.3.2 复测」的保留是对的,但结论不变:**2026.9.3.2 上全部复现**。

---

## 9. 建议的 issue 拆分

| | 内容 | 独立性 | 证据 |
|---|---|---|---|
| **#583 主体** | home 可搬性(§6 B + C) | 大改,需设计 | §2、§5 |
| **新 1** | `self doctor --fix` 在只读 home 上 SIGABRT;`self`/`subos`/`profile` 在顶层 catch 之外 | 完全独立,可立刻修 | §4 + gdb 栈 |
| **新 2** | 断链扫描不覆盖 `<subos>/lib`/`lib64` | 完全独立 | §2,真实 home 6 条 |
| **新 3** | prune 谓词把「不在记录路径上」当成「载荷没了」;`--fix` 把「删掉问题」计为 healed 并 exit 0 | 独立,是 §0 表里那类 | §1、§3 |

---

## 附录:复现脚本

```bash
S=$(mktemp -d); H=$S/homeC
mkdir -p $H/bin $H/data/xpkgs/xim-x-demo/1.0.0/{bin,lib,include} \
         $H/subos/default/{bin,lib,usr/include}
printf '#!/bin/sh\necho demo-ok\n' > $H/data/xpkgs/xim-x-demo/1.0.0/bin/demo
chmod +x $H/data/xpkgs/xim-x-demo/1.0.0/bin/demo
echo 'int x;' > $H/data/xpkgs/xim-x-demo/1.0.0/include/demo.h
touch $H/data/xpkgs/xim-x-demo/1.0.0/lib/libdemo.so
cp ~/.xlings/bin/xlings $H/bin/xlings

python3 - "$H" <<'PY'
import json,sys,os; H=sys.argv[1]
json.dump({"activeSubos":"default","version":"2026.9.3.2","mirror":"CN",
 "versions":{"demo":{"filename":"demo","type":"program","versions":{
   "1.0.0":{"path":os.path.join(H,"data/xpkgs/xim-x-demo/1.0.0")}}}}},
 open(os.path.join(H,".xlings.json"),"w"),indent=2)
json.dump({"subos_info":{"created_by":"repro"},
 "workspace":{"demo":{"active":"1.0.0","installed":["1.0.0"]}}},
 open(os.path.join(H,"subos/default/.xlings.json"),"w"),indent=2)
PY

# 三类链接,各按其真实写者的写法
ln -s $H/data/xpkgs/xim-x-demo/1.0.0/lib/libdemo.so   $H/subos/default/lib/libdemo.so       # installer.cpp:2084 绝对
ln -s $H/data/xpkgs/xim-x-demo/1.0.0/include/demo.h   $H/subos/default/usr/include/demo.h   # 头文件农场 绝对
ln -s ../../../bin/xlings                             $H/subos/default/bin/demo             # shim 相对

mv $H $S/homeD
XLINGS_HOME=$S/homeD $S/homeD/bin/xlings self doctor          # → broken payload,指向旧路径
XLINGS_HOME=$S/homeD $S/homeD/bin/xlings self doctor --fix    # → dropped / removed / status OK / exit 0
```

只读 home 的 SIGABRT:

```bash
chmod -R a-w $S/homeD
XLINGS_HOME=$S/homeD ~/.xlings/bin/xlings self doctor --fix; echo $?   # → 134
```
