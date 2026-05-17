## 快速使用

> 使用xlings快速创建一个项目(以clings为例)

### 项目配置

> 在一个空目录中新建一个config.xlings文件, 填写描述项目的基础信息

```lua
xlings_name = "clings" -- 项目名
xlings_lang = "c" -- 项目中使用的编程语言
```

### 生成项目结构

> 在config.xlings同目录执行项目初始化

```bash
xlings init
```

### 运行自动练习检测

> 使用`xlings [项目名]`开启自动练习检测

```bash
xlings clings # clings 是配置文件中配置的项目名
```

或

```bash
xlings checker
```

### 更多xlings命令

```bash
xlings version: pre-v0.0.4

Usage: $ xlings [command] [target]

Commands:
         checker,        start auto-exercises from target
         update,         update xlings to the latest version
         uninstall,      uninstall xlings
         help,           help info

repo: https://github.com/d2learn/xlings
```