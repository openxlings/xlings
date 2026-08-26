# 私有 glibc 的 preload 应当跟随 sysconfdir(方案 P1)

> 2026-08-26 · 承接 `2026-08-09-ecosystem-closure-design.md`「一个进程只有一个 libc」
> 触发:`mcpp-community/mcpp#484` / `#485`(评审见 mcpp 侧
> `.agents/reviews/2026-08-26-pr485-review.md`)
> 目标:把「宿主凭什么往我们的封闭进程里塞库」这个洞一次堵上,
> 并顺带给 xlings 补上一个它今天**没有**的能力:用户态 preload

---

## 0. 一句话

私有 glibc 的 rtld 目前**硬编码**读宿主 `/etc/ld.so.preload`,把宿主 `.so`
注入到每一个受管进程——包括**我们分发给用户的产物**。
把这个路径改成跟随 `--sysconfdir`(`ld.so.cache` 早就是这么做的),
并允许运行期用环境变量覆盖以支持重定位。

**这不是给 glibc 加开关,是修正 glibc 自身的不一致。**
对 `--sysconfdir=/etc` 的普通构建,该补丁是**恒等变换**。

---

## 1. 问题:定级被原始 issue 说小了

`#484` 的标题是「gcc probe fails」。实测(本机,全部可复现,见 §2):

```
N1. 我们产出的程序,在有 /etc/ld.so.preload 的机器上   → exit=127,起不来
N2. 同一台机器上,宿主编译的程序                       → exit=0
```

**真正的性质是:我们产出的二进制在这一类宿主上不可运行。**
编译器只是第一个撞上的,因为它本身就是一个受管二进制,和用户拿到的产物
共用同一个私有 loader(产物 `PT_INTERP` = `.../xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2`)。

### 两种失败模式,第二种更危险

| preload 库的依赖 | 结果 |
|---|---|
| 不自足(如 `libonion.so → libdl.so.2`) | **硬挂**,`exit=127`,报错指向一个我们没引用过的库 |
| 自足(只需 `libc.so.6`) | **静默成功**,宿主 glibc 2.39 编的 `.so` 被绑到私有 2.44 上跑 |

第二种正是前作要防的「一个进程两个 libc」,却从 preload 这个后门进来,零诊断。
两条今天都无人知晓,因为触发条件(宿主 `/etc/ld.so.preload` 非空)在
开发机和 CI 上几乎不出现,而在**企业内网机、云上生产机、受合规管控的机器**上常见
——恰好是用户跑我们产物的地方。

---

## 2. 取证(全部本机实测)

### 2.1 同一个 rtld 里,一个路径可重定位,一个不可

```
$ strings ~/.mcpp/.../xim-x-glibc/2.44/lib/ld-linux-x86-64.so.2 | grep ld.so
/etc/ld.so.preload                                     ← 字面量,未被 --prefix 重写
/home/xlings/.xlings_data/.../2.44/etc/ld.so.cache     ← 被重写成打包机路径
```

`ld.so.cache` 是 `SYSCONFDIR "/ld.so.cache"`,`/etc/ld.so.preload` 是硬编码字面量。
**这就是全部病根。**

### 2.2 私有 `ld.so.cache` 从来没有被读到过

```
$ ls /home/xlings/.xlings_data/.../2.44/etc/ld.so.cache  → No such file or directory
$ ls ~/.mcpp/.../xim-x-glibc/2.44/etc/ld.so.cache        → 3223 bytes,内容全是打包机路径
```

loader 找的是打包机绝对路径,用户机器上不存在 ⇒ **私有 cache 恒为空**。
载荷里那 3223 字节是死重,而且正是它把 issue 报告者引向了「重建 cache」这个
**不成立**的 workaround(重建出来的落在安装目录,loader 根本不去那里读)。

### 2.3 现成开关不存在

`ld.so --help` 只有 `--inhibit-cache`、`--inhibit-rpath`;`--list-tunables` 里也没有。
**任何方案都必须改 rtld**,成本相同,应挑语义最好的那个。

### 2.4 受管闭包本来就是自足的

| 二进制 | `PT_INTERP` | `DT_RUNPATH` |
|---|---|---|
| `g++` / `cc1plus` / `as` / `ld` / `ar` | 私有 loader | 全部有 |

**不需要任何环境变量。** 所以正确的问题不是「怎么让被注入的宿主库找到依赖」,
而是「宿主库凭什么在我的封闭进程里」。

### 2.5 两个被否掉的方向,各自的实测理由

| 方向 | `--version` | `-c` | 链接 | 否掉的理由 |
|---|---|---|---|---|
| `#485` 的 `loader --library-path` 包住 driver | OK | **FAIL(cc1plus)** | FAIL | `--library-path` **不跨 execve**,而 preload 逐进程重做 |
| `LD_LIBRARY_PATH`,宿主 binutils | — | FAIL | FAIL | 泄漏进宿主 `as` ⇒ 宿主 loader 装私有 `libc.so.6` ⇒ `__pointer_chk_guard` GLIBC_PRIVATE 崩 |
| `LD_LIBRARY_PATH`,受管 binutils(`-B`) | — | OK | OK | 端到端可行,但依赖「永不回落宿主 as/ld」这个额外不变量 |

### 2.6 复现手法(无需 root)

⚠️ `LD_PRELOAD`(环境变量)与 `/etc/ld.so.preload`(文件)**是两套机制**,
只测前者会得出错误结论(补丁 preload 文件路径对 `LD_PRELOAD` 无效)。

```bash
mkdir etcov && for e in /etc/*; do ln -s "$e" "etcov/$(basename $e)"; done
echo /path/to/libonion_sim.so > etcov/ld.so.preload
bwrap --dev-bind / / --ro-bind $PWD/etcov /etc \
      --unsetenv LD_PRELOAD --unsetenv LD_LIBRARY_PATH  <cmd>
```

构造件:`gcc -shared -fPIC -o libonion_sim.so x.c -Wl,--no-as-needed -lz`
(`DT_NEEDED: libz.so.1`,无 `RPATH`/`RUNPATH`)。

---

## 3. 为什么 glibc 把它写死:两层信任模型

`man 8 ld.so` 的 *Secure-execution mode* 一节:`LD_PRELOAD`、`LD_LIBRARY_PATH`、
`LD_AUDIT`、`LD_DYNAMIC_WEAK`…… 在 setuid 场景下**作废并从环境中抹掉,
让程序根本看不见**。

`/etc/ld.so.preload` **不在这个列表里**——它不是环境变量,它是那条
**在 secure-execution 下依然生效**的通道:

| 通道 | 谁能控制 | secure-execution 下 |
|---|---|---|
| `LD_PRELOAD` 等 env | 启动进程的任何人 | 作废 + 抹掉 |
| `/etc/ld.so.preload` | 能写 `/etc` 的人(root) | **照常生效** |

审计探针如果能被 `export X=` 关掉,那就不叫审计。**固定路径不是疏忽,
是这个机制的全部价值。** 另一个佐证:glibc 给了 cache 和 rpath 的 inhibit 开关
(且 `--inhibit-rpath` 明确写「secure-execution 下忽略」),**唯独没给 preload 的**。

### 由此得到的判别式

- `ld.so.cache` / `ld.so.conf` / `DT_RPATH` —— 回答「**去哪找**」:可以有多份、
  无策略含义 → 跟随 sysconfdir、配 inhibit 开关
- `/etc/ld.so.preload` —— 回答「**什么被强加给你**」:既不 sysconfdir 化,也不给开关

**我们要动的是第二类,所以必须把安全语义一起设计,不能只当成一个路径。**

---

## 4. 方案 P1

### 4.1 语义

rtld 中读 preload 文件的那一处,改为:

```
1. 若 XLINGS_LD_PRELOAD_FILE 已设置 且 非 AT_SECURE:
       "" (空)  → 不读任何 preload 文件
       <path>   → 读该文件
2. 否则:读 SYSCONFDIR "/ld.so.preload"
```

三态语义,一个变量。**默认(不设变量)= 读自己 sysconfdir 的那份**,
而不是宿主 `/etc` 的。

### 4.2 三个关键性质

**① 对普通系统构建是恒等变换。** stock glibc 用 `--sysconfdir=/etc` 构建,
此时第 2 条即 `/etc/ld.so.preload`,行为与上游逐字一致。
这既降低风险,也让它有上游化的余地——可以论证当前上游行为是自身的不一致:
同一个 rtld,cache 被 `--sysconfdir` 重定位了,preload 却没有。

**② 我们的载荷默认封闭。** sysconfdir 是打包机路径,那份文件在用户机器上
不存在 ⇒ 不 preload ⇒ 编译器、`cc1plus`、`as`、`ld`、`ar`、产出的程序
**全部自动免疫,不需要任何一处设环境变量**。

**③ env 只为可重定位而存在,不为策略。** sysconfdir 烘死(cache 就是这么坏的),
所以要开放 payload 级 preload 就必须运行期给路径。**这是新增能力,不是修复的一部分。**

### 4.3 安全规则(承重,不可省)

`XLINGS_LD_PRELOAD_FILE` 必须与 `LD_PRELOAD` **同级对待**:
`AT_SECURE`(setuid / setgid / file capabilities)时**作废并从环境中抹掉**。

做到这一点,新增权限面 = **0**:`LD_PRELOAD` 本来就给了完全等价的能力
(任意注入 `.so`),我们没有创造新的攻击面,只是多了一个同权限的入口。
漏掉这一点,则任何使用私有 glibc 的 setuid 程序都成为提权向量。

### 4.4 这不是 ABI 变更

不涉及符号版本、struct 布局、调用约定;`libc.so.6` 导出符号集不变;
对该 glibc 编译的程序字节不变;rtld 与程序的接口(auxv、入口协议、符号解析)不变。
载荷本来就不是 stock(版本横幅已是 `xlings-fromsource`)。

**动的是一条策略语义**,靠 §4.3 化解。

---

## 5. 影响面

### 5.1 受影响 / 不受影响

| 对象 | 是否受影响 | 说明 |
|---|---|---|
| 受管编译器与其全部子进程 | ✅ 修好 | 默认封闭 |
| `mcpp pack --mode self-contained` 产物 | ✅ 修好 | 自带私有 loader |
| `mcpp pack --mode vendored` 产物 | ➖ 本就不受影响 | `PT_INTERP` 是宿主 loader |
| `mcpp pack --mode static` 产物 | ➖ 本就不受影响 | 无动态 loader |
| 宿主上的其他一切程序 | ➖ 完全不受影响 | 补丁只在我们的 rtld 里 |

**不是巧合**:需要自带 loader 的模式才会踩私有 rtld 的坑。

### 5.2 必须明说的策略后果

**我们分发的产物将不再加载用户主机的 preload 探针**(审计 / APM / HIDS)。

支持这个选择的论据:**今天在这类主机上,没有任何「正确行为」值得保留**
——要么 `exit=127` 起不来,要么静默跨 glibc 混链(§1)。P1 不会破坏任何
**能工作**的东西。

但这仍是一个策略决定,必须写进 `xim-x-glibc` 的包文档,并在
release notes 里点名。

### 5.3 旧载荷仍在野

已发布的 glibc 载荷没有这个补丁。过渡期内 mcpp 侧的**诊断**是唯一能把
`exit=127` 翻译成人话的东西,不可省。

---

## 6. 改动清单

| 仓库 / 文件 | 改动 |
|---|---|
| `xim-pkgindex-fromsource` `pkgs/g/glibc.lua`(configure 在 `:102-112`) | ① configure 前加 rtld 源码补丁步骤;② 安装后删除 `etc/ld.so.cache`;③ 版本 bump |
| 同上 `config()` / `xvm.add("glibc")` 附近 | (可选,新能力)声明 `envs`,机制见 `xlings` `src/core/xvm/types.cppm:44` |
| 打包机 + 索引 | 重建 artifact、重新发布、索引 bump |
| `mcpp` | **只加诊断**;撤 `PR#485` 的机制部分 |
| `mcpp` `src/pack/pack.cppm:686` | (可选,新能力)wrapper 加一行 `export`,紧挨现有 `MCPP_BUNDLE_DIR` |

`glibc.lua:117` 已有的 TODO 正是同一类问题,可一并处理:

```lua
-- TODO: use make install DESTDIR=$SYSROOT to avoid prefix hardcoding path in some files (libc.so)
```

### ⚠️ 落地前先确认

本地检出的两个索引里 glibc 都停在 **2.39**
(`d2learn/xim-pkgindex` 是 `XLINGS_RES` 预编译,`xim-pkgindex-fromsource` 是源码构建),
而本机装的是 **2.44 预编译产物**(baked path 是打包机的,证明非本机构建)。
**2.44 的描述符由谁发布,必须先查清**——补丁要打在产出该 artifact 的那条链上。

---

## 7. 验收判据

⚠️ 每一条都要做**撤掉补丁的对照**,否则不知道判据在不在测东西。

1. **端到端,带对照。** 在 §2.6 的 bwrap 环境里,四项全过:
   `g++ --version` / `-c` 编译 / 完整链接 / **运行产出的程序**;
   且换回未打补丁的 loader 后**四项全红**。
   (`#485` 只过第一项——这正是它的测试计划打勾却没发现问题的原因。)

2. **闭包判据,带分母。** `LD_DEBUG=libs` 跑一次受管编译,统计加载对象:
   **来自宿主路径的数量 = 0 / 总数 N**。这条覆盖 §1 的「静默混链」模式,
   而它今天没有任何测试。

3. **字面量判据。** `strings <新 loader> | grep -c '^/etc/ld.so.preload$'` == 0。

4. **恒等性判据。** 另用 `--sysconfdir=/etc` 构建一份,在同样 bwrap 环境里
   preload **照常生效**——证明补丁对普通构建是恒等变换。

5. **安全判据。** 一个 setuid 测试程序在设了 `XLINGS_LD_PRELOAD_FILE` 时,
   该变量被忽略**且 `/proc/self/environ` 里已被抹掉**。

6. **死重判据。** 新载荷里不存在 `etc/ld.so.cache`。

---

## 8. 风险与回退

| 风险 | 处置 |
|---|---|
| glibc 升级需 rebase 补丁 | 一个函数,且我们本就从源码构建;把补丁作为配方的一部分维护 |
| 用户依赖宿主 preload 探针 | §5.2 已明说;需要时用 `XLINGS_LD_PRELOAD_FILE=/etc/ld.so.preload` 显式恢复 |
| 补丁写错导致载荷不可用 | 判据 1 的对照 + 判据 4 的恒等性一起构成双向门 |
| 回退 | 索引层面回退到上一版 glibc 即可;`min` 版本门要写下界不写死 |

---

## 9. 未决

1. **默认值 P1 已定**(读自己的 sysconfdir),但 §5.2 的策略后果需要产品层面确认。
2. 2.44 描述符的发布位置(§6)。
3. 变量名:建议 `XLINGS_LD_PRELOAD_FILE`。**归属是 glibc 载荷而非 mcpp**
   ——吃它的是 xim 装的任何使用私有 glibc 的东西,故用 `XLINGS_*`。
   勿用 `LD_` 前缀,避免与 glibc 自身命名空间混淆。
4. `mcpp` `write_topentry_wrapper`(`pack.cppm:727`)服务的 BundleProject 模式
   是否也用私有 loader,未核实。
