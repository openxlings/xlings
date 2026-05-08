# subos sandbox 模式 — 设计方案

> 把 subos 从"PATH/env 隔离"扩展到"文件系统视图隔离",通过 proot 实现,无 sudo,Linux 专属。
>
> 这是 0.4.x 系列 subos 设计演化的最终形:`global` / `shell-level` 两种现有模式 + 新增第三种 `sandbox` 模式。
>
> 配套先前的探索:
> - `2026-05-05-subos-tiered-mode-architecture.md`(四档隔离的初稿)
> - `2026-05-06-subos-mode-implementation-details.md`(四档实现细节)
> - 本文是经过多轮收敛后的实现版,**显著简化**了之前的方案。

---

## 1. 设计目标

| 目标 | 说明 |
|------|------|
| **真正的 fs 隔离** | sandbox 内 `~/.gitconfig`、`~/.npm/`、`~/.cache/` 等 dotfile 完全独立于宿主,跑脏了删 sandbox 即可 |
| **零 sudo** | 不依赖 setuid 二进制、不依赖 user-namespace、不依赖 AppArmor 配合 |
| **xlings 是唯一包管理器** | sandbox 内不带 apt/apk/dnf,只通过 xlings + xim-pkgindex 安装东西 |
| **跨发行版一致** | sandbox 行为只跟 xlings 自己的 xpkg 生态有关,跟宿主 distro 无关 |
| **最小启动开销** | sandbox 创建后 < 1 KB 物理目录(只是几个空目录 + 几行 /etc + 一个 shell shim) |
| **payload 共享** | 所有 sandbox 共享宿主全局 xpkgs 池(几 GB 工具链不重复) |
| **现有两种 mode 不动** | `global` / `shell-level` 行为零回归;只新增 sandbox 类型 |

---

## 2. 三种 subos 类型对比

| | global | shell-level | **sandbox**(新)|
|---|---|---|---|
| **创建** | `xlings subos new foo` | 同上 | `xlings subos new foo --sandbox-shell <xpkg>` |
| **进入** | `xlings subos use foo --global` | `xlings subos use foo` | `xlings subos use foo` |
| **机制** | 改 `~/.xlings/.xlings.json activeSubos` 全局指针 | spawn 子 shell + `XLINGS_ACTIVE_SUBOS` env | **proot -R rootfs + bind 系统资源** |
| **fs 视图** | 宿主 / 不变,PATH 优先 subos bin | 同 global | **`/` = subos rootfs**,选择性 bind 宿主资源 |
| **`$HOME`** | 宿主用户主目录 | 宿主用户主目录 | **subos 私有 `/root`** |
| **dotfile** | 共享宿主 | 共享宿主 | **完全独立** |
| **`/usr`、`/etc`、`/lib`** | 宿主 | 宿主 | sandbox 内的(默认空,用户用 xlings 装)|
| **是否 root** | 你是宿主 uid 1000 | 你是宿主 uid 1000 | **proot -0:伪 root(uid 0)** |
| **平台支持** | 全平台 | 全平台 | **仅 Linux**(macOS/Windows 报错)|
| **sandbox-shell 字段** | 无 | 无 | **必填** |
| **后端** | env spawn shell | env spawn shell | proot |

**关键不变量**:三种类型共享同一份 subos schema(`workspace` / `installed[]` 等 0.4.18 起的 C2 字段),只是多一个可选的 `sandbox-shell` 字段当类型识别。

---

## 3. sandbox 文件系统映射

### 3.1 静态布局(创建后,未进入)

```
~/.xlings/subos/<name>/                        # 物理目录(宿主上)
├── .xlings.json                              # subos config(含 sandbox-shell)
├── bin/                                       # 用户装的工具 shim
│   └── <shell>                               # subos new 时由 xim:<shell> 装的
├── etc/                                       # 最小 OS 骨架(由 xlings 写)
│   ├── passwd                                # 一行: root:x:0:0:root:/root:/bin/sh
│   ├── group                                 # 一行: root:x:0:
│   ├── hosts                                 # 一行: 127.0.0.1 localhost
│   └── nsswitch.conf                         # 一行: hosts: files dns
├── root/                                      # /root (= $HOME)
└── tmp/                                       # /tmp
```

**总固定开销 ~80 字节配置 + shell xpkg 体积**(dash 150 KB / bash 1.5 MB)。

### 3.2 动态视图(进入后,proot 看到的)

```
INSIDE sandbox             OUTSIDE                                    用途
─────────────────          ─────────                                  ────
/                       =  ~/.xlings/subos/<name>/                    sandbox rootfs(本身)
/bin/                   =  ~/.xlings/subos/<name>/bin/                shim + 用户工具(per-sandbox)
/etc/passwd 等          =  ~/.xlings/subos/<name>/etc/...             OS 骨架配置
/root/                  =  ~/.xlings/subos/<name>/root/               $HOME(per-sandbox,完整 dotfile 隔离)
/tmp/                   =  ~/.xlings/subos/<name>/tmp/                /tmp(per-sandbox)
/.xlings.json           =  ~/.xlings/subos/<name>/.xlings.json        subos 自己的 config

/xlings/                ←  bind ~/.xlings/                            ★ 整个 xlings 宇宙
/xlings/bin/xlings      =  ~/.xlings/bin/xlings                       xlings 二进制
/xlings/data/xpkgs/     =  ~/.xlings/data/xpkgs/                      全局 payload 池
/xlings/.xlings.json    =  ~/.xlings/.xlings.json                     全局 versions DB

/proc /sys /dev         ←  bind host                                  内核接口共享
/etc/resolv.conf        ←  bind host                                  DNS 共享(覆盖 sandbox 自己的 etc/resolv.conf)
```

### 3.3 proot 命令面

```bash
proot -0 \
  -R ~/.xlings/subos/<name> \                  # rootfs
  --bind=/proc:/proc \                         # kernel 接口(3 条)
  --bind=/sys:/sys \
  --bind=/dev:/dev \
  --bind=/etc/resolv.conf:/etc/resolv.conf \   # DNS(1 条)
  --bind=$HOME/.xlings:/xlings \               # ★ 整个 xlings home(1 条)
  --setenv XLINGS_HOME=/xlings \
  --setenv XLINGS_ACTIVE_SUBOS=<name> \
  --setenv HOME=/root \
  --setenv USER=root \
  --setenv PATH=/bin:/xlings/bin:/usr/bin:/bin \
  -- /bin/<shell>
```

**5 条 bind**:1 条 xlings home + 4 条 host 资源。`-0` flag 让程序看到自己是 root(uid 0)。

---

## 4. `.xlings.json` schema

普通 subos(global / shell-level,**零变更**):
```jsonc
{
  "workspace": { ... }              // C2 schema (0.4.18+)
}
```

sandbox subos(新增):
```jsonc
{
  "sandbox-shell": "/bin/dash",     // ★ 唯一新字段;路径是 subos 内部视图
  "workspace": {
    "dash": { "active": "0.5.12", "installed": ["0.5.12"] }
  }
}
```

**单字段触发整个 sandbox 路径**:有 `sandbox-shell` → sandbox 类型;无 → 走 global / shell-level 现有路径。

---

## 5. 用户身份(root 伪装)

### 5.1 为什么 `-0`

1. 多数工具 `geteuid() != 0` 直接报错(npm 装含 native module 的包、某些 *-config 工具)
2. 业界惯例:Termux / proot-distro / Toolbox / Distrobox 全部默认 `-0`
3. 文件 ownership **实际不变** —— proot 对 syscall 撒谎,磁盘上仍是宿主真实 uid;退出 sandbox 后用户能正常读写所有文件
4. xlings 自己**不需要**真 root,`-0` 只是讨好其他工具

### 5.2 必备的最小 `/etc/passwd`

`proot -0` 内部 `getpwuid(0)` 仍会查 `/etc/passwd`。文件不存在或没 root 行 → `whoami` 等会怪错。所以 sandbox 创建时写两个迷你文件:

```
# /etc/passwd  (40 字节)
root:x:0:0:root:/root:/bin/sh

# /etc/group  (10 字节)
root:x:0:
```

### 5.3 `$HOME = /root`(对齐惯例)

| `$HOME` | 利弊 |
|---------|------|
| **`/root`**(选定)| Linux 惯例:uid 0 用户的 home。`cd ~` → `/root`,`~/.gitconfig` → `/root/.gitconfig`。`/home` 在 sandbox 内是空 |
| `/home` (备选)| 单一目录,但违反 root 用户 home 在 `/root` 的惯例 |

dotfile 落点示例(都在 `subos/<name>/root/...` 物理目录下):

| 程序写的路径 | sandbox 内 | 物理位置 |
|------------|-----------|--------|
| `~/.gitconfig` | `/root/.gitconfig` | `subos/<name>/root/.gitconfig` |
| `~/.cache/pip/...` | `/root/.cache/pip/...` | `subos/<name>/root/.cache/pip/...` |
| `~/.cargo/registry/...` | `/root/.cargo/registry/...` | `subos/<name>/root/.cargo/registry/...` |
| `~/.ssh/` | `/root/.ssh/` | `subos/<name>/root/.ssh/` |

**完整 dotfile 隔离**,sandbox 跑脏了 `xlings subos remove <name>` 一键清零,宿主主目录一尘不染。

---

## 6. 命令面(V1.1 范围)

### 6.1 创建

```bash
xlings subos new <name> --sandbox-shell <xpkg>
```

行为:
1. 创建 `~/.xlings/subos/<name>/` 物理目录
2. 在 sandbox 内安装 `<xpkg>`(走标准 xpkg 流程,落到 `subos/<name>/bin/<shell>`)
3. 写 `subos/<name>/etc/{passwd,group,hosts,nsswitch.conf}`
4. 创建 `subos/<name>/{root,tmp}/` 空目录
5. 写 `.xlings.json`,标 `sandbox-shell` 字段为安装的 shell 路径

### 6.2 进入

```bash
xlings subos use <name>
```

行为:
1. 读 `subos/<name>/.xlings.json`
2. 有 `sandbox-shell` → 走 sandbox 路径(本设计)
3. 无 → 走现有 shell-level 路径(零回归)

sandbox 路径具体步骤:
1. 平台检查:非 Linux → 报错 `sandbox is only supported on Linux`
2. proot 探测(顺序):
   - `~/.xlings/data/xpkgs/xim-x-proot/<ver>/bin/proot`(未来 xpkg 路径)
   - `~/.xlings/runtimedir/proot`(本地缓存)
   - `which proot`(系统 PATH)
3. 都没找到 → **自动从 https://proot.gitlab.io/proot/bin/proot 下载**到 `~/.xlings/runtimedir/proot`(2 MB 一次性,SHA256 验证)
4. 构造 proot argv(见 §3.3)
5. `posix_spawn(proot)`,父 xlings 阻塞 wait
6. 用户 `exit` → proot 退出 → 父进程返回

### 6.3 嵌套检测

xlings 内部检测 `XLINGS_ACTIVE_SUBOS` env + 当前在 sandbox 中(可通过 `/.xlings.json` 是否含 `sandbox-shell` 字段判断)→ **拒绝嵌套**:

```
$ xlings subos use mybox        # 已经在 sandbox 里
error: cannot enter sandbox from inside another sandbox
hint: type 'exit' first to leave the current one
```

---

## 7. 平台与限制

### 7.1 仅 Linux

非 Linux 上 `--sandbox-shell` 创建立即拒绝:

```
$ xlings subos new mybox --sandbox-shell xim:dash    # macOS / Windows
error: --sandbox-shell is only supported on Linux (current: macosx)
hint: use --no-shell or omit for default subos type (cross-platform)
```

理由:proot 依赖 ptrace + Linux syscall 拦截,macOS 受 SIP 限制,Windows 完全没这套。**不静默降级**(欺骗用户以为有隔离)。

### 7.2 sandbox 不是什么

| 东西 | sandbox 是吗 |
|------|----|
| Docker container(完整 namespace + cgroup) | ✗ — 没 PID/net 隔离,共享宿主进程视图和网络栈 |
| VM(独立内核) | ✗ — 共享宿主内核,`uname -r` 出宿主 |
| chroot(裸 fs 切换) | sandbox 是它的"无 sudo + 路径选择性 bind"升级版 |
| systemd-nspawn / lxc | ✗ — 那些要 cgroup + user-ns |

**用 sandbox 做**:隔离 build 环境、跨 distro 测试、清理 dotfile 实验、给 LLM agent 干净操场
**别用 sandbox 做**:跑系统服务、性能敏感的 IO-heavy 任务、需要 PID/net 隔离

### 7.3 限制清单

| 操作 | sandbox 内表现 |
|------|------------|
| `mount /dev/...` | 失败(真 mount 要 CAP_SYS_ADMIN)|
| `insmod` | 失败 |
| `chown` 给其他 uid | 报告成功,实际不生效(proot 撒谎)|
| `hostname foo` | 没影响内核 hostname |
| `ps aux` | **看到宿主全部进程**(没 PID-ns)|
| `ip a` | **看到宿主网卡**(没 net-ns)|
| `apt install` / `apk add` | 命令不存在(没装)|
| `xlings install <xpkg>` | ✓ 唯一包管理路径 |
| `git clone https://...` | ✓ 跟宿主网络共享 |
| 性能敏感 syscall-heavy | **慢 30-50%**(proot ptrace 开销)|

---

## 8. 实现拆解

### 8.1 模块改动

```
src/core/subos.cppm                       (改) facade,sandbox 分支
src/core/subos/sandbox.cppm               (新) sandbox 入口逻辑(创建 + 进入)
src/core/subos/proot.cppm                 (新) proot 探测 + 自动下载 fallback
```

### 8.2 关键函数

```cpp
// src/core/subos/sandbox.cppm

// 创建 sandbox 类型 subos:写 etc/* 模板 + 装 shell xpkg + 写 config
std::expected<void, std::string>
create_sandbox_subos(const std::string& name, const std::string& shell_xpkg);

// 进入 sandbox:probe proot + 构造 argv + posix_spawn
int enter_sandbox(const std::string& name);

// 检测当前是否已经在 sandbox 内(env + config 双重检测)
bool currently_in_sandbox();
```

```cpp
// src/core/subos/proot.cppm

// 按优先级顺序探测 proot 二进制位置;缺失则自动下载到 runtimedir
std::expected<std::filesystem::path, std::string>
locate_or_fetch_proot();
```

### 8.3 proot 自动下载流程

```
1. 检查 ~/.xlings/runtimedir/proot 是否存在 + 可执行
2. 没有 → curl/wget 下载 https://proot.gitlab.io/proot/bin/proot
3. SHA256 验证(预置已知 SHA)
4. chmod 0755
5. 缓存供后续 sandbox 使用
6. 失败 → 报错给用户:`xlings subos use <name>` 失败,提示手动装 proot 或装系统包
```

### 8.4 LOC 估计

| 改动 | LOC |
|------|-----|
| `subos new --sandbox-shell` 实现(install + etc 模板 + config)| ~80 |
| `subos use` 入口加 sandbox 分支 | ~30 |
| `proot.cppm`(probe + auto-fetch + SHA 验证)| ~120 |
| `sandbox.cppm`(argv 构造 + posix_spawn)| ~100 |
| `.xlings.json` schema 加 `sandbox-shell`(向后兼容)| ~20 |
| 嵌套检测 | ~15 |
| etc/* 模板字符串(常量)| ~30 |
| e2e 测试 | ~150 |

**总 ~545 LOC**。

---

## 9. 测试方案

### 9.1 e2e: `tests/e2e/subos_sandbox_test.sh`(E2E-25)

| Scenario | 验证 |
|----------|------|
| S1 | `subos new mybox --sandbox-shell xim:dash` 创建后,subos 物理目录有正确的 etc/{passwd,group,hosts,nsswitch.conf} + bin/dash + root/ + tmp/ |
| S2 | `subos use mybox` 进入后,`whoami` 输出 `root`,`id -u` 输出 `0` |
| S3 | sandbox 内 `cat /etc/os-release` 不存在(空 rootfs);`ls /xlings/bin/xlings` 存在(bind 进来) |
| S4 | sandbox 内 `xlings install xim:bash` 装到 `/bin/bash`,持久 |
| S5 | sandbox 内 `mkdir -p /root/.config/test && echo X > /root/.config/test/foo` 后退出,再进入,文件还在 |
| S6 | sandbox 内 `cd /root && ls` 是空(隔离),宿主 `~/.gitconfig` 不可见 |
| S7 | 在 sandbox 内 `xlings subos use other` 应被拒绝(嵌套检测)|

### 9.2 平台覆盖

- Linux x86_64:全部 7 个 scenario
- macOS / Windows:只测 `subos new --sandbox-shell` 应当报"仅 Linux"错误,不创建任何文件

### 9.3 已有测试不能回归

`subos new <name>` 不带 `--sandbox-shell` → 仍走现有 shell-level 创建路径,**所有现有 subos 测试通过**:
- subos_workspace_c2_schema_test
- subos_install_remove_isolation_test
- nested_xlings_home_test
- subos_payload_refcount_test
- subos_shell_level_test
- 其他

---

## 10. 后续(不在 V1.1 范围)

| 后续工作 | 备注 |
|---------|------|
| `xim:proot` xpkg(走 xim-pkgindex)| 替代当前的 GitLab CDN 自动下载,纳入 xlings 自家供应链 |
| `--bring-in <path>` 选择性 dotfile bind | 让用户能把宿主 `~/.gitconfig`、`~/.ssh` 等 opt-in 进 sandbox |
| `subos config <name> --sandbox-shell <new>` 换 shell | 当前要换只能重建 sandbox |
| bwrap 后端(高性能场景) | 接 `IsolationBackend` 抽象层,在 user-ns 可用的内核上自动选 bwrap |
| OverlayFS(节省多 sandbox 空间)| sandbox 之间共享只读 base layer |
| `xlings subos export <name>` / `import` | sandbox 打包 + 跨机器迁移 |

---

## 11. 决策清单(已定)

1. ✅ **`--sandbox-shell <xpkg>` 单字段触发**,不引入 `--mode` flag
2. ✅ **`/etc/passwd` 等 4 个最小文件由 xlings 在 subos 创建时写**,不依赖具体 shell xpkg
3. ✅ **`$HOME = /root`**,匹配 Linux 惯例
4. ✅ **`proot -0`(伪 root)**,业界惯例 + 工具兼容
5. ✅ **嵌套 sandbox 严格拒绝**
6. ✅ **整个 xlings home bind 到 `/xlings`**,1 条 bind 替代多条 magic path
7. ✅ **proot 自动下载 fallback**(V1.1)→ 后续 `xim:proot` xpkg 接管
8. ✅ **仅 Linux,不静默降级**(macOS/Windows 显式报错)
9. ✅ **不引入 abi 偏好字段**(0.5.0+ 再考虑)
10. ✅ **不引入 `xlings shell` subcommand**(过度设计)
11. ✅ **不引入 `subos config --sandbox-shell` 子命令**(V1.1 不做,需要换 shell 就重建 sandbox)

---

## 12. UX 示例

```bash
# 创建 + 进入
$ xlings subos new mybox --sandbox-shell xim:dash
  ▾ installing xim:dash to mybox/bin/dash (~150 KB)
  ✓ subos created: mybox  (sandbox-shell: /bin/dash)

$ xlings subos use mybox
  ▸ entering subos mybox  (sandbox: kernel-shared, root-faked)

[mybox] # whoami
root
[mybox] # id
uid=0(root) gid=0(root) groups=0(root)
[mybox] # ls /
bin etc proc root sys tmp xlings dev
[mybox] # cat /root/.gitconfig
cat: /root/.gitconfig: No such file or directory   # ← 完整隔离

# 在里面装东西
[mybox] # xlings install gcc node
  ✓ installed gcc@15.1.0
  ✓ installed nodejs@22.0.0
[mybox] # gcc --version
gcc (xim:gcc 15.1.0) 15.1.0
[mybox] # which gcc node
/bin/gcc
/bin/node

# 持久化测试
[mybox] # echo "myproject = 'hello'" > /root/.config/myapp.toml
[mybox] # exit

$ xlings subos use mybox
[mybox] # cat /root/.config/myapp.toml
myproject = 'hello'                           # ← 还在

# 退出 + 删除
[mybox] # exit
$ xlings subos remove mybox
  ✓ removed: mybox
$ ls ~/.gitconfig                              # ← 宿主 dotfile 一尘不染
... (原内容不变)
```

---

## 13. 风险评估

| 风险 | 影响 | 缓解 |
|------|------|------|
| proot.gitlab.io CDN 不可达(防火墙 / GFW)| 用户首次创建 sandbox 失败 | 后续走 `xim:proot` xpkg + xlings 镜像加速;V1.1 已在错误信息里给出 `apt install proot` / `dnf install proot` 兜底 |
| proot 自身 bug(罕见) | sandbox 内某些极端 syscall 报怪错 | proot 5.4 stable,VHSgunzo 静态版近 3.5k stars,常见 workload 已被 termux 等大量用过 |
| 性能 30-50% 慢 | syscall-heavy build 不爽 | 文档明示;后续 V1.2 用 bwrap 后端补位 |
| `/etc/passwd` 模板被工具写花 | sandbox 内程序改 passwd 后 xlings 升级模板会冲突 | passwd 是用户域(他们 sandbox),xlings 不主动覆写;升级时只在文件不存在时写 |
| sandbox 内 xlings install 失败 | 全局 xpkgs 池写入失败 | 错误向用户透明,跟外面 xlings install 一致;原因多半是网络 |
| 嵌套 sandbox 误检测 | 误拒绝合法操作 | 双重检测:env + 当前进程 cgroup/cwd 路径,有冲突优先 env |

---

## 14. 验收标准

- [x] 设计 review 通过 ← 本文档
- [ ] V1.1 实现合入 main
- [ ] e2e 测试 7 scenario 全过(Linux)
- [ ] 现有 subos 测试零回归
- [ ] CI 三平台 pass(Linux 全测,macOS/Windows 验证非 Linux 拒绝行为)
- [ ] 用户实测 `subos new mybox --sandbox-shell xim:dash` + `subos use mybox` + `xlings install gcc` 正常工作

实施工作即将开始,本文档作为实施依据。
