# Linux root 用户下的 xlings 可用性调研

> 日期: 2026-06-21
> 类型: 调研 (survey)
> 范围: `xlings self install/uninstall/init`、`xim` 包安装/解包、`xvm` shim、`subos` 沙箱在 Linux **root / sudo** 下的行为
> 方法: 三路并行源码深挖 + 一手核验

## TL;DR

xlings 在 `self` / `xim` / `xvm` / `mirror` 全链路里**没有任何 EUID/root/sudo 感知**。全树唯一的 `::getuid()` 在 `src/core/subos.cppm:1416`,仅用于 sandbox NSS 模板。代码隐含假设「调用者就是文件的拥有者」,这在两类 root 场景下分别失效。

## 两类 root 场景(失效模式不同)

| 场景 | `HOME` | 典型问题 |
|---|---|---|
| **纯 root 登录**(容器 / 裸 root) | `/root` | 装到 `/root/.xlings`,基本可用;但 sandbox passwd 冲突、`sudo` 硬编码在无 sudo 镜像里直接失败 |
| **`sudo xlings ...`**(普通用户提权单次执行) | 多数发行版重置为 `/root`,`-E` 时仍是用户 home | **split-brain**:真实用户拿不到 PATH;`~/.xlings` 留下 root 属主文件,后续非 sudo 调用 `EACCES` |

---

## 问题清单(按影响排序)

### P0-1 — subos 把 `sudo` 硬编码进 mount/umount/chown,纯 root 容器里直接失败
`src/core/subos.cppm:319 / 327 / 337 / 1890` 无条件 `sudo mount` / `sudo chown` / `sudo umount`。
- 已经是 root 时 `sudo` 冗余;
- 最小化 root 容器(image 模式主场景)**常常没装 sudo** → `sudo: command not found`,image 隔离级在 root 容器下「有权限却用不了」。

**优化**: `priv_prefix()` 助手 —— `geteuid()==0 ? "" : "sudo "`,套到所有 mount/umount/chown。

### P0-2 — `sudo` 安装造成属主撕裂,毒化 `~/.xlings`
全链路 `fs::create_directories` / `copy_file` / `write_string_to_file`(`installer.cppm`、`init.cppm`、`config.cppm:499`、`shim.cppm`)**无 chown-back**。
- `sudo -E xlings install`(或导出了 `XLINGS_HOME`)把 `runtimedir`、`xpkgs`、版本 DB、shim、`.xlings.json` 全部以 `root:root` 写进用户 home。
- 下一次**非 sudo** `xlings install` → `EACCES`;且错误常被吞(`installer.cppm:1228-1230` 忽略 `ec`),损坏状态。
- 现成范式: `subos.cppm:324-329`(image mount 后 `chown` 回真实用户)—— self/xim 路径完全没采用。

**优化**: `geteuid()==0 && SUDO_UID` 时 `chown -R $SUDO_UID:$SUDO_GID` 安装产物;或启动时检测 home 属主 ≠ EUID 即明确报错。

### P1-3 — `sudo` 下真实用户拿不到 PATH(rc 文件写错对象)
`install.cppm:249-308` 的 rc 选择来自 `get_home_dir()`(`linux.cppm:163` 直接返回 `$HOME`)。sudo 下写的是 `/root/.bashrc`、`/root/.config/fish/`,**真实用户 shell 永远没有那行 PATH/XLINGS_HOME**。安装「成功」但敲 `xlings` 找不到。无 `SUDO_USER` 检测。
另:fish 分支(`install.cppm:289-303`)只要 `command -v fish` 存在就给 root 建 `~/.config/fish/config.fish`,即便没人用 fish。

**优化**: `SUDO_USER` 存在时经 `getpwnam(SUDO_USER)->pw_dir` 解析真实 home 写 rc 并 chown;退一步至少打印警告。

### P1-4 — sandbox 内 root 的 passwd 自相矛盾(纯 root-only 缺陷)
`subos.cppm:1416` 用 `::getuid()`(root=0),喂给 `make_etc_passwd_`(`:177-182`),`user=="root"` 时生成**两条 uid-0 记录**:
```
root:x:0:0:root:/root:/bin/sh        ← 硬编码,getpwuid(0) 命中这条
root:x:0:0:root:/home/root:/bin/sh   ← 实际 bind 的 home 在这
```
沙箱里 `HOME=/home/root`(`:1554`)、`--chdir /home/root`、profile bind 在 `/home/root/.xlings`。任何用 `getpwuid(0)` 解析 root home 的工具(bash `~`、ssh、脚本)拿到 `/root`(空目录),与真实 `/home/root` 不一致 → `cd ~`、`~/.config`、profile 失效。

**优化**: `user=="root"` 时不追加第二条;或把 root 合成 home 统一成 `/root` 并调整 bind/chdir。

### P2-5 — `home_knows_program` 把 `EACCES` 误报成「未安装」
`shim.cppm:113-149`:读 `/root/.xlings/.xlings.json` 遇权限错误被 catch 当「未注册」(`:145-147`)。sudo 装在 `/root/.xlings` 的程序,普通用户调用时 shim 跨读 root json `EACCES` → 报「not installed」,误导。

**优化**: 区分 `EACCES` 与真正「未注册」,提示「该 home 属于其它用户 / 勿用 sudo」。

### P2-6 — 解包信任 tarball 的 mode 位(setuid/世界可写直通)
`extract.cppm:49-53` 用 `ARCHIVE_EXTRACT_PERM` 但**没设 `ARCHIVE_EXTRACT_OWNER`** → 不发生 chown;`:225-228` 的「强制 uid 0」回调实际是**死代码**(无 OWNER 时不被调用)。真正隐患是 mode 位原样落盘:条目若打 `4777`,在 root/sudo 下落成 **setuid-root**。路径穿越有 `SECURE_NODOTDOT/SYMLINKS` 防护,mode 无掩码。

**优化**: 解包掩掉危险位 `& ~(S_ISUID|S_ISGID|S_IWOTH)`。

### P3-7 — keeper/nsenter 隐含需要 root 但无守卫(目前未接线)
`keeper.cppm:126-156` 的 `nsenter --mount=...` 需 `CAP_SYS_ADMIN`,无 `sudo` 前缀也无 uid 检查;非特权下静默失败。`register_pid`/`nsenter_and_exec` 当前**无调用点**(仅 `should_auto_keeper`/`stop_keeper` 被引用),属潜在问题,修复 keeper 接线时一并处理。

---

## 澄清:bwrap 探测在 root 下是非问题
`probe_bwrap_`(`:1102`)+ `classify_bwrap_probe_error_`(`:1114`)的三类错误全是**非特权 userns** 失败签名。root 下一般不发生,探测通常直接 pass,功能正常。小瑕疵:root 下若出现别的失败会落 raw-output 分支,且给出对 root 无意义的「关 apparmor userns 限制」提示。另 root 下 `locate_bwrap_`(`:1069`)坚持只用 setuid 的 xim:bwrap、跳过 `/usr/bin/bwrap` 的理由对 root 已不成立,可放宽。

> 修正记录:初版三方调研中曾判定 `extract.cppm:225-228` 在 sudo 下「强制 chown 到 root」。一手核验 `kWriteFlags`(`:49-53`)未含 `ARCHIVE_EXTRACT_OWNER`,libarchive 不执行 chown,该回调为死代码;sudo 下文件 root 属主仅因**进程本身是 root**,与该回调无关。已在 P2-6 修正。

---

## 建议的最小落地方案

新增 `platform::priv` 模块集中三件事,在 install 入口与 subos 各处接入:

```cpp
bool is_root();                       // geteuid()==0
std::string sudo_prefix();            // is_root() ? "" : "sudo "          // 解 P0-1
std::optional<RealUser> sudo_user();  // SUDO_UID/SUDO_GID + getpwnam(SUDO_USER)->pw_dir
void chown_back(path, RealUser);      // EUID==0 && SUDO_USER 时 chown -R  // 解 P0-2/P1-3
```

落地优先级:
1. **P0-1**(去 sudo 硬编码)—— 改动最小、解锁 image 模式在 root 容器,最高性价比;
2. **P0-2 + P1-3**(`SUDO_USER`-aware home + chown-back)—— 一处修复同时关掉属主撕裂、PATH 丢失、shim 误报;
3. **P1-4**(root passwd 去重)、**P2-6**(mode 掩码)—— 独立小补丁;
4. 文档: 在 `docs/` 说明「root/sudo 安装的预期落点与限制」(当前 docs 仅在 image 模式提「需要 root」,未讲 sudo 混用风险)。

---

## 关键代码位置索引

- `src/platform/linux.cppm:163-178` — `get_home_dir`(只读 `$HOME`)、`make_files_executable`(chmod 0755)
- `src/core/config.cppm:414-456, 499` — home 解析优先级、`dataDir`
- `src/core/xself/install.cppm:225-227, 249-308` — `default_home`、rc 写入
- `src/core/xself/uninstall.cppm:65-98` — 路径守卫(阻 `/root`/`$HOME`,放行 `/root/.xlings`)
- `src/core/xim/extract.cppm:49-53, 215-228` — 写盘 flags、死代码 uid-0 回调
- `src/core/xim/installer.cppm:1228-1230` — 吞 `ec`
- `src/core/xvm/shim.cppm:113-149, 155-227` — 程序解析、home 借用、EACCES 吞错
- `src/core/subos.cppm:177-182, 319-329, 337, 1416-1418, 1554, 1890` — 合成 passwd、sudo 硬编码、NSS uid
- `src/core/subos/keeper.cppm:126-156` — nsenter(需 root,未接线)
