# Change Log | [xlings论坛](https://forum.d2learn.org/category/9/xlings)

## 2026

### 2026-05 (v0.4.23) — Sandbox V4 重设计 (BREAKING)

- **`--sandbox` 是 `use` 的修饰符,不再是 subos 的 type** —— 一个 boolean flag。同一个 subos 可以选择带或不带 sandbox 隔离进入,正交两个维度。

  ```
  xlings subos new mybox            # 普通 subos,不变
  xlings subos use mybox             # 普通进入,提示符 [xsubos:mybox]
  xlings subos use mybox --sandbox   # NEW:fs 隔离进入,提示符 <xsubos:mybox>
  ```

  设计文档:[`.agents/docs/sandbox-v4-design.md`](sandbox-v4-design.md)。

- **核心心智**:sandbox = 普通 subos + dotfile 隔离 + 必要 host 系统目录 RO 复用 + xlings home RW 共享。
  - **私有(对应 `<subos>/{home/<user>, tmp, etc/...}` 子树)**:`$HOME`、`/tmp`、`/etc/{passwd,group,hosts,nsswitch.conf}`(模板用真实 user,不再是 root 行)
  - **host RO**:`/proc /sys /dev`、`/etc/resolv.conf`、`/etc/ld.so.cache`、`/usr`(POSIX 工具 + 库)、`/usr/lib:/lib`、`/usr/lib64:/lib64`(usrmerge 兼容)
  - **host RW**:`~/.xlings`(嵌套 bind 在 `/home/<user>` 之上 ── proot 子路径覆盖父路径) ── 这就是为什么 sandbox 里 `xlings install/list/remove` 跟外面一模一样:同一个 xpkg 池,同一个 xvm DB,workspace 仍然 per-subos
  - **真实 user 身份**:proot 不带 `-0`,`whoami` 返回 `speak`(uid 1000),`$HOME=/home/speak`,`$USER=speak`。sandbox 内创建的文件 owner 自然连续,出 sandbox 后用户能直接读写
  - **shell 跟外界一致**:sandbox 用 `$SHELL`(用户当前 shell)。`/bin/<x>` 自动翻译成 `/usr/bin/<x>` 因为 sandbox `/bin` 私有不带 host 二进制

- **删除的旧实现**(V1.1-V1.3,0.4.21-0.4.22):
  - `xlings subos new --sandbox-shell <xpkg>` flag 删除(parser 直接报 unknown option)
  - `.xlings.json` 里 `sandbox-shell` / `sandbox-shell-xpkg` 字段不再读写(老 sandbox 的字段被静默忽略)
  - `create_sandbox()`、`use_sandbox_()`、`ensure_shell_installed_and_linked_()` 函数全删
  - `<subos>/root`、`<subos>/etc/passwd`(只 root 行)等老布局停止生成
  - **不做老 sandbox 迁移**:0.4.21-0.4.22 创建的 sandbox `subos use --sandbox` 会 work(init_sandbox_dirs_ idempotent),但老的 `<subos>/root` `<subos>/bin/<shell>` symlink 静静躺着不影响。要彻底清掉 → `subos remove + subos new`

- **提示符切括号**:profile_resources.cppm bump 9 → 10。bash/zsh/fish/pwsh 4 个 profile 各加一个 `XLINGS_SUBOS_MODE=sandbox` 检查分支,选 `<>` 替代 `[]`。idempotency 检查涵盖两种括号形式,re-source 安全。

- **用户体验**:
  ```
  $ xlings subos new mybox
  $ xlings subos use mybox --sandbox
  <xsubos:mybox> $ whoami                    # speak,真实身份
  <xsubos:mybox> $ echo $HOME                # /home/speak,但是 sandbox 私有 dir
  <xsubos:mybox> $ ls ~/                      # 空(只有 .xlings,因为它是宿主 RW bind)
  <xsubos:mybox> $ xlings install fish        # 跟宿主一样,装到宿主 xpkg 池,workspace 进 mybox
  <xsubos:mybox> $ fish                       # 启动 fish
  <xsubos:mybox> $ exit
  $ ls ~/.config                              # 宿主 ~/.config,sandbox 内的修改不可见 ── 没污染
  ```

- **修了哪些 0.4.22 实测 bug**(全部由 V4 设计自动解决):
  - `cd /root: Permission denied` → V4 不去 `/root`,$HOME 是 `/home/<user>`(私有 dir,真实 user owner,可写)
  - `ls`、`uname` not found → V4 把 `/usr` 整个 RO bind 进来,所有 host coreutils 可用
  - sandbox 里 `xlings install` 不生效 → V4 把 host `~/.xlings` RW bind 进 sandbox,xlings 看到的是同一个 home,普通 subos workspace activation 流程跑通
  - elfpatched binary 不能跑 → 没有这个问题了,因为 sandbox 装的包是 sandbox 自己装的(走宿主 xlings install 路径,落到宿主池,跟外面一致)

- **改动量**:`src/core/subos.cppm` 净 -150 LOC(删的比加的多);`profile_resources.cppm` +30 LOC(4 个 shell 都加 mode 分支);`tests/e2e/subos_sandbox_test.sh` 全重写 11 个场景。版本 0.4.22 → 0.4.23。

### 2026-05 (v0.4.22)

- **subos sandbox 一步到位 + 真正能跑 elfpatched binary**
  - V1.1(0.4.21)的痛点:`subos new <name> --sandbox-shell xim:fish` 只写 config,不装 shell,不连 binary。用户 `subos use` 报错"shell not found",**提示又让用户先 `subos use && xlings install`** —— 死循环。就算手动 `xlings install fish` 把 fish 装进默认 subos,fish 仍然不在 sandbox 自己的 `bin/`,**且进入 sandbox 后 elfpatched fish 找不到 loader**(绝对宿主路径在 proot rootfs 里指向空目录)。
  - **V1.2 修复(`src/core/subos.cppm`)**:
    1. **`subos new --sandbox-shell` 创建期 eager install + symlink**:写完 sandbox 目录布局后,把 `--sandbox-shell` 指定的 xpkg 通过 `xim::cmd_install` 装好(yes=true 静默),从 xvm DB 解析其 bindir,**symlink `<sandbox>/bin/<basename>` → `<xpkg-payload>/bin/<basename>`**。一行命令拿到可用的 sandbox。
    2. **proot argv 加 `--bind=<home_dir>:<home_dir>` 自映射**。xim 装的 binary 经 elfpatch 后 interpreter / rpath 是宿主绝对路径(如 `/home/<user>/.xlings/data/xpkgs/xim-x-d2x/.../ld-linux-x86-64.so.2`)。proot `-R <sandbox>` 会把绝对路径路由到 `<sandbox>/<原路径>`(空目录 → loader 找不到 → 启动即死)。**自映射让那条绝对路径在 sandbox 内仍然指向宿主真实位置**,fish / bash / zsh 等动态链接的 elf 可执行原生跑起来。`/xlings` 那条 bind 是面向用户的便捷别名,这条自映射才是让 binary 真正能跑的关键。
    3. **`subos use` lazy 自愈**:进入路径检查到 `<sandbox>/bin/<shell>` 缺失或 dangling(用户后来 `xlings remove xim:fish` 把 payload 删了)→ 直接复用 hydrate 流程重装 + 重连。原死循环错误提示删掉,换成可执行的 `xlings install <xpkg>`(没记 xpkg id 的兜底分支)。
    4. **顺手修一个 dangling-pointer bug**:`Config::versions()` 是 by-value 返回临时 VersionDB,把它当 `get_vdata(Config::versions(), ...)` 的实参用,返回的 `VData*` 在表达式结束就指向已析构的临时对象 —— 落在 `vd->path` 上观察到一段乱码字符,导致 `fs::exists` 始终 false。**改成先 bind 到 local `auto db = Config::versions();`** 再传 `get_vdata(db, ...)`。这种坑全文件 grep 一遍,后续 PR 应该全 audit。
  - 用户可见 UX:
    ```
    xlings subos new sb --sandbox-shell xim:fish    # 自动装 fish + 连进 sb/bin/fish
    xlings subos use sb                              # proot 直接进,fish 真能用
    xlings remove xim:fish && xlings subos use sb    # 自愈:重装 + 重连,继续进
    ```
  - **测试(`tests/e2e/subos_sandbox_test.sh`)**:`xim:sh` 改成本地 fixture xpkg(用宿主 busybox,无网络依赖),从 7 scenario 扩到 8 scenario:
    - S1 加 symlink 验证(target 必须落在 `data/xpkgs/xim-x-sh` 下)
    - S6 删掉手动 `cp busybox` 一步,验证 hydrate 流程交付的 binary 也能在 proot 里跑通
    - **新增 S7**:rm 掉 sandbox 的 shell symlink,下一次 `subos use` 必须自动恢复 symlink
    - 旧 S7 (remove cleanup) 改成 S8
  - 改动量:subos.cppm +~150 LOC(helper + 2 调用点 + bind 一行);test +~80 LOC(fixture + S1/S6/S7 改写)。版本 0.4.21 → 0.4.22。

### 2026-05 (v0.4.21)

- **subos sandbox 模式:proot 文件系统隔离,无 sudo,Linux 专属**
  - 设计文档:[`docs/plans/2026-05-09-subos-sandbox-design.md`](../../docs/plans/2026-05-09-subos-sandbox-design.md)。
  - subos 现在有第三种类型 — **sandbox**(在原有 global / shell-level 之上)。通过 proot `-R` + 选择性 bind 实现 fs 视图隔离,**dotfile 完全独立**(`~/.gitconfig`、`~/.cache`、`~/.npm` 等不再污染宿主),**zero sudo**(用户态 ptrace,不需要 user-namespace,绕过 Ubuntu 24.04+ 的 AppArmor `unprivileged_userns` 限制)。
  - **命令面只动一个 flag**:`xlings subos new <name> --sandbox-shell <xpkg>`。`subos use <name>` 自动检测 `.xlings.json` 里的 `sandbox-shell` 字段决定是否走 proot 路径(有 → sandbox;无 → 现有 global / shell-level 行为,**零回归**)。
  - **FS 映射(进入后 inside view)**:
    - `/` = `~/.xlings/subos/<name>/`(rootfs,sandbox 物理目录)
    - `/bin/`、`/etc/`、`/root/`(=`$HOME`)、`/tmp/` = sandbox 私有
    - `/xlings/` ← bind `~/.xlings/`(整个 xlings 宇宙,xlings 二进制 + 全局 xpkgs 池都在这一条 bind 下)
    - `/proc /sys /dev /etc/resolv.conf` ← bind 自宿主(内核接口 + DNS)
    - `/etc/{passwd,group,hosts,nsswitch.conf}` ← 显式 bind 自 sandbox 自家模板,**覆盖 proot 默认对宿主 /etc/* 的 kompat 自动 bind**(否则 sandbox 内 `cat /etc/passwd` 会看到宿主全部用户)
  - **用户身份**:`proot -0` 伪装 root(`whoami` → root, `id -u` → 0)。理由:多数工具 `geteuid != 0` 直接报错;业界惯例(Termux / proot-distro / Toolbox 全用 -0);文件 ownership 实际是宿主真实 uid,退出后用户能正常读写。
  - **subos 创建时 xlings 自动写 4 个 /etc/* 模板**(passwd / group / hosts / nsswitch.conf,共 ~80 字节);加上 root/、tmp/ 空目录;sandbox 物理目录初始约 1 KB(不含 shell xpkg)。
  - **proot 二进制探测顺序**:`xim:proot` xpkg → `~/.xlings/runtimedir/proot` → 系统 PATH。三处都没 → 报错并提示 `apt install proot` / `dnf install proot` / 手动放到 `runtimedir/`。**V1.1 不自动下载**(后续可加)。
  - **嵌套 sandbox 拒绝**:在 sandbox 内再跑 `xlings subos use <name>` 会被拦下(env + `/.xlings.json` 双检测),提示 `type 'exit' first`。proot 嵌套 ptrace 行为不稳定,直接禁掉。
  - **平台明示**:macOS / Windows 上 `--sandbox-shell` 在 `subos new` 阶段就被拒绝,**不静默降级**(避免欺骗用户以为有隔离)。
  - **限制 / 跟其他 OS 的不同**(写在设计文档里,运行时 `subos info` 后续可输出):
    - 共享内核(`uname -r` = 宿主)、共享网络栈、共享 PID 视图(`ps aux` 看到全部宿主进程)
    - 不能 `mount`、`insmod`、改 hostname,这些需要 CAP_SYS_ADMIN
    - 没装 apt / apk / dnf,**xlings 是唯一包管理器**(用 `xlings install <xpkg>`)
    - syscall-heavy workload 慢 30-50%(proot ptrace 拦截开销)
  - 改动量:`src/core/subos.cppm` ~270 LOC(sandbox 创建 + 进入 + proot 探测 + argv 构造);`tests/e2e/subos_sandbox_test.sh` ~150 LOC(7 scenario);`.github/workflows/xlings-ci-linux.yml` 加 E2E-25。**没有新模块**,纯增量。
  - 测试覆盖:S1-S7(目录布局 / etc 模板 / config 字段 / 普通 subos 不受影响 / proot 缺失提示 / 真实进入 sandbox 验 root + /etc 隔离 / 删除清零)。本机静态 `/bin/busybox` 当 shell stand-in 验证完整流。
  - **后续(不在 V1.1)**:`xim:proot` xpkg(自家供应链)、`xlings subos use --sandbox-shell <new>` 换 shell、`--bring-in <path>` 选择性 dotfile bind、bwrap 后端(在 user-ns 可用的内核上自动选 bwrap,接 IsolationBackend 抽象)、OverlayFS 多 sandbox 共享 base layer。

### 2026-05 (v0.4.20)

- **嵌套 xlings home 时 project discovery 越界污染修复 (#276)**
  - 报告:用 xlings 安装 mcpp(mcpp 的 payload 里**包了一个独立的 xlings home** —— 内层 `XLINGS_HOME=$MCPP_HOME/registry`,`registry/` 落在 `~/.xlings/data/xpkgs/<repo>-x-mcpp/<ver>/registry`),内层 xlings 启动后,workspace / subos / install 路径全部跑偏到 `~/.xlings/.xlings/{subos,data,...}` 这种根本不存在的伪 project tree。下游所有 install/shim/workspace 操作的"现实"和 mcpp recipe 看到的"现实"分裂。
  - 根因(`src/core/config.cppm` `load_project_config_()`):cwd 向上 walk 找 `.xlings.json` 时,只用 `curNorm != homeNorm` 跳过自家 home,**没有挡别家**。内层 walk 会越过自己的 home 之后,继续向上爬到外层 `~/.xlings/.xlings.json` —— 那个文件其实是另一个 xlings home 的全局配置,但 walk 把它当 project root 加载了。
  - 修复:加 xlings-home 边界检测。任何同时含 `.xlings.json` **和** `subos/` **同级目录**的位置都识别为某个 xlings home(自家或别家),walk 在那里 break,绝不当 project 加载。可靠依据:project layout 的 subos 在 `<proj>/.xlings/subos/`(多一层 `.xlings/`),不会和 home 的 `<home>/subos/` 撞名。原 `curNorm != homeNorm` 检查被 break 取代(自家 home 也满足签名,break 已覆盖)。
  - 同样的边界检测也加到 `XLINGS_PROJECT_DIR` env-var fallback 路径上 —— env 指向某个 xlings home 时也拒绝(否则等于绕过 walk 检查重新引入污染)。
  - 锁住:新增 `nested_xlings_home_test.sh`(4 个 scenario,作为 E2E-24 加入 CI):
    - S1:内层 xlings cwd 在 `registry/data/runtimedir` → 不进 project mode,XLINGS_HOME 报内层
    - S2:常规 user project(不在任何 xlings home 下面)→ project mode 仍正确触发
    - S3:cwd `/tmp` + XLINGS_HOME=内层 → 不会误把任何 home 当 project
    - S4:`XLINGS_PROJECT_DIR=外层 home` 显式指过去 → 也被 boundary 检查拒绝
  - 影响 LOC:`config.cppm` ~25 行,新增 e2e ~100 行。无 schema 变更,无 API 变更,纯 walk 逻辑收紧。

- **大型 tarball 解压性能修复:libarchive read block 64 KiB → 4 MiB,跳过 NSS lookup (#275)**
  - 用户报告:`xlings install local:musl-gcc@15.1.0`(847 MiB tarball)整个安装阶段挂在 "下载阶段" ~217 秒,实际是解压。raw `tar -xzf` 同一文件 9.2 秒,**xlings ~23×**。
  - 实测定位(在用户原始 tarball 上跑):
    - **read block 大小**:`archive_read_open_filename` 默认参数 65536 字节 → 每 GiB ~16k 次 read syscall。改成 4 MiB(GNU tar 内部用的量级)后,syscall 数掉一个数量级。
    - **NSS lookup**:`archive_write_disk_set_standard_lookup` 给每个 unique uname/gname 调 `getpwnam_r`/`getgrnam_r`。在 LDAP / sssd / nscd 慢的环境下这一项可能挂秒级。xim-pkgindex tarball 一律 root:root,改成自定义 lookup callback 永远返回 0,绕掉整条 NSS 路径。
  - **效果**(808 MiB musl-gcc tarball):
    - 修复前(实测旧逻辑):**~217 s**
    - 修复后(本 patch):**5.79 s**(比 `tar -xpf` 9.31 s 还快 60%,因为没有 subprocess 启动开销 + pipe IPC)
    - 提速 **~37×**
  - **不引入新代码路径**:之前考虑过加 `posix_spawn("tar -xpf")` 子进程方案,实测对比发现 Tier 1 单独已经超过了 subprocess 路径(在多数硬件上 zlib 单线程 + 4 MiB block 已经接近 IO 上限,subprocess 启动成本反而拖慢小 archive 4 倍)。因此**直接增强 libarchive 路径**,不加 fallback / dispatcher / env var。
  - 改动量:`src/core/xim/extract.cppm` 改 ~30 行,无新模块、无新依赖、无新 escape hatch。

### 2026-05 (v0.4.19)

- **subos `xlings install` / `remove` 跨 subos workspace 污染修复(0.4.19 critical)**
  - 报告:在 fresh subos `tmp` 里 `xlings install pkg@2.0.0 -y` 然后 `xlings remove pkg -y`,**tmp 的 workspace 被污染成 default subos 的版本(比如 `pkg: 1.0.0/[1.0.0]`)**,而 tmp 从未装过 1.0.0。tmp 的 shim 也跟着 active 错误版本残留。
  - 根因:`installer.cppm` `uninstall()` 内联 xvm-ops loop(line ~1647)在 active 版本被卸时,fallback active pointer 用的是 **global versions DB** 的 `pick_highest_version`,subos-blind。具体说:tmp 卸 2.0.0 → detach 已正确清空 tmp.workspace[pkg],但接下来的 ops loop 看 global DB(还留着 default 用的 1.0.0)→ 把 1.0.0 写回 tmp.workspace。属于 0.4.18 之前就存在的旧 bug,但只有 C2 schema 落地、跨 subos 装不同版本之后才能稳定复现。
  - 修复:fallback 改成只看**当前 subos 的 `installed[]`**,空就清掉 workspace pointer 并删 shim(不再"凭空"分配一个本 subos 没 opt-in 的版本)。同一个 loop 也补上 sibling op(比如 node 的 npm/npx)的 `installed[]` 同步删除。
  - 锁住:新增 `subos_install_remove_isolation_test.sh`(default 装 1.0.0,tmp 装 2.0.0,tmp remove 后断言 6 项:tmp.workspace 空、default 不变、tmp shim 删、default shim 留、payload 2.0.0 物理删除、payload 1.0.0 保留),作为 E2E-23 加进 CI。

- **`xlings list / use / remove` 全部 subos-aware (PR B,接 0.4.18 的 C2 schema)**
  - 0.4.18 落了 `installed[]` 数据,但 user-visible 的几个命令还是从全局 `versions` DB 读 —— 在新建的空 subos 里跑这些命令体感像啥都没改。这次把 shim + use + list + remove 全部改成读 `Config::workspace_installed()`。
  - **shim 错误提示**(`src/core/xvm/shim.cppm`)三态区分:
    - 当前 subos `installed[]` 里有 program 但没 active → `no active version of '...' in current subos` + `available: <subos-scoped 列表>` + `hint: xlings use ...`
    - 当前 subos 没有但全局 DB 有 → `'X' is not installed in current subos` + `hint: xlings install X`(payload 在别的 subos 装过,这个 subos 需要显式 opt-in)
    - 全局也没有 → `'X' is not installed` + `hint: xlings install X`
  - **`xlings use <pkg>`(无 version)** 默认只列当前 subos `installed[]` 里的版本,标题改成 `<pkg> versions (current subos)`;加 `--all` / `-a` flag 切回全局视图(`<pkg> versions (all subos)`)。subos `installed[]` 为空时报错并提示 `xlings install`,不再丢一个空 panel 让用户猜。
  - **`xlings use <pkg> <ver>` auto-add 语义**:用户在 fresh subos 里 `use` 一个全局 versions DB 已知但当前 subos 没装的版本 → **自动把版本加进 `installed[]` 然后切 active**,不强迫用户先 `install`。payload 全局共享,这个操作零成本。
  - **`xlings list`** 默认只列当前 subos `installed[]` 的包,标题 `Installed packages (current subos):`;加 `--all` / `-a` 看全局 (`Installed packages (all subos):`)。空时给 hint。
  - **`xlings remove <pkg>` 拒绝 fresh subos 的"假 remove"**(`src/core/xim/commands.cppm`):pre-fix 的现象是 `xlings remove gcc` 在没装 gcc 的 subos 里也会"成功",因为 catalog resolve 到 latest declared version、active workspace 是空所以 detach no-op、跨 subos refcount 看到别的 subos 装了 gcc 就走"detach only"分支并报告 `✓ removed (subos: tmp)`。新逻辑在 cmd_remove 入口处先检查 `Config::workspace()` + `Config::workspace_installed()`,**不在当前 subos 直接报错** + 列出哪些 subos 装了这个包 + 给出切 subos 的命令。
  - **`xlings update`** 不需要改 —— 它已经走 `effective_workspace` 拿 `currentActive`,空就报 not installed。已经 subos-aware 了。
  - 影响:`shim.cppm` / `xvm/commands.cppm` / `xim/commands.cppm` / `cli.cppm`。新加 `cmd_list / cmd_list_versions(..., bool all = false)`。
  - 后续:还可以再加 `xlings list <pkg>` 也支持(目前 list 是顶层包列表,不是某 pkg 的版本列表)、`xlings status` 显示 subos 的 installed[] 总览 —— 但这次先把用户报告的 case 全部修掉,别的小 polish 后续 patch 处理。

### 2026-05 (v0.4.18)

- **subos workspace schema 升级:`{active, installed[]}` 形式 (Plan C2,PR A) (#273)**
  - 0.4.18 `2026-05-07-xlings-cmd-subos-binding-analysis.md` 里的 C 系方案落地第一步:**subos** 的 `.xlings.json` 中 `workspace` 每个 target 的 value 从原本的 `"<version>"` 字符串升级为对象形式 `{ "active": "X", "installed": ["X","Y","Z"] }`。**project** `.xlings.json` 不动 —— 它是声明意图(`xxx = { linux = "..." }` 平台条件式)而非运行时状态,无需 `installed`。
  - **解析层(`xvm::subos_workspace_from_json`)三种 value 形态都接收**:
    1. 字符串 `"1.2.3"`(0.4.18 之前的旧文件,自动当作 `active`,`installed[]` 留空,下次 save 自动改写)。
    2. 对象 `{active, installed}`(新形式)。
    3. 对象 `{linux: ..., windows: ..., default: ...}`(平台条件式 fallback,subos 文件本来不会出现这个形态,但用户手编时容忍掉)。
  - **形态 (2) vs (3) 的歧义靠保留 key 区分**(C2 design 里的 Plan 1):object 出现 `active` 或 `installed` 这两个 key → 当作新形式;否则按平台条件解析。两套 schema 永远不会撞到同一份文件里。
  - **写入层(`xvm::subos_workspace_to_json`)永远输出新形式**,且强制不变量:`active` 出现在序列化结果里时,一定也出现在 `installed[]` 里(serializer 自动补)。`installed[]` 排序稳定,降低 git diff 噪音。
  - **GC refcount 修正(`installer::is_version_referenced_anywhere_`)**:之前只检查"某 subos 的 active == version"。新形式下,某 subos 可能保留 X 在 `installed[]` 但 active 是别的版本,这种 case 之前会被误判为"无人用",触发 GC 把 payload 删掉。现在跨 subos refcount 同时遍历 active 和 `installed[]`,**两者任一命中就视为被引用**。
  - **install / remove 维护 `installed[]`**:
    - `process_xvm_operations_` add(program 类型) → 把 `ver_key` 加进当前 subos 的 `installed[]`(去重);active pointer 仍按旧规则(无 active 或 `--use` 时切到新版本)。
    - `process_xvm_operations_` remove(无论是 user-initiated remove 还是 install-time upgrade) → 同步从 `installed[]` 抹掉 `ver_key`(再次防御:detach_current_subos_ 通常已经先抹了,但 install upgrade 路径不走 detach)。
    - `detach_current_subos_` → 永远先把 `version` 从 `installed[]` 移除;如果它正好是 active,则做 sysroot 拆除并**自动 fallback 到 `installed[]` 里剩余最高版本**(列表是 sorted ascending,`back()` 即最高);剩余为空才清空 active pointer。**避免"删了 active 之后命令就完全报错"的硬中断**。
  - 影响范围:`config.cppm`、`xvm/db.cppm`、`xvm/types.cppm`(新增 `WorkspaceInstalled` 类型 + `SubosWorkspace` 结构)、`xim/installer.cppm`、`profile.cppm`(GC scan 同时看两边)。**没有 schema 强制升级步骤** —— 旧文件第一次 save 才被改写。
  - **compat 入口**:`xself::compat::v0_4_18::kSchemaForm` 是空 sentinel,文档化此次 schema migration,挂在 `removal_target: 0.6.0`(到时把 `subos_workspace_from_json` 里支持字符串形态的分支也一起删掉)。
  - **db.cppm 顺手清理**:之前 `workspace_from_json` 内嵌一份 `current_platform_key` lambda,硬编码 `linux` / `macosx` / `windows`,这次新加 `subos_workspace_from_json` 又复制了一份。两个都改成直接用 `platform::OS_NAME`(per-OS 模块已经导出的常量,`src/platform/{linux,macos,windows}.cppm`)。重复实现合并到 `resolve_platform_workspace_value_` 一个 helper 里。
  - 测试:新增 `subos_workspace_c2_schema_test.sh`(4 个 scenario,覆盖 lazy migration、active+installed 并存、save invariant、active-less object 的容忍读取);**原 7 个 e2e 测试**里把 workspace value 当字符串读的 python helper 全部升级成兼容 dict / str 两种(`remove_self_guard`、`remove_multi_version`、`xlings_self_replace`、`self_doctor`、`update_package`、`cli_target_compat`、`install_idempotent`)。
  - **下一步(后续 release)**:`xlings list / use / update` 加 subos-aware 过滤(只显示当前 subos 的 `installed[]` 视图),把这次落下来的数据模型用起来。

- **Windows CI / release xrepo 缓存路径修复 (#273)**
  - 现象:Linux / macOS 的 `Cache xrepo packages` 步骤缓存命中正常(每次 ~5MB / ~3MB),但 **Windows 这个缓存从来没有上传过**(`gh api repos/.../actions/caches` 里 `xmake-pkgs-Windows-*` 一条都没有)。导致 Windows 每次 PR / release 都从源码重编 libarchive + zstd + bzip2 + lz4 + zlib via MSVC,**`Configure xmake` 单步耗时 ~8 分钟**。
  - 根因:xmake 的 global dir 在 Windows 上是 `%LOCALAPPDATA%\.xmake`(xmake 源码 `_global_dir()`:`LOCALAPPDATA → APPDATA → USERPROFILE`,然后拼 `.xmake`)。原 workflow 缓存路径 `~\.xmake\packages` 在 GitHub Actions runner 上展开成 `%USERPROFILE%\.xmake\packages` —— 一个 xmake 根本不写入的目录。所以缓存步骤每次都"成功"但实际什么都没缓存。
  - 修复:`xlings-ci-windows.yml` 和 `release.yml` 的 Windows 缓存路径都从 `~\.xmake\packages` 改成 `~\AppData\Local\.xmake\packages`。Linux / macOS 的路径不动(那两个平台 `~/.xmake/packages` 就是 xmake 实际用的位置)。冷启动第一次仍然 ~8 分钟,之后命中缓存应该和 Linux/macOS 相当(~30 秒)。

- **prompt marker subos 名色调:bold green → bold magenta(8-color "经典紫")(#272)**
  - 用户反馈 v0.4.17 的 bold-green 名字跟周围 prompt 颜色站位不协调,绿色和 entering message 的 magenta、switched message 的 cyan 混在一起会让 prompt 看着乱。
  - 三种 shell(bash/zsh、fish、PowerShell)同步切到 SGR `\033[1;35m`(8-color magenta,大多数终端 theme 渲染为低饱和度紫色)。括号 `[xsubos:` 和 `]` 仍是 slate-400 灰。`NO_COLOR` / `TERM=dumb` 仍走纯文本 fallback。
  - `profile_resources::kVersion` 8 → 9。已装 v8 用户(只有 v0.4.17 极短窗口期内升级过的人)下次 xlings 调用时通过 `compat::v0_4_17::auto_upgrade_profiles_if_stale` 自动覆盖。

- **shim 报错 hint 区分 "未安装" vs "未激活"(用户 0.4.17 反馈)**
  - 旧:`gcc` 命令(orphan shim) → workspace 找不到 → 永远提示 `xlings use gcc <version>`,但 gcc 可能根本没装,使用 `xlings use` 没用。
  - 新:shim 拿到空 active 后,先查全局 versions DB:
    - 0 个版本 → `[error] 'gcc' is not installed` + `hint: xlings install gcc`
    - ≥1 个版本 → `[error] no active version of 'gcc' in current subos` + `available: <list>` + `hint: xlings use gcc <version>`
  - 仅改报错文案,不动 install/remove/use 的语义,零回归风险。

- **subos-binding 设计文档(`docs/plans/2026-05-07-xlings-cmd-subos-binding-analysis.md`)**
  - 把"`xlings list / use / update / install / remove` 与 subos 绑定"的真实数据模型讲清楚:`versions DB` 全局、`workspace` per-subos、还缺一个 per-subos `registered` 集合。
  - 列出三种修复方向:**A**(只改 shim UX,本 patch 已落)/ **B**(installer 收紧 versionless `xvm.remove` 处理,0.4.19 候选)/ **C**(per-subos `registered` 集合,真正的 subos 作用域,0.5.0 minor)。
  - 用户报告的 `xlings remove gcc` 后 `gcc` 命令仍报错的根因(versions DB 是全局视图,subos 没有 registered 集合,导致 shim 留存 + workspace auto-switch 走偏)文档化,作为 0.5.0 重构的 baseline。

### 2026-05 (v0.4.17)

- **shell-level subos 切换 + auto-upgrading profile + TUI 渲染统一 (#269)**
  - `xlings subos use foo` 默认从"改全局符号链接"改成 **spawn 一个新交互 shell**(POSIX `execl` / Windows `CreateProcess+WaitForSingleObject`),env 里有 `XLINGS_ACTIVE_SUBOS=foo`,`exit` 直接回到原 shell,**两个 terminal 终于可以用不同 subos 互不干扰**。原"持久 + 全局"行为通过显式 `--global` flag 保留。`--shell <kind>` 是给测试 / 高级用户的 eval-able snippet 输出,不在用户帮助里出现。
  - 嵌套策略:同 subos 二次 use → 一行 `› already in subos NAME` 退 0,**不重复 spawn**。不同 subos use → `▾ nesting subos FROM -> TO  ('exit' returns to FROM)` 提示后再 spawn。spawn 模型下"先退出当前 subos 再进新的"在物理层做不到(子进程无法操控父 shell exec(2)),所以只做嵌套 + 友好提示。
  - shell profile 三种 shell 都换成 `XLINGS_BIN="$XLINGS_HOME/subos/${XLINGS_ACTIVE_SUBOS:-current}/bin"`(env 优先,没设就回退到 `subos/current` 全局符号链接,保留原有兜底);Config 端的 `update_effective_paths_()` 也对齐同样的 project > env > global 优先级链,保证 `xpkg install` / PATH 看到的是同一个 active subos。
  - profile 多了一个 prompt marker:进入 subos shell 后 prompt 自动加 `[xsubos:NAME]` 前缀(cyan + bold name,bash/zsh/fish/pwsh 各自原生上色),色盲 / 不支持 ANSI 的终端通过 `NO_COLOR` 或 `TERM=dumb` 退到纯文本。再次 source profile **不会**叠加多个 marker。
  - profile 内嵌到 C++ 模块:新增 `src/core/xself/profile_resources.cppm` 持有 bash / fish / pwsh 三份 payload + `kVersion` 常量,`init.cppm` 通过 `import` 消费;旧的 `config/shell/xlings-profile.{sh,fish,ps1}` 文件删掉(那三份和 init.cppm 内嵌的 raw literal 之前差 1 字节都没人发现)。**单一来源**。
  - profile 升级机制:`# xlings-profile-version: <N>` 标记 + `write_or_upgrade_profile_`。版本递进 v1 → v5(env override → 加 prompt marker → marker 改名 `[xsubos:...]` → 加 ANSI 颜色)。**新装走 fresh write,旧装走自动升级且保留用户在 marker 之外的编辑**。
  - **`xlings update xlings` 后老用户也能拿到新 profile**:新 binary 启动时(每次 xlings 调用)跑 `xself::compat::v0_4_17::auto_upgrade_profiles_if_stale`,版本不一致就静默重写。一次性的成本,旧 binary 的 `xlings update xlings` 路径只翻 xvm 指针、不调 `ensure_home_layout`;新 binary 自检自愈。
  - TUI 输出统一为单行模板,四种事件 + 四种颜色:`▸ switched to subos NAME [global] (DIR)` cyan / `▸ entering subos NAME (exit to leave)` magenta / `› already in subos NAME` gray / `▾ nesting subos FROM -> TO ('exit' returns to FROM)` amber。`[global]` tag 把"持久"和"per-shell ephemeral"显式区分。
  - **compat 模块整理**:`src/core/xself/compat_0_4_8.cppm` → `compat.cppm`,每个 compat 块归到 `vX_Y_Z` 子命名空间,header 注释含 `removal_target` 字段。当前两组:`v0_4_8::*`(legacy alias 迁移,drop in 0.6.0)+ `v0_4_17::*`(profile auto-upgrade,permanent self-heal)。删除某个 compat 是删一整块 namespace 的一次性操作,所有 caller 立刻编译报错暴露出来。
  - 测试:新增 `subos_shell_level_test.sh`(14 个 scenario,含 prompt 颜色 + 嵌套 + 并行 sub-shell 隔离 + PATH dedup)和 `subos_profile_upgrade_test.sh`(4 个,含 v1 legacy 升级 + 同版本保留用户编辑)。
  - 现有测试一处适配:`xlings subos use NAME` 默认行为变更 → `release_subos_smoke` / `subos_events` / `subos_payload_refcount` 显式补 `--global`。

### 2026-05 (v0.4.16)

- **修复：对未安装的包再次 `xlings remove` 仍打印 "✓ removed" 的循环 (#266)**
  - `src/core/xim/commands.cppm`：`cmd_remove` 在用户不带版本号、active 绑定又为空时,曾把解析降级到 catalog 配方里的"声明最高版本",再把 `!installed` 的判定门控在 `resolvedToDefiniteVersion` 上 —— 注释里那句"留给 installer.uninstall 处理"是错的,uninstall 在空 DB 上会跑完所有 no-op 仍报 success,把用户卡在重复卸载循环里。
  - 修复双管齐下:active 为空时先回查 xvm DB(`xvm::pick_highest_version`,与 multi-version remove 的兜底复用同一函数);移除 `resolvedToDefiniteVersion` 守门,只要 catalog 解出的 match 不在磁盘上就 warn `"<pkg>@<ver> is not installed"` 并 exit 0,不再 emit `remove_summary`。
  - `tests/e2e/remove_multi_version_test.sh` 加 Scenario 4:第三次卸完最后一个版本后再 `remove`,断言 exit 0、无 `removed.*subos` 摘要、有 `not installed` 诊断、DB / workspace / store 仍为空。

### 2026-05 (v0.4.15)

- **下载缓存：sha256 缺失时改用 HEAD 探测，不再每次重下 (#TBD)**
  - `src/core/xim/downloader.cppm`：包索引中只声明 `url` 不声明 `sha256` 的条目（fixture 索引里约 8%；实际 `node.lua` / `nvm.lua` 之类 `_linux_url` helper 拼出来的 URL 占比更高）原来每次 `xlings install` 都会重下整包。新增 HEAD-fallback 缓存路径：`fs::file_size` 与服务端 `Content-Length` 比对 + `<destFile>.meta` sidecar 记录的 `Last-Modified` / `ETag` 与本次 HEAD 响应比对，命中则跳过下载。HEAD 失败（离线/服务端拒 HEAD）回退"文件存在即信"，airline-friendly。
  - sha256 路径在不匹配时主动 `fs::remove` 旧文件，预防将来 tinyhttps 启用 Range/resume 时拼出腐烂文件。
  - `src/libs/tinyhttps.cppm`：`query_remote_meta()` 返回完整 `RemoteFileMeta { contentLength, lastModified, etag, ... }`；老的 `query_content_length()` 改成薄包装。
  - `tests/unit/test_main.cpp::XimDownloaderTest::MetaSidecarRoundTrip` 锁定写入 + 读取 + 缺失 + 畸形行容错。

- **bump：mcpplibs-xpkg 0.0.37 → 0.0.38**
  - 带入 libxpkg 的 `os.dirs` glob 修复（POSIX 下 `ls -d "<pat>"` 双引号会让 shell 跳过 glob 展开，导致 `os.dirs("…/v*")` 静默返回空表）。

### 2026-04 (v0.4.3)

- **下载器：自动识别系统代理 (#222)**
  - `src/libs/tinyhttps.cppm` 增加 env 解析：`HTTPS_PROXY` / `HTTP_PROXY` / `ALL_PROXY`（含小写变体）按 libcurl 优先级生效；`NO_PROXY` 支持 exact / dotted-suffix / bare-suffix / `*` 通配。
  - 命中代理时通过 `log::debug("tinyhttps: using proxy ... for ...")` 输出，全局 `-v` 可见。
  - 复用 `mcpplibs::tinyhttps` 0.2.0 已内建的 HTTP CONNECT 隧道，无需上游改动。
  - 9 个 `TEST(Proxy, …)` 单测锁定行为矩阵。

- **TUI：Linux/macOS/Windows 图标一致 (#221)**
  - 移除 `src/ui/theme.cppm` 的 `#ifdef _WIN32` ASCII fallback：三个平台用同一组 BMP 图标 `○ ↓ ▾ ⊕ ✓ ✗ › ▸ ◆`，把 `⟐` `⚙` 这种缺字形的偏门字符换掉。
  - 新增 `tests/unit/test_main.cpp::ThemeIcons` 4 个单测：逐字节锁定 + 防 ASCII fallback + 强制 3-byte BMP UTF-8 + 渲染流抓 stdout 验字节。
  - 配套 `tests/e2e/tui_utf8_test.sh` 在 Linux/macOS CI 跑端到端编码验证。
  - macOS / 文档加 Windows 字体推荐段。

- **架构：移除 agent 子系统 (#220)**
  - 删除 `src/agent/` 整目录（14 个 .cppm，~3000 行）+ `src/libs/` 中 8 个 agent-only 模块。
  - 删除 `mcpplibs-llmapi` 外部依赖；`mcpplibs-tinyhttps` 保留（xim 下载器使用）。
  - 51 files changed, +119 / -16768 行；xlings 回归到纯包管理器 + xvm 运行时定位。

- **修复：`xlings remove <pkg>` 不再误删整包版本表 (#219)**
  - `src/core/xim/installer.cppm` 修复：当 uninstall hook 发出无版本号的 `xvm.remove(name)` 时，旧路径 `versions_mut().erase(name)` 把整个包条目擦掉（其它已装版本变孤儿）；现在改为用外层 resolved 的 `detachVersion` 兜底，`xvm::remove_version` 精确删一个；删后若有剩余版本且被删的是 active，自动按 semver 降序切到最高。
  - 新增 `xvm::pick_highest_version()`。
  - `tests/e2e/remove_multi_version_test.sh` 端到端回归（hermetic、私有 fixture index、Linux + macOS CI 都跑）。

- **CI：macOS bootstrap xlings v0.3.2 → v0.4.2**
  - `.github/workflows/xlings-ci-macos.yml` 和 `release.yml`：旧 v0.3.2 在 GitHub Actions 非 TTY 下载 LLVM 时 progress 输出被吞，导致 7+ 分钟无日志看似卡死；v0.4.2 进度行正常输出，CI 不再误判。

### 2026-03 (v0.4.0)

- **xvm C++ 集成：消除 Rust xvm 依赖**
  - Lua xvm 模块从 shell-out 调用 Rust `xvm` 二进制改为收集 `_XVM_OPS` 操作表
  - C++ 侧在 hook 执行后通过 `PackageExecutor::xvm_operations()` 读取并统一处理
  - 新增 `xvm.setup()` / `xvm.teardown()` 高层 API（一次调用注册程序/库/头文件）
  - VData 扩展 `includedir` / `libdir` 字段，支持头文件和库的 symlink 追踪
  - 头文件安装改为 symlink 方式（`install_headers()`），版本切换时自动切换
  - 卸载流程增强：自动清理 VersionDB 条目和 workspace 引用
  - 修复 shim.cppm 跨平台 PATH 分隔符问题（使用 `platform::PATH_SEPARATOR`）
  - 修复 commands.cppm 跨平台 symlink 问题（Windows 使用 junction/hardlink 回退）
  - 移除 Rust xvm 源码（core/xvm/Cargo.toml, src/, shim/, xvmlib/）
  - 移除 CI/release 脚本中所有 Rust 构建步骤和 xvm 二进制验证
  - 新增 7 个单元测试（VData 新字段 + 头文件 symlink 操作），总计 81 个测试

- **xim 核心模块 C++ 重写**
  - 将 xim 包管理器核心从 Lua/xmake 子进程架构迁移到原生 C++23 模块实现
  - 消除 `xmake xim -P ...` 子进程调用，install/remove/search/list/info/update 命令全部在 C++ 内完成
  - 新增 7 个 C++23 模块：`xlings.xim.types`、`xlings.xim.repo`、`xlings.xim.index`、`xlings.xim.resolver`、`xlings.xim.downloader`、`xlings.xim.installer`、`xlings.xim.commands`
  - 基于 libxpkg (C++ 库) 实现包索引构建、搜索、版本匹配和包加载
  - 新增 DAG 依赖解析器：DFS 拓扑排序、循环检测、已安装跳过
  - 新增并行下载器：`std::jthread` 并发控制、SHA256 校验、镜像支持
  - 安装编排器通过 libxpkg `PackageExecutor` 运行 Lua hook（install/config/uninstall）
  - CLI 直接调用 `xim::cmd_*` 函数，不再依赖 xmake 运行时
  - 51 个单元测试覆盖所有模块（类型、索引、解析器、下载器、安装器、命令）
  - 解决多个 GCC 15.1.0 C++23 模块 bug（ICE、链接符号缺失、运行时格式错误）

### 2026-02

- **Bug fixes: xim task / xvm path quoting / elfpatch tool detection**
  - Fix `xlings install`/`search` failing with "invalid task: xim" on Windows when `find_xim_project_dir` falls back to source tree root (which lacks `task("xim")` definition). Now falls through to `~/.xlings` installed layout automatically.
  - Fix `xvm add --path` breaking when path contains spaces (e.g. macOS `/Applications/My App/`). The `--path` argument is now properly quoted, consistent with `--alias`, `--type`, etc.
  - Replace `find_tool()` in elfpatch with direct execution probe (`_try_probe_tool`). Tools like `install_name_tool`/`otool` on macOS and `patchelf`/`readelf` on Linux are now detected by actually running them, with `try{}` catching failures gracefully and printing actionable hints.
  - Added `tests/e2e/bugfix_regression_test.sh` covering all three fixes.

- **xlings self install 安装逻辑优化**
  - data/subos 保留策略改为「直接不删除、选择性不覆盖」，不再使用备份/恢复
  - 升级时 data/subos 完全保留不合并，避免 subos 损坏
  - 移除「保留缓存数据」交互提示，data/subos 自动保留
  - 优化用户提示与打印布局

## 2025

### 2025-08

- **发布xvm-0.0.5 + xpkg/xscript复用机制**
  - 增加了库类型的多版本管理机制, 以及`xvm info`详情查询 - [PR](https://github.com/d2learn/xlings/pull/108) - 2025/8/16
  - xpkg/xscript 即是包也是程序(脚本)的复用机制 (示例: [musl-cross-make](https://github.com/d2learn/xim-pkgindex/blob/main/pkgs/m/musl-cross-make.lua)) - [PR](https://github.com/d2learn/xlings/pull/109) - 2025/8/14
- **文档:** 初步完善文档: [快速开始](https://xlings.d2learn.org/documents/quick-start/one-click-install.html)、[常用命令](https://xlings.d2learn.org/documents/commands/install.html)、 [xpkg包](https://xlings.d2learn.org/documents/xpkg/intro.html)、[参与贡献](https://xlings.d2learn.org/documents/community/contribute/issues.html) - [PR](https://github.com/d2learn/xlings-docs/commit/122b060855e4c41cd7f95801f2656bca0a5a6fc1) - 2025/8/9


### 2025-07

- **代码优化:** 修复一些bug并优化相关代码、适配macos - [commits](https://github.com/d2learn/xlings/commits/main/?since=2025-07-01&until=2025-07-31) - 2025/7

### 2025-06

- 跨平台: 初步支持MacOS平台、xim添加冲突解决功能(xpkg的`mutex_group`字段实现) - 2025/6

### 2025-05

- 新功能: 增加包索引网站、支持多语言i18n - 2025/5

### 2025-02

- d2x: 重构公开课/教程项目相关命令, 形成独立的d2x工具 - [PR](https://github.com/d2learn/xlings/pull/79) - 2025/2/19

### 2025-01

- xim: 增加archlinux上aur的支持 - [PR](https://github.com/d2learn/xlings/pull/67) - 2025/1/10
- xvm: 增加版本管理模块 - [文章](https://forum.d2learn.org/topic/62) / [PR](https://github.com/d2learn/xlings/pull/60) - 2025/1/1

## 2024

- xpkg增加自动匹配github上release的url功能 - [文章](http://forum.d2learn.org/post/208) - 2024/12/30
- xlings跨平台短命令 - [视频](https://www.bilibili.com/video/BV1dH6sYKEdB) - 2024/12/29
- xinstall模块: 重构&分离框架代码和包文件 - [包索引仓库](https://github.com/d2learn/xim-pkgindex) / [PR](https://github.com/d2learn/xlings/pull/49) -- 2024/12/16
- xinstall功能更新介绍 - [文章](https://forum.d2learn.org/topic/48) / [视频](https://www.bilibili.com/video/BV1ejzvY4Eg7/?share_source=copy_web&vd_source=2ab9f3bdf795fb473263ee1fc1d268d0)
- 增加DotNet/C#和java/jdk8环境的支持
- 增加windows模块和安装器自动加载功能, 以及WSL和ProjectGraph的安装支持 - [详情](http://forum.d2learn.org/post/96)
- 软件安装模块增加deps依赖配置和"递归"安装实现
- 初步xdeps项目依赖功能实现和配置文件格式初步确定
- install模块添加info功能并支持Rust安装