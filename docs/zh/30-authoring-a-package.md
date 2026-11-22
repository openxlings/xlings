# 编写包

[English](../30-authoring-a-package.md) | **简体中文**

一个包就是一个配方：一个 Lua 文件，声明它是什么、在每个平台和架构上 payload 从哪里来（带 sha256），
以及下载之后要做什么。精确规则见 [xpkg 包描述规范](../specs/xpkg-manifest-v1.md)；本章是从零到装上一个包的路径。

## 第一个配方

```lua
package = {
    spec = "1",
    name = "hello",
    description = "Says hello",
    type = "package",
    archs = {"x86_64", "aarch64"},
    licenses = {"MIT"},
    programs = {"hello"},
    xvm_enable = true,

    xpm = {
        linux = {
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {
                url = "https://example.com/hello-1.0.0-linux-${arch}.tar.gz",
                sha256 = { x86_64 = "<sha256>", aarch64 = "<sha256>" },
            },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

function install()
    os.tryrm(pkginfo.install_dir())
    os.mv(pkginfo.install_file():replace(".tar.gz", ""), pkginfo.install_dir())
    return true
end

function config()
    xvm.add("hello", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("hello")
    return true
end
```

## 试一试

```bash
xlings config --add-xpkg ./hello.lua     # into the local overlay index
xlings install hello
hello
xlings config --remove-xpkg hello
```

## 需要知道的

- **每个资源都有 sha256**，文件按架构不同时按架构给出。按架构列出产物而其中没有某台机器的架构时，在那台机器上下载之前就被拒绝。
- **`archs`** 写明这个包在哪些架构上提供。
- **钩子**：`install()` 把文件放进 `pkginfo.install_dir()`；`config()` 注册程序（`xvm.add`）、库和环境；
  `uninstall()` 撤销 `config()`。没有 `install()` 的包得到归档里原样的条目。
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
  if system.admin_step then  -- libxpkg 0.0.63
      system.admin_step({"chown", "root:root", helper}, "its sandbox helper must be root's")
  end
  ```

  索引的静态检查会拒绝未标注就运行 `sudo` 的 hook。
- **`extensions`** 给另一个程序的数据，按那个程序登记的命名空间分开（`extensions = { luban = { tier = 2 } }`）：
  xlings 原样带出、从不解读，改它不需要提升 `revision`。xlings 自己需要理解的字段，应提进规范，而不是放进 `extensions`
  （`docs/specs/xpkg-manifest-v1.md` §3.1）。
- **`host_facts`**：`config()` 依据宿主生成内容时（以宿主当前加载的驱动版本命名的链接），按平台列出它读取的宿主文件
  （`host_facts = { linux = { "/sys/module/nvidia/version" } }`）。xlings 记录 `config()` 运行时这些文件的内容，
  文件变化后重新运行 `config()`：安装时、`xlings use` 时、进入 SubOS 时，以及 `xlings self doctor --fix`。
  按当时的宿主生成（用通配，不用文件名列表），重新运行 `config()` 即是完整的修复
  （`docs/specs/xpkg-manifest-v1.md` §3.2）。

## 发布

发布到官方索引：向 [openxlings/xim-pkgindex](https://github.com/openxlings/xim-pkgindex) 提交 pull request，
它的文档说明了检查项、资源服务器（`xlings-res`）和 CI。发布到自己的索引：[31](31-hosting-an-index.md)。
