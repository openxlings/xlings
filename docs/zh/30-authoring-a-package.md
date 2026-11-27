# 编写包

[English](../30-authoring-a-package.md) | **简体中文**

一个包就是一个配方：一个 Lua 文件，声明它是什么、在每个平台和架构上 payload 从哪里来（带 sha256），
以及下载之后要做什么。精确规则见 [xpkg 包描述规范](../specs/xpkg.md)；本章是从零到装上一个包的路径。

## 第一个配方

```lua
package = {
    spec = "3",
    name = "hello",
    description = "Says hello",
    type = "package",
    archs = {"x86_64", "aarch64"},
    licenses = {"MIT"},
    programs = {"hello"},

    xpm = {
        linux = {
            strip = 1,   -- the archive holds hello-1.0.0/: drop that level
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                url = "https://example.com/hello-1.0.0-linux-${arch}.tar.gz",
                sha256 = { x86_64 = "<sha256>", aarch64 = "<sha256>" },
            },
        },
    },
}

import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function config()
    xvm.add("hello", { bindir = path.join(pkginfo.install_dir(), "bin") })
end

function uninstall()
    xvm.remove("hello")
end
```

这里没有 `install()`：任何 hook 运行之前，xlings 已经下载归档、校验 sha256，并按 `strip` 解压进
`pkginfo.install_dir()`。`install()` 只用来做剩下的调整——改权限、生成包装脚本、打补丁。

## 试一试

```bash
xlings index check ./hello.lua           # the format's rules, before anything else
xlings config --add-xpkg ./hello.lua     # into the local overlay index
xlings install hello
hello
xlings config --remove-xpkg hello
```

## 需要知道的

- **`spec = "3"`** 是现行的格式。它的 hook 看到的是标准 Lua 里做计算的部分（`string`、`table`、`math`）
  和它导入的模块——[hook 接口](../generated/xpkg-hook-api.md)：`fs`、`system`、`pkginfo`、`path`、`xvm`、`log` 等。
  一次调用是**动作**（`fs.move`、`fs.write`、`system.exec`：做不到它说的那件事时 hook 失败，并说出动作、路径和原因）、
  **询问**（`fs.is_file`、`system.run`），或交给 xlings 去做的**声明**（`xvm.add`）。recipe 要容忍的失败用 `pcall` 写出来。
  `spec` 为 1、2 的 recipe 照旧工作；要迁移一个，libxpkg `tools/` 里的 `xpkg-migrate` 改写它能改的，并列出其余的。
- **`install()` 之前下载已经放好。** 归档按条目的 `strip` 解压进 `pkginfo.install_dir()`（`strip = 1` 去掉顶层目录；
  有成员与声明不符时安装失败，并指出那个成员）；不是归档的下载按原名放进去。归档里套着的归档用 `fs.extract`。
- **顶层只做计算**：只是读 recipe（搜索、构建索引）时顶层也会运行，所以那里不调用 `fs`、`system`、`pkginfo`；
  随平台或架构不同的东西写进条目，或者写在 hook 里（`pkginfo.platform()`、`pkginfo.arch()`）。
- **每个资源都有 sha256，条目说明它提供哪些架构。** 文件按架构不同时按架构给出；只有一种架构的产物写固定 URL 加
  那一个键（`sha256 = { x86_64 = "<h>" }`）；各架构字节相同的（脚本、jar）每个键写同一个哈希。条目没列出的架构，
  在那台机器上下载之前就被拒绝；宿主要从载荷里运行的文件必须是宿主的机器
  （[载荷规则](../specs/payload-rules.md)，规则 M）。
- **`archs`** 是各条目所列架构的并集，用于搜索；索引会检查它。
- **钩子**：`config()` 注册程序（`xvm.add`）、库和环境——声明了 `programs` 的包要注册它们；
  `uninstall()` 撤销 `config()` 做过而 xlings 不会自己撤销的事。hook 只要不返回 `false`、不报错就是成功。
  载荷保存安装时的 recipe 以及它导入的索引 lib，卸载运行的就是这一份：`uninstall()` 撤销的是它自己那次安装
  做过的事，不管索引那时是什么样。
- **shell 补全用声明**，不写进 profile：
  `xvm.add("tool", { bindir = ..., completion = { bash = "share/completion/tool.bash", zsh = "share/completion/_tool", fish = "share/completion/tool.fish", pwsh = "share/completion/tool.ps1" } })`。
  载荷里的每个文件被链接到对应 shell 查找的位置，随 `xlings use` 切换、随版本卸载；xlings 的 profile 加载它们。
  程序能自己打印补全的，在 `install()` 里生成这些文件。hook 从不修改 shell 的 rc 文件或 xlings 的 profile。
- **`log.warn` 是给用户看的**：显示在命令的总结之后。进度用 `log.info`（`-v` 时显示）。
- **依赖**按平台声明（`deps = { runtime = {…}, build = {…} }`）。不带命名空间的名字指同一索引里的同名包。
- 版本上的 **`revision`**：已发布版本的 payload 或 `config()` 的效果变了就提升它——这是安装该版本时（在任何一个 scope）唯一会响应的信号。
- 已发布版本的资源不可变：新的 payload 用新的资产名，并提升 revision。
- 版本上的 **`floor`**：新 revision 修复的是每个已装副本都有的缺陷时，把 `floor` 提到这个 revision，使用它的程序才会替换旧副本；否则不写，旧副本继续可用，`xlings install` 时才换成新打包。
- payload 里没有硬链接（Android 和 FAT 拒绝 `link(2)`）。
- **运行时属于环境。** 依赖 `glibc`（或 `musl`、`gcc-runtime`）的包，经 `<home>/abi/<家族>` 绑定到它所在环境的运行时，
  而不是安装时那一份 glibc：一个 payload 服务所有环境，环境换运行时也不改写它。不带版本的运行时依赖，由环境已有的运行时满足。
- **hook 在什么里运行。** hook 看到的是包所安装进的那个系统：xlings 自用时是这台机器，Windows、macOS 上是
  Luban host machine（静态的 xlings、busybox 与环境的运行时，没有这台电脑的任何工具），系统里是那个系统。
  这个根在标准 loader 路径（`/lib64/ld-linux-x86-64.so.2`、`/lib/ld-linux-aarch64.so.1`）上有 glibc 时，
  hook 启动的程序用它；没有时（host machine、Alpine），xlings 在那里给 hook 补上环境的运行时
  （`<home>/abi/<家族>`），所以 hook 下载后、xlings 改写之前就启动的程序也能运行——前提是包声明了运行时依赖
  （`deps = { runtime = { "xim:glibc" } }`）。由此有两点：
  - 机器的 glibc 比这个程序要求的旧时，它会失败，错误里说明 hook 看到的是哪个 glibc。要在 hook 里不依赖根的运行时
    就运行自己的 payload，先改写它（`elfpatch.set_interpreter(file, loader)`），或经环境的 loader 启动
    （`<home>/abi/<家族>/ld-linux-x86-64.so.2 <程序>`）。
  - 预编译的程序优先直接取它的平台归档（esbuild、codex、bun），不要依赖包管理器在运行时挑选平台的安装脚本。
- **hook 里不要运行 `sudo`。** 无人看守的安装没有终端可问。声明这一步，xlings 在安装结束后统一询问一次
  （没人可问时留给 `xlings self doctor --fix`）：

  ```lua
  system.admin_step({"chown", "root:root", helper}, "its sandbox helper must be root's")
  ```

  （面向 libxpkg 0.0.63 之前客户端的 spec 1、2 recipe 先探测：`if system.admin_step then ... end`。）
  索引的静态检查会拒绝未标注就运行 `sudo` 的 hook。
- **`extensions`** 给另一个程序的数据，按那个程序登记的命名空间分开（`extensions = { luban = { tier = 2 } }`）：
  xlings 原样带出、从不解读，改它不需要提升 `revision`。xlings 自己需要理解的字段，应提进规范，而不是放进 `extensions`
  （`docs/specs/xpkg.md` §3.4）。
- **`host_facts`**：`config()` 依据宿主生成内容时（以宿主当前加载的驱动版本命名的链接），按平台列出它读取的宿主文件
  （`host_facts = { linux = { "/sys/module/nvidia/version" } }`）。xlings 记录 `config()` 运行时这些文件的内容，
  文件变化后重新运行 `config()`：安装时、`xlings use` 时、进入 SubOS 时，以及 `xlings self doctor --fix`。
  按当时的宿主生成（用通配，不用文件名列表），重新运行 `config()` 即是完整的修复
  （`docs/specs/xpkg.md` §3.5）。

## 发布

先检查：`xlings index check <recipe 或索引>` 指出格式不接受的地方和所在行。比 recipe 所用格式旧的客户端只拒绝那一个包，
并说明哪个版本能读它。

发布到官方索引：向 [openxlings/xim-pkgindex](https://github.com/openxlings/xim-pkgindex) 提交 pull request，
它的文档说明了检查项、资源服务器（`xlings-res`）和 CI。发布到自己的索引：[31](31-hosting-an-index.md)。
