# The xpkg hook interface

<!-- Generated from `xlings --xpkg-api-json` (libxpkg's interface manifest);
     do not edit by hand. Regenerate with:
       python3 tests/scripts/test_generated_xpkg_hook_api.py \
         --xlings <path-to-xlings> --write
-->

What a hook can call, as the libxpkg this xlings is built with states it. The rules are [the xpkg format](../specs/xpkg.md) §6: an **action** makes its statement true or raises, a **query** answers, a **declaration** is handed to the host, a **pure** name computes (the only kind a recipe's top level calls). A recipe of spec "3" sees exactly these; spec 1 and 2 also have the compatibility layer below.

## Standard Lua in spec 3

Globals: `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `print`, `rawequal`, `rawget`, `rawlen`, `rawset`, `select`, `setmetatable`, `getmetatable`, `tonumber`, `tostring`, `type`, `xpcall`, `import`.

Libraries, whole: `string`, `table`, `math`, `utf8`; of `os`: `os.time`, `os.date`, `os.clock`, `os.getenv`.

## `fs` — files and directories

`import("xim.libxpkg.fs")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `fs.exists(path) -> boolean` | query | path exists; a link is followed | — | — |
| `fs.is_file(path) -> boolean` | query | path is a regular file | — | — |
| `fs.is_dir(path) -> boolean` | query | path is a directory | 0.0.68 | — |
| `fs.is_symlink(path) -> boolean` | query | path is a symbolic link | — | — |
| `fs.readlink(path) -> string\|nil` | query | the target of the link at path; nil when path is no link | — | — |
| `fs.read(path) -> string` | query | the bytes of the file; raises when it cannot be read | 0.0.68 | — |
| `fs.list(dir) -> { {name, path, type}, ... }` | query | the entries of dir sorted by name, type "file" \| "dir" \| "symlink" \| "other"; raises when dir is not a directory | 0.0.68 | — |
| `fs.mkdir(path)` | action | path is a directory, its parents made; already one is done | 0.0.68 | — |
| `fs.remove(path)` | action | path does not exist: a file, a link or a whole tree; never there is done | 0.0.68 | — |
| `fs.move(src, dst [, { replace = true }])` | action | dst is what src was and src is gone; dst is the final path, its parents made; one already there is an error unless replace; across devices it is copied and removed | 0.0.68 | — |
| `fs.copy(src, dst [, { replace = true }])` | action | dst is a copy of src (a directory whole, links kept as links); dst as for move | 0.0.68 | — |
| `fs.symlink(target, link)` | action | link is a symbolic link to target | 0.0.68 | Windows may need a privilege to make one |
| `fs.write(path, text)` | action | the file at path holds text, its parents made | 0.0.68 | — |
| `fs.chmod(path, mode)` | action | path has the mode: octal digits ("755") or "+x" | 0.0.68 | does nothing on Windows (after checking path exists) |
| `fs.extract(archive, dir [, { strip = n }])` | action | dir holds the archive's members, the first n components of each path dropped; a member with fewer is an error | 0.0.68 | — |

## `system` — programs and the install's directories

`import("xim.libxpkg.system")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `system.exec(cmd [, { cwd, env, retry, tty, capture }]) -> string\|nil` | action | cmd ran and succeeded, else it raises with the command and how it ended. cmd is a list { program, arg, ... } (no shell) or one shell line; capture returns its stdout | — | a list naming a batch file is refused on Windows |
| `system.run(cmd [, { cwd, env }]) -> { ok, code, out [, error] }` | query | what cmd does: its exit code and stdout; a failure or a program that cannot start (code 127) is an answer | 0.0.68 | — |
| `system.admin_step(argv, why)` | declaration | a step only an administrator may take, for the host to ask about once after the install | — | — |
| `system.rundir() -> string` | query | the directory xlings runs from | — | — |
| `system.xpkgdir() -> string` | query | the store of installed packages | — | — |
| `system.bindir() -> string` | query | the directory of the home's program shims | — | — |
| `system.subos_sysrootdir() -> string` | query | the active SubOS's root | — | — |
| `system.xpkg_args() -> { string, ... }` | query | the arguments the install was given | — | — |

## `pkginfo` — what is being installed

`import("xim.libxpkg.pkginfo")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `pkginfo.name() -> string` | query | the package's name | — | — |
| `pkginfo.version() -> string` | query | the version being installed | — | — |
| `pkginfo.install_dir() -> string` | query | where the payload is | — | — |
| `pkginfo.install_file() -> string` | query | the downloaded file (read-only) | — | — |
| `pkginfo.platform() -> string` | query | the install's platform: linux, macosx, windows | 0.0.68 | — |
| `pkginfo.arch() -> string` | query | the install's architecture, by the format's name: x86_64, aarch64, ... | 0.0.68 | — |
| `pkginfo.deps_list() -> { string, ... }` | query | the dependencies as the recipe wrote them | — | — |
| `pkginfo.resolved_dep(name [, version]) -> table\|nil` | query | a runtime dependency as the resolver settled it | — | — |
| `pkginfo.dep_install_dir(name [, version]) -> string\|nil` | query | where a dependency's payload is | — | — |
| `pkginfo.build_dep(name [, version]) -> table\|nil` | query | a build dependency's payload | — | — |
| `pkginfo.tool_payload_dir(package [, version]) -> string\|nil` | query | where a tool package's payload is | — | — |
| `pkginfo.with_build_deps_on_path(names, fn)` | action | fn runs with those build dependencies' programs first on PATH | — | — |

## `path` — path strings

`import("xim.libxpkg.path")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `path.join(part, ...) -> string` | pure | the parts joined with "/" | 0.0.68 | — |
| `path.filename(p) -> string` | pure | the last component | 0.0.68 | — |
| `path.directory(p) -> string` | pure | everything before the last component | 0.0.68 | — |
| `path.is_absolute(p) -> boolean` | pure | "/..." or a drive letter | 0.0.68 | — |

## `log` — what the recipe says

`import("xim.libxpkg.log")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `log.debug(fmt, ...)` | declaration | a line of the hook log only | — | — |
| `log.info(fmt, ...)` | declaration | a line of progress, shown with -v | — | — |
| `log.warn(fmt, ...)` | declaration | a notice to the user, shown after the summary | — | — |
| `log.error(fmt, ...)` | declaration | a notice to the user, shown after the summary | — | — |

## `xvm` — what the home registers

`import("xim.libxpkg.xvm")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `xvm.add(name [, opt])` | declaration | register a program, library or version; opt.completion: the program's shell completion files | — | — |
| `xvm.files(opt)` | declaration | place a file of the payload into the SubOS | — | — |
| `xvm.remove(name [, version])` | declaration | unregister it | — | — |
| `xvm.use(name, version)` | declaration | make a version the active one | — | — |
| `xvm.setup(name, opt)` | declaration | register a package's programs, libraries and headers at once | — | — |
| `xvm.teardown(name, opt)` | declaration | unregister what setup registered | — | — |
| `xvm.has(name [, version]) -> boolean` | query | it is registered | — | — |
| `xvm.info(name [, version]) -> table\|nil` | query | what is registered for it | — | — |

## `subos` — the SubOS's environment

`import("xim.libxpkg.subos")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `subos.env(opt)` | declaration | a variable every process in the SubOS gets | — | — |

## `pkgmanager` — other packages

`import("xim.libxpkg.pkgmanager")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `pkgmanager.install(target)` | declaration | install another package after this one | — | — |
| `pkgmanager.remove(target)` | declaration | remove another package after this one | — | — |

## `elfpatch` — the loader and search paths of ELF and Mach-O files

`import("xim.libxpkg.elfpatch")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `elfpatch.set(opt)` | declaration | how the host patches the payload after install() | — | — |
| `elfpatch.skip()` | declaration | the host patches nothing | — | — |
| `elfpatch.set_interpreter(target, interpreter [, opt])` | action | the files' PT_INTERP is interpreter; raises when files are named and patchelf is missing | — | — |
| `elfpatch.set_rpath(target, rpath [, opt])` | action | the files' search path is rpath; raises when files are named and the tool is missing | — | — |
| `elfpatch.patch_elf_loader_rpath(target, opt) -> table` | action | loader and search path together; raises when files are named and the tool is missing | — | — |
| `elfpatch.relocate_build_paths(opt)` | action | build-machine paths in the payload's text files rewritten | — | — |
| `elfpatch.host_link_interposer(opt)` | action | an interposer linked against the host's driver | — | — |
| `elfpatch.closure_lib_paths(opt) -> { string, ... }` | query | the library directories of the install's closure | — | — |
| `elfpatch.is_shrink() -> boolean` | query | the recipe asked for shrunk search paths | — | — |

## `json` — JSON text

`import("xim.libxpkg.json")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `json.encode(value) -> string` | pure | value as JSON text | — | — |
| `json.decode(text) -> value` | pure | the value JSON text holds | — | — |

## `base64` — base64 text

`import("xim.libxpkg.base64")`

| Name | Kind | Holds after it / answers | Since | Platforms |
|---|---|---|---|---|
| `base64.encode(bytes) -> string` | pure | bytes as base64 | — | — |
| `base64.decode(text) -> string` | pure | the bytes base64 text holds | — | — |

## The compatibility layer (spec 1 and 2)

Frozen: no name is added or changed. What a recipe of spec "3" writes instead:

| Name | In spec 3 |
|---|---|
| `os.mv` | fs.move(src, final_path) |
| `os.trymv` | pcall(fs.move, src, final_path) |
| `os.cp` | fs.copy(src, final_path) |
| `os.mkdir` | fs.mkdir |
| `os.tryrm` | fs.remove |
| `os.isfile` | fs.is_file |
| `os.isdir` | fs.is_dir |
| `os.dirs` | fs.list |
| `os.exec` | system.exec |
| `os.execute` | system.run |
| `os.iorun` | system.run(cmd).out |
| `os.cd` | system.exec's cwd option |
| `os.setenv` | system.exec's env option |
| `os.addenv` | system.exec's env option |
| `os.host` | pkginfo.platform |
| `os.arch` | pkginfo.arch |
| `os.sleep` | nothing: it is not needed |
| `os.remove` | fs.remove |
| `os.rename` | fs.move(src, final_path) |
| `os.tmpname` | nothing: it is not needed |
| `io.readfile` | fs.read |
| `io.writefile` | fs.write |
| `io.open` | fs.read / fs.write |
| `io.popen` | system.run |
| `path` | import("xim.libxpkg.path") |
| `format` | string.format |
| `raise` | error |
| `is_host` | pkginfo.platform |
| `is_plat` | pkginfo.platform |
| `is_arch` | pkginfo.arch |
| `try` | pcall |
| `cprint` | log.info |
| `fs.is_directory` | fs.is_dir |
| `fs.mkdir_p` | fs.mkdir |
| `fs.remove_all` | fs.remove |
| `fs.copy_file` | fs.copy |
| `fs.entries` | fs.list |
| `fs.files` | fs.list |
| `fs.dirs` | fs.list |
| `fs.basename` | path.filename |
| `fs.dirname` | path.directory |
| `system.run_in_script` | system.exec, or system.admin_step for a step that needs an administrator |
| `system.unix_api` | nothing: it is not needed |
| `log.set_level` | nothing: it is not needed |
| `log.get_level` | nothing: it is not needed |
| `xvm.log_tag` | nothing: it is not needed |
| `pkgmanager.uninstall` | pkgmanager.remove |
| `elfpatch.auto` | elfpatch.set / elfpatch.skip |
| `elfpatch.is_auto` | nothing: it is not needed |
| `json.loadfile` | json.decode(fs.read(path)) |
| `json.savefile` | fs.write(path, json.encode(value)) |
| `utils.filepath_to_absolute` | path.join |
| `utils.try_download_and_check` | nothing: it is not needed |
| `utils.input_args_process` | nothing: it is not needed |
| `import of an unknown module` | an error: the module's name is checked |
