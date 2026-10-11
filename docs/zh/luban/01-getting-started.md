# 快速开始

[English](../../luban/01-getting-started.md) | **简体中文**

`luban` 随 xlings 一起安装，版本相同；在宿主上、在每个 Luban 环境里、在每台 Luban 机器上都是同一个程序。

## 常用命令

```bash
luban                         # one screen: where you are, the environments, what next
luban new box                 # an environment (Luban Core by default)
luban new box tiny            # an edition: nano, tiny, core, agent-workspace, or ns:name
luban enter box               # go into it
luban run box -- make -j8     # one command in it
luban ls                      # the environments
luban status box              # one environment; without a name, this host
```

装软件仍然是 xlings：`xlings install <package>`——在环境里就装进那个环境。

```bash
luban run box -- xlings install -y cmake
```

## 在宿主上第一次使用

Ubuntu 23.10 及以后，AppArmor 不允许没有 profile 的程序创建环境所需的沙箱。第一次 `luban new` 会在下载任何东西之前检查宿主，
并询问一次性设置：一个 root 拥有的 bwrap 和一个范围很窄的 AppArmor profile，对这台机器的所有用户生效，只需一次 `sudo`。

```bash
luban setup                   # the same setup, whenever you like
```

agent（`--agent`）从不被提问：命令以退出码 2 结束并给出这项设置。

## 更多

```bash
luban help --all              # every command
luban <command> --help        # one command, and the xlings command it is
```

[Edition](02-editions.md) · [环境](03-environments.md) · [镜像](05-images.md)
