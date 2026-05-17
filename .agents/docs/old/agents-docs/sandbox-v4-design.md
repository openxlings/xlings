# Sandbox V4 — `--sandbox` 是 use 的修饰符

**Status**: Active design (target 0.4.23)
**Replaces**: `--sandbox-shell <xpkg>` 那一套(0.4.21 / 0.4.22 V1 ~ V3)

## 一句话

`xlings subos use <name> --sandbox` 进入 subos 的同时罩一层 fs 隔离。`--sandbox` 是 **use 时的模式选择**,不是 subos 创建时的属性。subos 本身没变化。

```
xlings subos new mybox                # 普通 subos,跟以前完全一样
xlings subos use mybox                # 普通进入(env-spawn shell),提示符 [xsubos:mybox]
xlings subos use mybox --sandbox      # NEW:proot fs 隔离进入,提示符 <xsubos:mybox>
```

## 设计前提与排除项

### 前提

1. **subos 概念不变**:仍然是 workspace 隔离单元。`xlings install`/`use`/`remove` 在 subos 内的语义跟以前一样(payload 进 host 池,workspace 进 `<subos>/.xlings.json`,shim 进 `<subos>/bin/`)。
2. **xlings home 共享**:sandbox 内 `~/.xlings` 通过 RW bind 指向 host 的 `~/.xlings`,所以 xlings 命令在 sandbox 里跟 host 行为完全一致 ── 装包、查询、xvm 状态都共享。
3. **真实 user**:proot 不带 `-0`。sandbox 内 `whoami` 返回真实 user(如 speak),`$USER`、`$HOME` 跟 host 同名。
4. **shell 跟 host 一致**:进 sandbox 用 `$SHELL`(用户当前 shell)。不再有"sandbox 必须先选 shell"概念。

### 排除项

- ❌ 不再支持 `--sandbox-shell <xpkg>` (V1.1 - V1.3 的设计)
- ❌ `.xlings.json` 里 `sandbox-shell` / `sandbox-shell-xpkg` 字段不再读写,创建时不写,读取时静默忽略
- ❌ 不再有 `use_sandbox_` 单独函数;sandbox 是 `use_spawn_shell` 的一个 mode 分支
- ❌ 不再有 sandbox 创建期 eager install / 自愈逻辑
- ❌ 不需要 fuse-overlayfs / hardlink-pool 等 payload 复用层 ── xlings home 直接 RW 共享
- ❌ 不主动迁移老 sandbox(0.4.21 - 0.4.22 创建的) ── 老字段直接被忽略,使用上自动走新逻辑

## FS 视图

### sandbox 内 mount table(逻辑)

```
sandbox view ← source                                    | role
══════════════════════════════════════════════════════════════════════
/                    ← <subos>/                          | proot rootfs
/proc /sys /dev      ← host                               | kernel pseudo-fs(必需)

/usr                 ← /usr                  (host RO)    | POSIX userland
/lib                 ← /usr/lib              (host RO)    | usrmerge alias
/lib64               ← /usr/lib64            (host RO)    | loader / libs
/etc/resolv.conf     ← /etc/resolv.conf      (host RO)    | DNS
/etc/ld.so.cache     ← /etc/ld.so.cache      (host RO)    | loader cache

/etc/passwd          ← <subos>/etc/passwd                 | sandbox 模板,真实 user 行 + root 行
/etc/group           ← <subos>/etc/group                  | sandbox 模板
/etc/hosts           ← <subos>/etc/hosts                  | sandbox 模板
/etc/nsswitch.conf   ← <subos>/etc/nsswitch.conf          | sandbox 模板(files-only)

/home                ← <subos>/home/                      | dotfile 隔离根
/home/<user>         ← <subos>/home/<user>/               |   sandbox 私有 user home
/home/<user>/.xlings ← host ~/.xlings        (RW)         |   xlings home host bind 覆盖
/tmp                 ← <subos>/tmp/                       | sandbox 私有 temp
```

### Bind 解析(关键 nesting)

proot 处理 bind 时,**最 specific 的路径胜出**:

```
--bind=<subos>/home:/home                       # 父 bind:/home 整体指向 sandbox 私有
--bind=<host>/.xlings:/home/<user>/.xlings      # 子 bind:.xlings 子路径 override 回 host
```

效果:
- `/home/<user>/.config/fish/x.txt` → `<subos>/home/<user>/.config/fish/x.txt` (sandbox 私有)
- `/home/<user>/.xlings/data/xpkgs/foo` → `<host>/.xlings/data/xpkgs/foo` (host 共享)

### user 行为映射

| 进 sandbox 后做的事 | 实际写到哪里 |
|---|---|
| `vim ~/.bashrc` | `<subos>/home/<user>/.bashrc` (sandbox 私有) |
| `mkdir ~/.config/myapp` | `<subos>/home/<user>/.config/myapp/` (sandbox 私有) |
| `xlings install fish` | xpkgs payload → host `~/.xlings/data/xpkgs/`(共享);workspace 改 `<subos>/.xlings.json`;shim → `<subos>/bin/fish` |
| `xlings list` | 看 `<subos>/.xlings.json` workspace + host xvm DB |
| `cat /etc/passwd` | sandbox 模板(只看到自己的 user 行 + root 行,看不到其他 host user) |
| `mkdir /tmp/work` | `<subos>/tmp/work/` (sandbox 私有) |
| `mkdir /usr/local/foo` | EROFS (host RO) |
| `whoami` | 真实 user(如 `speak`) |
| `id -u` | 真实 uid(如 `1000`) |

## 提示符规范

`src/core/xself/profile_resources.cppm` 已有的 `[xsubos:<name>]` 提示符增强:

| mode | 提示符 | 由 env 决定 |
|---|---|---|
| 普通 subos use | `[xsubos:<name>]` (现有) | `XLINGS_ACTIVE_SUBOS=<name>` |
| sandbox subos use | `<xsubos:<name>>` (新) | `XLINGS_ACTIVE_SUBOS=<name>` + `XLINGS_SUBOS_MODE=sandbox` |

profile 改动(bash / zsh / fish / pwsh 4 种各加一个 `XLINGS_SUBOS_MODE` 检查分支):

```sh
# bash 例
if [ "${XLINGS_SUBOS_MODE-}" = "sandbox" ]; then
    PS1="...<xsubos:${XLINGS_ACTIVE_SUBOS}>... ${PS1}"
else
    PS1="...[xsubos:${XLINGS_ACTIVE_SUBOS}]... ${PS1}"
fi
```

profile 版本号 bump(`xlings-profile-version: 9 → 10`)以触发自动重写。

## 实施细节

### subos.cppm 改动

#### 删除

- `export create_sandbox()` 整个函数
- `create_impl_()` 里关于 `sandboxShellXpkg` 的所有分支
- `use_sandbox_()` 整个函数
- `ensure_shell_installed_and_linked_()` 整个函数(及其前向声明)
- `sandbox_detail_::` 内不再使用的 helper(只保留 `init_sandbox_dirs_` 和 `build_proot_argv_`)
- `use_spawn_shell()` 里检查 `sandbox-shell` 字段并跳到 `use_sandbox_` 的分支

#### 修改

`init_sandbox_layout_(dir)` → `init_sandbox_dirs_(dir, user)`

```cpp
// 新签名:接 user 名,写真实 user 行的 /etc/passwd
void init_sandbox_dirs_(const fs::path& subos_dir, const std::string& user, uid_t uid, gid_t gid);
```

逻辑(idempotent,可以多次调用):
1. `mkdir -p <subos>/home/<user>` (即 sandbox 内的 `$HOME`)
2. `mkdir -p <subos>/tmp` (mode 1777 sticky bit, 即使外面没 root)
3. `mkdir -p <subos>/etc`
4. 如果 `<subos>/etc/passwd` 不存在,写:
   ```
   root:x:0:0:root:/root:/bin/sh
   <user>:x:<uid>:<gid>:<user>:/home/<user>:/bin/sh
   ```
5. 类似写 group, hosts, nsswitch.conf

`build_proot_argv_` 重写为最简版:

```cpp
std::vector<std::string> build_proot_argv_(
    const fs::path& proot_bin,
    const fs::path& subos_dir,
    const fs::path& host_xlings_home,
    const std::string& user,
    const std::string& shell)  // $SHELL,如 /usr/bin/fish
{
    auto etc = subos_dir / "etc";
    return {
        proot_bin.string(),
        // NO -0 ── 用真实 user identity
        "-r", subos_dir.string(),
        "--bind=/proc:/proc",
        "--bind=/sys:/sys",
        "--bind=/dev:/dev",
        "--bind=/etc/resolv.conf:/etc/resolv.conf",
        "--bind=/etc/ld.so.cache:/etc/ld.so.cache",
        std::format("--bind={}:/etc/passwd",       (etc / "passwd").string()),
        std::format("--bind={}:/etc/group",        (etc / "group").string()),
        std::format("--bind={}:/etc/hosts",        (etc / "hosts").string()),
        std::format("--bind={}:/etc/nsswitch.conf",(etc / "nsswitch.conf").string()),
        "--bind=/usr:/usr",
        "--bind=/usr/lib:/lib",
        "--bind=/usr/lib64:/lib64",
        // sandbox 私有 /home(覆盖 chroot 默认的空 /home)
        std::format("--bind={}:/home", (subos_dir / "home").string()),
        // 在 /home 之上叠加 .xlings 的 host bind ── 子路径覆盖父路径
        std::format("--bind={}:/home/{}/.xlings", host_xlings_home.string(), user),
        std::format("--cwd=/home/{}", user),
        shell,
    };
}
```

`use_spawn_shell()` 加 sandbox 模式:

```cpp
int use_spawn_shell(const std::string& name, EventStream& stream, bool sandbox = false) {
    // ... validate, nesting check 不变 ...

    if (sandbox) {
#if !defined(__linux__)
        stream.emit(ErrorEvent{
            .message = "sandbox mode is only supported on Linux",
            // ... 跟以前一样 ...
        });
        return 1;
#else
        return use_sandbox_mode_(name, stream);
#endif
    }

    // ... env-spawn shell 的现有逻辑(完全不变)...
}

int use_sandbox_mode_(const std::string& name, EventStream& stream) {
    auto& p = Config::paths();
    auto subos_dir = p.homeDir / "subos" / name;

    // proot 探测(沿用现有 locate_proot_)
    auto proot = sandbox_detail_::locate_proot_(p.homeDir);
    if (!proot) { stream.emit(ErrorEvent{...}); return 1; }

    // lazy init sandbox 私有 dirs
    auto user = utils::get_env_or_default("USER", "user");
    auto uid = ::getuid();
    auto gid = ::getgid();
    sandbox_detail_::init_sandbox_dirs_(subos_dir, user, uid, gid);

    // env
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_SUBOS_MODE", "sandbox");
    platform::set_env_variable("HOME", "/home/" + user);
    // 不设 USER ── 用 host 的(已经是真实 user)
    // PATH 由 profile + xlings shim 解决,不在这里 hardcode

    // shell:用 $SHELL,fallback /bin/sh
    auto shell = utils::get_env_or_default("SHELL", "/bin/sh");

    // 组装 proot argv 并 exec
    auto argv = sandbox_detail_::build_proot_argv_(
        *proot, subos_dir, p.homeDir, user, shell);
    // ... execvp 不变 ...
}
```

### profile_resources.cppm 改动

每个 shell profile 加 `XLINGS_SUBOS_MODE=sandbox` 分支选 `<>` 括号。bump 版本号到 10。

### CLI 解析

`src/core/cmdprocessor.cppm` 或 `src/cli.cppm` 的 `subos use` 子命令解析支持 `--sandbox` flag:

```cpp
bool sandbox_flag = false;
for (...) {
    if (arg == "--sandbox") { sandbox_flag = true; continue; }
    // ...
}
return subos::use_spawn_shell(name, stream, sandbox_flag);
```

## E2E 测试

`tests/e2e/subos_sandbox_test.sh` 重写为 V4 场景。需要 Linux + proot(可装 xim:proot)。

```
S1: subos new mybox(纯创建,无 sandbox 字段)
    断言 .xlings.json 不含 sandbox-shell / sandbox-shell-xpkg

S2: subos use mybox(普通)
    断言 PS1 出现 [xsubos:mybox]
    断言 $HOME = host $HOME(没改)

S3: subos use mybox --sandbox
    断言 PS1 出现 <xsubos:mybox>
    断言 $HOME = /home/<user>
    断言 whoami = real user
    断言 id -u = real uid

S4: dotfile 隔离
    sandbox 内 mkdir ~/.config/sandboxtest/
    退出后 host ~/.config/sandboxtest 不存在
    再次 use --sandbox,~/.config/sandboxtest 还在(persistent)

S5: ~/.xlings 共享
    sandbox 内 xlings install <fixture>
    退出后 host ls ~/.xlings/data/xpkgs/<fixture>/<ver>/ 存在
    sandbox 内 xlings list 显示 <fixture>
    退出后 host xlings list 也显示 <fixture>(因 workspace 在 mybox subos)

S6: /etc/passwd 隔离
    sandbox 内 cat /etc/passwd | wc -l → 应 ≤ 2 (root + real user)
    sandbox 内 grep ^<user> /etc/passwd → 真实 user 行

S7: /tmp 隔离
    sandbox 内 touch /tmp/sandboxtest
    退出后 host /tmp/sandboxtest 不存在
    <subos>/tmp/sandboxtest 存在

S8: subos remove 清干净(包括 home dir、tmp、etc)

S9 (non-Linux): subos use --sandbox 在 macOS / Windows 拒绝
```

## 老 sandbox 兼容(零代价)

老 0.4.21 - 0.4.22 创建的 sandbox 在 `<subos>/.xlings.json` 里有 `sandbox-shell` / `sandbox-shell-xpkg` 字段,和 `<subos>/root` `<subos>/bin/<shell>` symlink 等遗留。

V4 处理:
- `subos use mybox`(无 --sandbox) → 走普通路径,完全不读那两个字段,跟现在一致
- `subos use mybox --sandbox` → 走新 V4 sandbox 路径,init_sandbox_dirs_ 是 idempotent 的(只 mkdir 不存在的目录),老遗留的 `<subos>/root`、shell symlink 静静躺着不影响
- 用户想清掉老遗留:`subos remove mybox && subos new mybox`

**不主动迁移,不主动删除,不报错提示** ── 沉默兼容。

## 关键不变性 / 失败模式

| 不变性 | 测试位置 | 失败现象 |
|---|---|---|
| /home/<user> bind 在前,.xlings host bind 在后 | S5 | sandbox 看不到 host ~/.xlings |
| 不带 -0,真实 user | S3 | sandbox 内 whoami = root(错) |
| /etc/passwd 模板含真实 user 行 | S6 | shell 启动时 `getpwuid(uid)` 失败,环境变量异常 |
| profile 版本 bump → 触发重写 | 手动验证 | 老 profile 不读 XLINGS_SUBOS_MODE,提示符无 `<>` |
| init_sandbox_dirs_ idempotent | S2/S3 多次 use | 第二次 use --sandbox 报"目录已存在" |

## 不在本设计内(后续)

- payload 复用机制(hardlink / overlay / fuse-overlayfs):V4 不做,xlings home 直接 RW 共享
- sandbox 内 `xlings self update`:不限制(可能修改 host xlings ── 跟普通 subos 行为一致,本来就该如此)
- `--sandbox` 跟 `--exec <cmd>` 组合:留作后续 enhancement
- macOS / Windows sandbox:Linux-only,本设计不变

## 改动量估算

| 文件 | 删除 | 新增 | 修改 |
|---|---|---|---|
| `src/core/subos.cppm` | ~300 LOC | ~120 LOC | ~80 LOC |
| `src/core/xself/profile_resources.cppm` | 0 | ~40 LOC × 4 shells | bump version |
| `src/core/cmdprocessor.cppm` 或 cli | 0 | ~10 LOC | 1 行加 flag |
| `tests/e2e/subos_sandbox_test.sh` | 全删 | 全重写 ~200 LOC | — |
| `.agents/docs/changelog.md` | 0 | ~30 LOC | — |
| `src/core/config.cppm` | 0 | 0 | VERSION 0.4.22 → 0.4.23 |

**净 ~ -200 LOC**(删的多,设计干净了)。

## 版本号 / Release

包含在 0.4.23 中。0.4.23 内容:
1. (来自 PR #279)proot `-R` → `-r` 修复 + `/usr` RO bind + `/etc/ld.so.cache` bind ── **被 V4 设计直接吸收**
2. (新)V4 sandbox 重设计:`--sandbox-shell` 删除,`subos use --sandbox` 上线,提示符 `<>` 标识

changelog 标 BREAKING:
- `xlings subos new --sandbox-shell <xpkg>` 已删除
- 老 sandbox(0.4.21 - 0.4.22 创建的)的 `sandbox-shell` 字段被忽略;直接 `subos use <name> --sandbox` 进入新模式
- 用户原本预期 sandbox 自带 fish/bash 的,需要进入后 `xlings install fish` 自己装
