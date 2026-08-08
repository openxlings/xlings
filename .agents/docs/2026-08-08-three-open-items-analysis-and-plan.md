# 三条遗留项:实测、根因、与方案

> 2026-08-08 · 承接 `2026-08-08-declared-vs-effective-open-defects-design.md` §8
> 对象:D5 验收、D4 验收(被 pin 挡住)、`xim-pkgindex#573` / `xlings#511`
> 方法:**先量,再写**。本文每条结论后面都跟着产生它的那条命令输出。

---

## 0. 先说三条被测量改写的结论

写方案之前逐条验,**其中两条是我自己昨天刚写下的说法,都错了**。

| 我原来的说法 | 实测 |
|---|---|
| D5「卡在 loader 切换上」,言下之意是还要补包 | **不需要任何新包。** 是解析顺序问题,见 §1 |
| `#573`:rpath 每次运行被重新前插而累积 | **假的。** 脚本一直用 `-dumpspecs`(内建、干净),不可能自累积。真机制是 stamp 闸门把**历史损坏**冻住了,见 §2 |
| `#511` 的谓词是「不在两张 workspace 表里」 | 准确,但**不是最有害的那一面**——最有害的是载荷还在,见 §3 |

这个比例(三条里两条)和主文档 §7 记的完全一致。**规律没变:读了一个看起来对的地方就下结论,一半会错。**

---

## 1. D5 —— 不缺包,缺的是解析顺序

### 1.1 实测:运行期到底加载了谁

判据不能是「AWT 起来了」。§8.5 已经证明那会**假通过**——库是宿主提供的。
唯一能回答的是 `LD_DEBUG=libs` 的 `calling init:`(`ldd` 在沙箱里不存在,
`readelf -d` 只给 DT_NEEDED 不给实际解析结果)。

headless `Toolkit` + `getAvailableFontFamilyNames()` + `AudioSystem`,实测:

```
ours=12  host=15
```

15 个宿主对象,分三类:

| 类 | 对象 | 说明 |
|---|---|---|
| **glibc 族(6)** | `ld.so` `libc` `libdl` `libm` `libpthread` `librt` | 切 INTERP 后**按构造**变成我们的,不需要动作 |
| **我们已有、但解析输了(7)** | `libasound.so.2` `libfontconfig.so.1` `libfreetype.so.6` `libexpat.so.1` `libpng16.so.16` `libz.so.1` (+`libXrender.so.1` 装上后) | 都在 subos sysroot 里,**只是没被选中** |
| **看似缺失(3)** | `libbrotlicommon.so.1` `libbrotlidec.so.1` `libbz2.so.1.0` | 见 1.2 —— **它们会自己消失** |

### 1.2 那三个"缺失"的包不用打

```
我们的 freetype NEEDED:  libc.so.6
宿主的 freetype NEEDED:  libz.so.1 libbz2.so.1.0 libpng16.so.16 libbrotlidec.so.1 libc.so.6
```

**brotli / bz2 / png 是宿主 freetype 的依赖,不是我们 freetype 的。** 我们的
freetype 把它们内建了。所以一旦解析优先我们的 sysroot,这三个直接从闭包里消失。

**结论:D5 不需要新包。** 我昨天把「JDK 还在用宿主库」读成了「闭包还不完整」,
这是两回事。`libXtst` + `alsa-lib`(`xim-pkgindex#570`)确实是最后两个要补的,
它们补完了。

> 唯一真的没装的是 `libXrender`,而它**索引里一直有**(`pkgs/l/libXrender.lua`),
> 只是本机没装。`xlings install libXrender` 即可。我第一次查时写成
> 「HOST-ONLY `<nowhere>`」,是我的 `ldconfig` grep 模式写错了。

### 1.3 为什么我们的库会输

JDK 自己的 `.so` 的 RPATH 只有 `$ORIGIN`:

```
libawt_xawt.so  RPATH: [$ORIGIN]
libjsound.so    RPATH: [$ORIGIN]
```

`$ORIGIN` 是 JDK 的 `lib/`,里面没有 `libXtst`。于是查找落到宿主的 `ld.so.cache`
——今天能找到,**切 INTERP 之后找不到**(我们 ld.so 里编进去的 cache 路径在任何
机器上都不存在,见 2026.8.8.1 发布说明)。

所以顺序必须是:**先让解析指向我们,再切 INTERP**。反过来做,JDK 直接起不来。

### 1.4 方案

```
D5-1  装 libXrender(索引已有,无新包)
D5-2  给 JDK 配方加 elfpatch:把 subos lib 目录写进 JDK 全部 .so 的 RUNPATH
D5-3  验证 LD_DEBUG host 计数:15 → 6(只剩 glibc 族)
D5-4  切 PT_INTERP(java/javac/... 以及 .so)
D5-5  验证 host 计数 → 0,且 headless Toolkit + 字体 + 音频全过
```

**D5-3 是闸门。** 在 host 计数降到只剩 glibc 族之前不许切 INTERP——那正是
2026.8.8.1 踩过的坑(切了之后 `libawt_xawt.so: libX11.so.6` 起不来)。

**验收必须是 provenance,不是 `LOAD_OK`。** 固化成脚本:跑 `LD_DEBUG=libs`,
断言 `calling init:` 里不含 `/lib/x86_64-linux-gnu`。这条断言可证伪——今天跑它
会失败(15 个),这正是它有价值的证据。

---

## 2. `xim-pkgindex#573` —— 我把机制写错了,真因更严重

### 2.1 更正

issue 里我写「rpath 段每次运行被重新前插,所以累积」。**这是错的**:

```
gcc -dumpspecs | grep -c "enable-new-dtags"   →  0
gcc -dumpspecs 里的 dynamic-linker            →  /lib64/ld-linux-x86-64.so.2
```

`-dumpspecs` 返回**内建的、干净的** specs,**不反映磁盘上的 specs 文件**。
而 `gcc-specs-config.lua` 从建立起就只用 `-dumpspecs`(git 历史全查过)。
所以它每次都从干净内容重建,**结构上不可能自累积**。

而且现场我也看走眼了:那是**一个 `-rpath` 后面跟 ~50 个冒号分隔的路径**,
不是 50 个重复的 `*link:` 块。累积在**传进来的 `--rpath` 值**里,不在这个脚本里。
`gcc.lua` 只拼两段(`glibc_lib .. ":" .. install_dir/lib64`),也不是它。

### 2.2 真正的机制:stamp 闸门把损坏冻住了

```lua
local stamp = install_dir .. "/.specs-rewritten-" .. version .. "-payload.stamp"
if os.isfile(stamp) then return end          -- 有戳就跳过
...
system.exec(specs_config_bin .. " ...")       -- 返回值没人看
io.writefile(stamp, version)                  -- 无论成败都写戳
```

三件事叠加:

1. **闸门问的是「跑过没有」,不是「结果对不对」**。specs 文件一旦损坏(旧版本
   脚本、被打断的运行、手工编辑、或**多个 home 写同一份共享载荷**——那些
   `/tmp/tmp.*/mcpphome/...` 路径正是隔离测试 home 留下的),就**永远不会被修复**。
2. `system.exec` 的结果没被检查。它失败会 raise,但**没有任何东西检查改写是否
   达到了目的**。
3. 戳无条件写。所以一次没做成的改写会被**永久记为做过**。

净效果就是我本机撞到的:INTERP 冻在 2.39,rpath 首项漂到 2.44,报出来是

```
libc.so.6: undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
```

——一个**完全不指向病因**的现象。

### 2.3 方案:把「跑过没有」换成「结果对不对」

```lua
-- 不再用 stamp 判断是否跳过,而是判断当前 specs 是否已经正确
local function specs_is_correct(gcc_bin, want_linker)
    local out = os.iorun(gcc_bin .. " -dumpspecs")   -- 干净内建,只用来对比
    -- 真正要看的是编译产物:拿 gcc 编个空程序,读它的 PT_INTERP
    ...
end
```

更简单也更强的做法 —— **验证后置条件,失败就报错**:

1. 改写后,用这个 gcc 编一个空 `main`,读它的 `PT_INTERP`;
2. 断言该路径**存在**,且它所属的 glibc 与 rpath 首项的 glibc **是同一个**;
3. 不满足则 `log.error` + `return false`(和 `__find_glibc_runtime()` 失败时
   一样 fail-fast,那段注释已经把理由写得很好了);
4. **戳只在断言通过后才写**。

这条方案**不依赖知道损坏是怎么来的**——这正是它的价值:上面 §2.1 说明我并不完全
知道。一个只在结果正确时才落戳的闸门,对任何来源的损坏都收敛。

> 顺带:`.specs-rewritten-*.stamp` 应当考虑不放在载荷目录里,或至少在
> 「载荷可能被多个 home 共享」这一点上写清楚。今天 `~/.xlings` 和 `~/.mcpp`
> 各有一份 gcc,而隔离测试 home 的路径出现在了共享载荷的 specs 里。

---

## 3. `xlings#511` —— 谓词错了,而且最有害的不是退出码

### 3.1 实测

```
$ xlings install noversion@1.0.0 -y      →  ✓ 1 package(s) installed
$ xlings remove  noversion       -y      →  [warn] 'noversion' is not installed in current subos 'default'
$ echo $?                                →  0
$ ls data/xpkgs/xim-x-noversion           →  还在
$ 钩子标记文件                            →  没生成
```

`src/core/xim/commands.cppm:704-742` 的判据是**两张 workspace 表**:

```cpp
bool in_active    = ws.contains(bareName)  && !ws.at(bareName).empty();
bool in_installed = wsi.contains(bareName) && !wsi.at(bareName).empty();
if (!in_active && !in_installed) { log::warn("... is not installed ..."); return 0; }
```

它引用的惯例(「移除不存在的东西算成功」)本身是对的。**错的是谓词:这个包是
存在的**——它的载荷目录就是 `install` 放的。两张表里没有它,于是判成不存在。

### 3.2 为什么载荷残留比退出码严重

`xlings install` 把「载荷目录存在」当作「已安装」,于是**跳过安装,连带跳过
`config()`**。所以

```
install → remove(报成功,载荷还在)→ install(报"已安装",config() 不跑)
```

得到一个 `config()` 永远不再执行的包。**这正是 aarch64 那条 bug 的同一条级联**:
一个没跑过 config() 的 gcc 载荷,产出的二进制带着打包机器的 `PT_INTERP`,
而报错(`posix_spawnp ... error 2`)指向完全无关的文件。

### 3.3 方案

把谓词从**记账**换成**事实**:

```
现在:  这个名字在 workspace / workspace_installed 里吗?
应为:  这个包的载荷目录在磁盘上吗?
```

- 载荷在 → 正常走移除:跑 `uninstall()` 钩子、删载荷、清理注册(有多少清多少);
- 载荷不在 → 保持现在的 no-op + 提示(这时惯例是对的);
- 两张表的信息**降级为诊断**(「顺带一提,它属于 subos X」),不再作为判据。

**不变量(值得单独断言)**:`remove` 返回 0 之后,不允许留下一个会让下一次
`install` 跳过 `config()` 的载荷。

**回归测试**:fixture 包 + `uninstall()` 写标记文件,断言三件事——退出码 0、
标记文件存在、载荷目录消失。我已经写过这个 fixture(在 #509 里,因为它够不到
D4 的分支所以撤掉了),可以直接复用。

### 3.4 与 D4 的关系(务必分清)

**这不是 #506。** 两个闸门,覆盖人群重叠,`#511` 这个**更早触发**:

| | 触发条件 | 现状 |
|---|---|---|
| `#511` 闸门 | 不在两张 workspace 表里 | 返回 0,**不跑钩子**,载荷残留 |
| D4 分支(`#506`) | 在表里,但没注册任何 xvm 版本 | 跑钩子,成功返回 |

真实的 `#506`(`local:gcc@15.1.0`)是 project-scoped 安装,**在表里**,所以走到
了 xvm 那条路。而我用 fixture 复现时两张表都没有它,于是被前一个闸门截走了——
**这就是 D4 无法在本地验证的原因**,不是环境问题。

---

## 4. D4 验收 —— 被一个此前没人注意的 pin 挡住

### 4.1 前置条件

```
xim-pkgindex/.github/workflows/ci-test.yml:53,153,214   XLINGS_VERSION=v2026.8.6.3
xim-pkgindex/.github/workflows/ci-xpkg-test.yml:58      XLINGS_VERSION=v2026.8.6.3
```

**比 D4 的修复还老。** 合并索引 bump(`latest` 已经是 2026.8.8.2)**不改变这个 CI
装什么**——它走 `quick_install` + 显式 `XLINGS_VERSION`。所以现在删掉
`posix-test.sh` 的容忍,只会把索引仓 CI 弄红。

### 4.2 升 pin 的风险是真实的

`v2026.8.6.3 → v2026.8.8.2` 跨越:

- **2026.8.7.1**
- **2026.8.8.1** —— host-loader 迁移 + 生态守卫,并且**改了 `remove` 的语义**
  (「`remove` 不再默默切断别人的依赖」)。而 `posix-test.sh` 正是**大量跑
  `remove`** 的那个脚本。
- **2026.8.8.2** —— D4 本身

也就是说:升 pin **必然**改变索引仓 CI 里 remove 的行为,而这既是我们想要的
(D4 生效),也可能带出无关的配方回归。

### 4.3 方案:拆成两个 PR,不要合并

```
PR-A  只升 pin:v2026.8.6.3 → v2026.8.8.2,容忍原样保留
      → 观察 CI。绿:说明升级本身干净。红:先修红的,与 D4 无关
PR-B  只删容忍
      → 观察 CI。绿:这就是 D4 唯一真实的验收
```

**必须拆。** 合在一起的话,CI 一红你分不清是升级带来的回归、还是 D4 没生效——
而这两者要做的事完全不同。这也是主文档那条规则的直接应用:一次只动一个变量。

---

## 5. 建议顺序

```
1. #511 谓词修正            —— 最高优先。它是 aarch64 那条级联的上游,
                               而且会让别人的验证悄悄失效(载荷残留 → 跳过 config())
2. #573 后置条件断言        —— 同一族(声明了 ≠ 生效了),且不依赖查清历史损坏来源
3. D4  PR-A 升 pin          —— 独立 PR,先看干净不干净
4. D4  PR-B 删容忍          —— 绿即验收
5. D5-1..D5-3 解析顺序      —— 装 libXrender + JDK RUNPATH,把 host 计数打到只剩 glibc
6. D5-4..D5-5 切 INTERP     —— 只有 5 达标才做
```

1 和 2 可以并行(不同仓)。3→4 必须串行。5→6 必须串行,且 6 有硬闸门。

---

## 6. 一条贯穿线(和主文档同一条)

这三件事底下是同一个形状,和 §3 写的一模一样:

```
#511   remove 报成功       →  钩子没跑、载荷还在       →  不出声
#573   stamp 说"改写过了"  →  改写没达到目的           →  不出声
D5     LOAD_OK             →  加载的是宿主的库         →  不出声
```

**都不是「有 bug」,是「系统接受/宣称了一件事,然后没有履行它,并且不说」。**

所以三条的修法也是同一条:

> **把闸门从「做过没有」换成「结果对不对」,并且在结果不对时出声。**

- `#511`:判据从记账换成载荷是否存在
- `#573`:戳只在编出的二进制 INTERP 可用时才落
- `D5`:验收从 `LOAD_OK` 换成 `/proc/self/maps` / `LD_DEBUG` 的 provenance

三条都**可证伪**:今天跑它们的新断言,三条都会失败。这正是它们值得写的理由。
