# Authoring a Package

**English** | [简体中文](zh/30-authoring-a-package.md)

A package is a recipe: a Lua file that declares what it is, where its payload
comes from for each platform and architecture (with a sha256), and what to do
after the download. The exact rules are in the
[xpkg manifest specification](specs/xpkg-manifest-v1.md); this chapter is the
path from nothing to an installed package.

## A first recipe

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

## Try it

```bash
xlings config --add-xpkg ./hello.lua     # into the local overlay index
xlings install hello
hello
xlings config --remove-xpkg hello
```

## What to know

- **Every resource has a sha256**, per architecture when the files differ. A
  recipe that lists builds per architecture and has none for a machine is refused
  there before the download.
- **`archs`** names the architectures the package is offered on.
- **Hooks**: `install()` places files into `pkginfo.install_dir()`; `config()`
  registers programs (`xvm.add`), libraries and environment; `uninstall()` undoes
  `config()`. A package with no `install()` gets its archive's entries as they are.
- **Dependencies** are declared per platform (`deps = { runtime = {…}, build = {…} }`).
  A bare name means the package of that name in the same index.
- **`revision`** on a version: raise it when the payload or what `config()` does
  changes for a published version — it is the one signal every installed copy acts on.
- A published version's resource is immutable: a new payload gets a new asset
  name and a raised revision.
- A payload has no hard links (Android and FAT refuse `link(2)`).
- **The runtime is the environment's.** A package that depends on `glibc` (or
  `musl`, or `gcc-runtime`) is bound to its environment's runtime through
  `<home>/abi/<family>`, not to the glibc it was installed with: one payload serves
  every environment, and an environment changes its runtime without rewriting it.
  A dependency on a runtime without a version is met by the runtime the environment
  already has.
- **Never run `sudo` in a hook.** An install nobody watches has no terminal to ask
  on. Declare the step instead, and xlings asks once after the install (or keeps it
  for `xlings self doctor --fix`):

  ```lua
  if system.admin_step then  -- libxpkg 0.0.63
      system.admin_step({"chown", "root:root", helper}, "its sandbox helper must be root's")
  end
  ```

  The index's static check refuses a hook that runs `sudo` unmarked.

## Publishing

To the official index: a pull request to
[openxlings/xim-pkgindex](https://github.com/openxlings/xim-pkgindex), whose
documentation covers its checks, its resource server (`xlings-res`) and its CI.
To an index of your own: [31](31-hosting-an-index.md).
