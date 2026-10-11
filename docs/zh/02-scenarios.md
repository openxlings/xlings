# 场景

[English](../02-scenarios.md) | **简体中文**

xlings 用来做什么，以及每类工作用到哪些功能。

## 多套工具链，互不冲突

```bash
xlings install gcc@16 gcc@11 node@24
xlings use gcc@11        # switch back any time; both stay installed
```

每个版本在 home 的 store 里只存一份；切换改变的是环境看到哪一个。[03](03-packages-and-versions.md)

## 整个团队同一个环境

```json
{ "workspace": { "xmake": "3.0.7", "gcc": { "linux": "16.1.0" }, "llvm": { "macosx": "20.1.7" } } }
```

```bash
cd my-project/ && xlings install     # the project's own SubOS, the declared versions
```

`.xlings.json` 纳入版本控制；克隆项目的每个人得到相同的版本，CI 也用同一个文件。[04](04-projects.md)

## 在沙箱里运行 agent 或不受信任的代码

```bash
xlings subos new agent-ws --sandbox=private      # isolation is declared once, here
xlings subos exec agent-ws -- python run.py      # every entry is that sandbox; its own exit code
xlings subos use agent-ws                        # or a shell inside -- no flag needed
```

Linux 上沙箱隔离文件系统、进程、网络和身份；macOS 与 Windows 上只重定向 home，不是安全边界。
[10](10-subos.md)、[11](11-sandbox-and-policy.md)

## 给 agent 的私有工作区

```bash
luban new agent agent-workspace --proxy socks5h://127.0.0.1:7897
luban run agent -- claude
```

创建完成时就是私有的：网络只有那个代理，有自己的身份，看不到你的 home。
[Luban：agent 工作区](luban/04-agent-workspace.md)

## 一个 Linux 环境、一个镜像、一台机器

```bash
luban new box tiny                  # a small Linux userland of its own
luban export box box.iso            # a live image; or box.img, a drive
luban try box                       # boot it in a local virtual machine
```

[Luban](luban/README.md)

## 自己的包和索引

放在自己仓库里的配方，列进 `.xlings.json` 的 `index_repos` 之后，安装起来和官方包一样。
[30](30-authoring-a-package.md)、[31](31-hosting-an-index.md)

## 升级与修复

```bash
xlings self update
xlings self doctor --fix
```

[06](06-self-management.md)
