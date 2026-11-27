# Authoring a Package

**English** | [简体中文](zh/30-authoring-a-package.md)

A package is a recipe: a Lua file that declares what it is, where its payload
comes from for each platform and architecture (with a sha256), and what to do
after the download. The exact rules are in the
[xpkg format specification](specs/xpkg.md); this chapter is the
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
  changes for a published version — it is the one signal an install of that version
  acts on, here and in every other scope.
- A published version's resource is immutable: a new payload gets a new asset
  name and a raised revision.
- **`floor`** on a version: when the new revision fixes something every installed
  copy has, raise `floor` to it, and programs that use the package replace older
  copies; otherwise leave it out, older copies stay usable, and `xlings install`
  brings the new packaging.
- A payload has no hard links (Android and FAT refuse `link(2)`).
- **The runtime is the environment's.** A package that depends on `glibc` (or
  `musl`, or `gcc-runtime`) is bound to its environment's runtime through
  `<home>/abi/<family>`, not to the glibc it was installed with: one payload serves
  every environment, and an environment changes its runtime without rewriting it.
  A dependency on a runtime without a version is met by the runtime the environment
  already has.
- **What a hook runs in.** A hook sees the system its package is installed into:
  the machine in xlings' own use, the Luban host machine on Windows and macOS (a
  static xlings, busybox and the environment's runtime -- none of the machine's
  tools), a system inside it. Where that root has a glibc loader at its standard
  path (`/lib64/ld-linux-x86-64.so.2`, `/lib/ld-linux-aarch64.so.1`), a program
  the hook starts uses it; where it has none (the host machine, Alpine), xlings
  gives the hook the environment's runtime there (`<home>/abi/<family>`), so a
  program a hook downloads and starts before xlings rewrites it runs -- with
  your package's runtime dependency declared (`deps = { runtime = { "xim:glibc" } }`).
  Two things follow:
  - On a machine whose glibc is older than such a program needs, it fails, and
    the error says which glibc the hook saw. To run your own payload in a hook
    regardless, rewrite it first (`elfpatch.set_interpreter(file, loader)`) or
    start it through the environment's loader
    (`<home>/abi/<family>/ld-linux-x86-64.so.2 <program>`).
  - Prefer taking a prebuilt program's platform archive directly (esbuild, codex,
    bun) over a package manager's install script that picks one at run time.
- **Never run `sudo` in a hook.** An install nobody watches has no terminal to ask
  on. Declare the step instead, and xlings asks once after the install (or keeps it
  for `xlings self doctor --fix`):

  ```lua
  if system.admin_step then  -- libxpkg 0.0.63
      system.admin_step({"chown", "root:root", helper}, "its sandbox helper must be root's")
  end
  ```

  The index's static check refuses a hook that runs `sudo` unmarked.
- **`extensions`** carries data for another program, keyed by that program's
  registered namespace (`extensions = { luban = { tier = 2 } }`): xlings hands it
  on as written and never reads it, and changing it raises no `revision`. When
  xlings itself must understand a field, propose it for the specification
  instead (`docs/specs/xpkg.md` §3.4).
- **`host_facts`**: when `config()` generates something from the host -- links
  named for the driver version the host has loaded -- list the host files it
  reads, per platform (`host_facts = { linux = { "/sys/module/nvidia/version" } }`).
  xlings records what they held when `config()` ran and runs `config()` again when
  they change: at an install, at `xlings use`, on entering a SubOS, and in
  `xlings self doctor --fix`. Generate from the host as it is then (a glob, not a
  list of file names), so that running `config()` again is the whole repair
  (`docs/specs/xpkg.md` §3.5).

## Publishing

To the official index: a pull request to
[openxlings/xim-pkgindex](https://github.com/openxlings/xim-pkgindex), whose
documentation covers its checks, its resource server (`xlings-res`) and its CI.
To an index of your own: [31](31-hosting-an-index.md).
