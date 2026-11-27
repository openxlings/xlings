# The xpkg format

> Status: in force. This is the specification of libxpkg 0.0.68.
> Source: libxpkg `docs/spec/xpkg.md` (https://github.com/openxlings/libxpkg),
> where it is edited. A copy, byte for byte the one of the libxpkg version
> xlings builds with, is xlings `docs/specs/xpkg.md`
> (https://github.com/openxlings/xlings/blob/main/docs/specs/xpkg.md), the one
> the site publishes; xlings' checks keep the two the same.
> Reference implementation: libxpkg (`import mcpplibs.xpkg;`). Executable
> half: the conformance cases, libxpkg `tests/conformance/`.
> The hook interface's reference, generated from libxpkg's interface manifest:
> xlings `docs/generated/xpkg-hook-api.md`.

It replaces "xpkg manifest v1" (xlings `docs/specs/xpkg-manifest-v1.md`) and
the format part of "XPackage V2" (xim-pkgindex `docs/specs/xpackage-v2.md`,
which keeps the official index's own conventions).

## 1. Names

| Word | Meaning |
|---|---|
| xpkg | the format |
| recipe | one file of it: a Lua chunk defining the global table `package` and, optionally, hook functions |
| `spec` | the format's version, a field of the recipe: `"1"`, `"2"`, `"3"` |
| index | a directory of recipes (`pkgs/`), the libs they share (`libs/`) and the index's own declarations (`index-compat.json`) |
| host | the program that reads recipes and installs packages (xlings; mcpp reads them too) |
| payload | what an install puts in the install directory, `pkginfo.install_dir()` |
| entry | one version of one platform in `xpm`: `xpm.<platform>["<version>"]` |

An item of this document says since which `spec` and which libxpkg it holds;
an item without a version is as old as the format. A host built with a
libxpkg reads what that libxpkg reads.

## 2. Versions (`spec`)

| `spec` | Since | What it is |
|---|---|---|
| `"1"`, or none | — | the base format; hooks run in the environment of spec 1 and 2 (§6.4) |
| `"2"` | libxpkg 0.0.42 | the same environment; the recipe states the multi-architecture entry shapes of §4.3 (they are read for every spec; `"2"` says the recipe relies on them) |
| `"3"` | libxpkg 0.0.68 | the hook interface of §6 without the compatibility layer; the host lands the download before `install()` (§5.2); an entry with an artifact enumerates its architectures (§4.4); the hooks are `installed`, `install`, `config` and `uninstall` |

- A recipe without `spec`, or with a value that names no spec, reads as `"1"`.
- **The ceiling.** A host reads up to the highest `spec` its libxpkg knows
  (0.0.68: `"3"`). A recipe of a higher one is read (its `package` table is
  listed and searched) and refused for that package alone before any hook
  runs, naming the update that reads it; the other packages of the same
  command are not affected, and the command fails when a package it was asked
  for is refused.
- What changes between specs is in §9.

## 3. The `package` table

A recipe defines the global `package` at its top level. Its top level is
evaluated whenever the recipe is read (search, an index build, an install), so
it only computes: a recipe of spec `"3"` may call only `pure` names there
(§6.2); the host's work happens in hooks. Each read is for a platform and an
architecture -- the loader's for the target it reads, the hook runtime's for
the install -- and a recipe of spec 1 or 2 that computes from them at its top
level (`pkginfo.platform()`, `pkginfo.arch()`, `os.host()`) is told the same
at every read.

### 3.1 Fields

| Field | Type | Required | Meaning |
|---|---|---|---|
| `spec` | string | — | §2 |
| `name` | string | yes | lower case, unique within the index; the recipe's file is `pkgs/<first letter>/<name>.lua` |
| `type` | string | — | §3.2; default `"package"` |
| `xpm` | table | yes | the platform and version matrix, §4 |
| `namespace` | string | — | a namespace such as `"subos"`, separating names |
| `description` | string | — | one sentence |
| `licenses` | string[] | — | SPDX identifiers |
| `homepage`, `repo`, `docs` | string | — | URLs |
| `authors`, `maintainers` | string[] | — | |
| `categories`, `keywords` | string[] | — | for search |
| `status` | string | — | `"dev"` (default), `"stable"`, `"deprecated"` |
| `archs` | string[] | — | §3.3 |
| `programs` | string[] | — | the executables the package provides; its `config()` registers them (§7), and a host fails an install that registers none of them |
| `xvm_enable` | boolean | — | the package registers with xvm |
| `extensions` | table | — | §3.4 (libxpkg 0.0.66) |
| `host_facts` | table | — | §3.5 (libxpkg 0.0.67) |

A field the loader does not know is ignored. A field the host must understand
belongs in this specification, not in `extensions`.

### 3.2 `type`

| Value | For |
|---|---|
| `"package"` | a package (the default) |
| `"script"` | a script tool |
| `"template"` | a project template |
| `"config"` | a configuration set |
| `"subos"` | a SubOS environment; a missing hook is the host's default (a skeleton directory and its registration) |

### 3.3 `archs`

`archs` is the union of the architectures the package's entries provide on
every platform, for search and the site. It does not accept or refuse an
entry: one platform's entry says what it ships (§4.4). An index's checker
holds it equal to that union (§8.4).

### 3.4 `extensions` (libxpkg 0.0.66)

Data for programs other than the host, one table per program, keyed by a
namespace naming the program that reads it (lower-case letters, digits, `-`).
The host keeps it and hands it on unchanged (as a JSON object with sorted
keys); it never reads it.

- Values: strings, numbers, booleans, arrays and tables, nested. A function or
  userdata, a number that is not finite, a non-string key outside an array, a
  table nested deeper than 16 is dropped when the recipe is read, with a
  loader warning.
- Size: at most 4 KiB as JSON; over that the whole table is dropped with a
  warning, never cut.
- It changes nothing installed: changing it raises no `revision`.
- Namespaces in use: `luban` (luban and the Luban OS app; xim-pkgindex
  `docs/12-luban-packages.md`), `autoupdate` (the official index's updater).

### 3.5 `host_facts` (libxpkg 0.0.67)

The host files a package's `config()` reads to decide what it configures,
per platform, as absolute paths (`/...`, `C:\...`, `C:/...`, `\\server\...`):

```lua
host_facts = { linux = { "/sys/module/nvidia/version" } },
```

- Data, never a hook: the host reads the files, the recipe's code is not run
  to check them. An entry that is not an absolute path, a value of another
  type and a list in place of the table are dropped with a loader warning.
- The host records, when `config()` succeeds in a scope, each path and its
  state (`sha256:<hex>`, `absent`, `unreadable`); where the configuration is
  used and the state differs, `config()` runs again in that scope.
- `config()` generates what depends on the fact from the fact as it reads then
  (a glob over the host's directory, not a list of names): running it again is
  the whole repair. Adding or changing `host_facts` raises no `revision`.

## 4. `xpm`

`xpm` maps a platform name (`linux`, `macosx`, `windows`) to that platform's
table. A host reads the table of its own platform.

### 4.1 A platform's table

A string key is a version and its value an entry (§4.3). These keys are not
versions:

| Key | Meaning | Since |
|---|---|---|
| `deps` | dependencies, §4.8 | — |
| `inherits` | the name of a platform whose versions, `deps`, `exports` and `source` this one takes where it states none; a cycle or an unknown platform stops the chain | libxpkg 0.0.64 (`{ ref = "<platform>" }` reads the same) |
| `exports` | what the package exposes to consumers at run time (`exports.runtime.loader`, `libdirs`, `abi`) | — |
| `source` | the platform's default source, §4.5 | libxpkg 0.0.44 |
| `strip` | the default of the entries' `strip`, §4.6 | spec 3, libxpkg 0.0.68 |

Platforms carrying the same versions name one table defined before `package`
(`local versions = { ... }`, `xpm = { linux = versions, macosx = versions }`).

### 4.2 Architectures

A recipe writes an architecture by its canonical name; the aliases are read as
it:

| Canonical | Aliases | ELF machine, class |
|---|---|---|
| `x86_64` | `amd64`, `x64`, `x86-64` | 62, 64 |
| `aarch64` | `arm64`, `armv8` | 183, 64 |
| `x86` | `i386`, `i686` | 3, 32 |
| `arm` | `armv7`, `armv7l`, `armhf` | 40, 32 |
| `riscv64` | — | 243, 64 |
| `loongarch64` | `loong64` | 258, 64 |

The table is libxpkg's (one table, read by the loader, `elfpatch` and the
host's payload rules; libxpkg 0.0.68). The host's machine is the architecture
its own process is built for: an x86_64 host under emulation installs x86_64
payloads.

### 4.3 Entries

An entry is one of:

| Shape | Example | Since |
|---|---|---|
| a URL | `["1.0.0"] = "https://…/foo-1.0.0.tar.gz"` | — |
| `"XLINGS_RES"` | the official resource server's URL for it, with no checksum | — |
| a table | `{ url = "<url>" or { GLOBAL = …, CN = … }, sha256 = "<h>" }` | — |
| per-architecture checksums | `{ url = "<url or template>", sha256 = { x86_64 = "<h>", aarch64 = "<h>" } }` | libxpkg 0.0.42 |
| a per-architecture map | `{ x86_64 = { url = …, sha256 = … }, aarch64 = { url = …, sha256 = … } }` | libxpkg 0.0.42 |
| a resource | `{ res = true, sha256 = { x86_64 = …, aarch64 = … } }`: the resource server's URL, with checksums | libxpkg 0.0.42 |
| from the source | `{ sha256 = { … } }` with `xpm.source` (§4.5) | libxpkg 0.0.44 |
| an alias | `{ ref = "<version>" }`, §4.7 | — |
| made by hooks | `{}`: no download; the hooks make the payload | — |

An entry's table may also hold:

| Field | Meaning | Since |
|---|---|---|
| `arch_alias` | `{ <arch> = "<upstream's word>" }`, for `${arch_alias}` | libxpkg 0.0.42 |
| `revision` | an integer ≥ 0, default 0: how many times what this version installs has changed while the upstream version has not | libxpkg 0.0.58 |
| `floor` | `0 ≤ floor ≤ revision`, default 0: the oldest installed revision still usable | libxpkg 0.0.65 |
| `min_client` | the oldest host version that can use this version | libxpkg 0.0.64 |
| `strip` | §4.6 | spec 3, libxpkg 0.0.68 |

`revision`, `floor` and `min_client` sit on the entry, never inside a
per-architecture map or on an alias. A URL template substitutes `${name}`,
`${version}`, `${os}` (`linux`, `macosx`, `windows`), `${arch}` (canonical),
`${arch_alias}` and `${ext}` (`zip` on Windows, else `tar.gz`). A mirror table
maps a region (`GLOBAL`, `CN`) to a URL of the same bytes.

**Revision and floor.** A host records the revision it installed with the
payload (a record without one reads as 0). A payload is *current* when its
recorded revision equals the entry's `revision` and *usable* when it is
complete and its recorded revision is at least `floor` (a floor above the
revision reads as the revision). An install replaces a payload that is not
current, saying why (`revision 0 -> 1`), and keeps the old one until the new
one is in place; a command that only uses a package keeps a usable payload. A
published URL and its checksum never change: a rebuilt asset gets a new URL
and a raised `revision`.

**min_client.** A host resolving a package with no version named passes over
the versions it is older than and takes the newest it can use, saying so; a
version named explicitly is that version.

### 4.4 What an entry says about architecture

An entry with an artifact states which architectures it ships:

| The artifact is | It writes | A host |
|---|---|---|
| one per architecture | a per-architecture map, or a URL template with per-architecture checksums | refuses an architecture not listed before downloading |
| for one architecture | a fixed URL, `sha256 = { x86_64 = "<h>" }` | the same |
| the same bytes for several (scripts, data, a jar) | a fixed URL, the same `<h>` for each: `sha256 = { x86_64 = "<h>", aarch64 = "<h>" }` | installs it on each listed architecture |
| unstated | one URL, one checksum string | installs it; the host's payload rule (§5.4) decides |

- An architecture an entry does not list is not this entry's: the host names
  the package and its architecture before anything is downloaded, and for a
  closure names every package that has no artifact for it, at once.
- "The same bytes" is a claim the payload rule checks on every listed
  architecture; a new host architecture adds a key to such entries.
- A git repository (a URL ending in `.git`) is a source the hooks build, not
  an artifact: it names no architecture.
- **spec `"3"`**: an entry with an artifact enumerates its architectures (the
  first three rows) on a platform with more than one host architecture
  (today: `linux`). On a platform with one (`macosx`: aarch64, `windows`:
  x86_64), an unstated entry is that architecture's, and counts as it in
  `archs` (§3.3). A recipe of spec 1 or 2 may leave it unstated;
  the checker reports `arch-unstated` (§8.4).

### 4.5 `source` (libxpkg 0.0.44)

`xpm.source`, or a platform's `source` (which wins), is the default URL of the
entries: `"xlings-res"` (the resource server the user configured), a URL
template, or a map of regions to templates (`{ GLOBAL = …, CN = … }`, the same
bytes). An entry's own `url` wins over it.

### 4.6 `strip` (spec 3, libxpkg 0.0.68)

`strip = n` drops the first n components of every member's path when the host
lands the download in the install directory (§5.2): an archive with one
top-level directory is `strip = 1`. The entry's `strip` wins over its
platform's; none is 0, the archive's own layout. A member with n components
or fewer is an error that names it (the declaration and the archive disagree),
never a file left out. A download that is no archive is placed as it is, and
`strip` does not apply. A recipe of spec 1 or 2 is landed by its hook; the
host does not read its `strip`.

### 4.7 Aliases and `ref`

An entry `{ ref = "<version>" }` makes its key another name of that version:
`latest`, `lts`, `stable`. A host installs what it names, records the alias
the user asked for, and an update follows the alias to what it names then.
An alias carries no `revision`; the entry it ends at does. An entry whose
`ref` names its own key is that version (libxpkg 0.0.68), not a cycle; two
versions naming each other are a cycle and an error.

A version key that is a concrete version (`2.44`, `2.44.3`) is always that key
exactly: an installed `2.44.3` does not answer a request for `2.44` when the
index has a `2.44` key.

### 4.8 Dependencies

`deps` sits on a platform's table:

```lua
deps = { "node", "npm" }                                   -- runtime and build alike
deps = { runtime = { "python" }, build = { "cmake", "ninja" } }
```

- `runtime`: activated in the workspace after the install. `build`: available
  only while the hooks run (`PATH`, `XLINGS_BUILDDEP_<NAME>_PATH`), not
  activated.
- A bare name means the package of that name in the index the recipe is in;
  only a name that index lacks falls through to the host's other indexes. A
  version the declaring index cannot satisfy is an error naming it.
- A version constraint after `@` (the same grammar as an install's version):
  exact (`2.15.0.1`: the parts written, at least three), a prefix range (`15`,
  `15.1`, `1.2.*`), `>=`, `^`, `~`. Versions compare part by part, numbers as
  numbers, letters lexically, a number above a letter (`6.5` > `6.5rc1`), a
  missing part as 0, a pre-release below its release.

### 4.9 Resolution

For the host's platform, architecture and the version asked for:

1. follow `ref` to the entry it ends at;
2. a per-architecture map: the host's architecture's `{ url, sha256 }`, or no
   artifact for it;
3. the entry's `url` and checksum (per architecture when a table);
4. the platform's `source`, then `xpm.source`;
5. expand the resource server or the template, and take the host
   architecture's checksum;
6. otherwise the single URL or `"XLINGS_RES"`;
7. the region's mirror, after all of the above.

A multi-architecture entry without the host's checksum is no artifact for it.
An entry with no hash at all downloads unverified (for old recipes' sake).

## 5. Lifecycle

### 5.1 Hooks

| Hook | Runs | Returns |
|---|---|---|
| `installed()` | before an install, to ask what is there | a version string, or nil |
| `install()` | once the download is in place (§5.2); adjusts the payload | nothing or `true` succeeds; `false` fails |
| `config()` | after a successful install, once per scope (a SubOS, the system); registers, sets the environment | the same |
| `uninstall()` | when the package is removed; withdraws what `config()` did that the host does not withdraw itself | the same |

- spec `"3"` defines these four; `build()` is no hook of spec 3 (the checker
  warns). A missing hook is nothing to do (for `type = "subos"`, the host's
  default).
- A hook fails by returning `false`, by raising an error, or by an action
  that is not done (§6.1). A failed step is the package's failure: the
  host removes the unfinished payload, registers nothing and the command fails.
- A command a hook runs writes to the hook's log
  (`<home>/logs/hooks/<ns>-<name>@<ver>.<hook>.log`); `system.exec(cmd, { tty = true })`
  reaches the terminal.
- The host keeps the recipe text a package was installed with (the file and
  its index's template) in the payload, and removes the package with it.

### 5.2 Landing the download

**spec `"3"` (libxpkg 0.0.68).** When `install()` starts, the download is in
`pkginfo.install_dir()`: an archive extracted with the entry's `strip`, any
other file copied in under its name. `install()` only adjusts (renames,
modes, wrappers, patches) and may be absent. There is no directory beside the
archive to find; `pkginfo.install_file()` names the download, read only. An
archive inside the archive (`data.tar` in a `.deb`), or one to put elsewhere,
is `fs.extract`'s.

**spec `"1"` and `"2"`.** The archive is extracted beside the download, in a
directory of its own for this install (`install_file()` without its
extension); `install()` moves what it needs into `install_dir()`. A package
without `install()` gets the archive's members in `install_dir()`, its layout
unchanged. When `install()` reports success, leaves `install_dir()` empty and
made a call of the compatibility layer that failed (§6.4), the step fails,
naming those calls.

### 5.3 What the host does after the hooks

1. `elfpatch`: the loader and search paths of the payload's ELF and Mach-O
   files are set as `elfpatch.set` declared (by default, to the package's
   closure); files for another machine are skipped and counted. A file to
   patch with no patch tool fails the step.
2. The payload rules: what the host would run or load (the programs the
   package registers, the libraries directly in its exported library
   directories, its exported loader) is of the host's machine (§4.2); a file
   of another machine fails the install. A path of the machine the payload was
   built on, in a search path, a `.pc` or a `.la` file, is a finding.
3. Registration: what `config()` declared (§7) is made, then the install is
   recorded with its revision.

## 6. The hook environment

### 6.1 Calls

Every name of the interface is one of four kinds:

| Kind | It | When it cannot |
|---|---|---|
| action | changes the machine so that its statement holds when it returns | raises, naming the action, its path and why; its return value means nothing |
| query | answers, changing nothing ("there is none" is an answer) | raises only when it cannot answer |
| declaration | hands something to the host, which does it after the hook | — |
| pure | computes from its arguments alone | — |

A failure a hook tolerates is written with Lua's `pcall`. An action whose
target exists already (`fs.move`, `fs.copy`) is an error unless it is given
`{ replace = true }`; an action that is already true (`fs.mkdir` of a
directory, `fs.remove` of a path that is not there) is done.

### 6.2 spec `"3"` (libxpkg 0.0.68)

A recipe of spec 3, and each index lib it imports, runs in an environment of
its own:

- of standard Lua: `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`,
  `print`, `rawequal`, `rawget`, `rawlen`, `rawset`, `select`,
  `setmetatable`, `getmetatable`, `tonumber`, `tostring`, `type`, `xpcall`;
  `string`, `table`, `math`, `utf8` whole; `os.time`, `os.date`, `os.clock`,
  `os.getenv`. No `io`, no `os.execute`, no `load`, no `require`: what
  changes the machine goes through a module.
- `string` has three helpers every spec has, pure and on plain text:
  `s:split(sep [, plain])` (a list), `s:trim()` (without the white space at
  either end), `s:replace(old, new)` (every `old`, not a pattern).
- `import("xim.libxpkg.<module>")` gives the module as a table of exactly its
  names in the interface (the generated reference lists them, with kind,
  signature, what holds after, the version that added it and what differs by
  platform). A module that is not in the interface is an error, not a stub.
- `import("xim.pkgindex.<lib>")` runs the index's `libs/<lib>.lua` in an
  environment of the same rules and gives what it returns.
- The modules: `fs` (files), `system` (programs and the install's
  directories), `pkginfo` (what is installed), `path` (path strings), `log`,
  `xvm`, `subos`, `pkgmanager`, `elfpatch`, `json`, `base64`.
- At the top level a recipe calls only `pure` names; anything else is an error
  there (the top level also runs when nothing is being installed).
- `system.exec` runs a command as a list `{ program, arg, ... }`, with no
  shell (each argument reaches the program as written), or as one shell line;
  `cwd`, `env` and `capture` replace changing the hook's own directory or
  environment. On Windows a list naming a batch file is refused.
- `pkginfo.platform()` and `pkginfo.arch()` are the install's platform and
  architecture, facts the host gives; a recipe does not probe the machine.
- The checker reports every name of the compatibility layer in a recipe of
  spec 3 as an error, with what replaces it.

### 6.3 Names that replace the compatibility layer's

| spec 1 and 2 | spec 3 |
|---|---|
| `os.isfile`, `os.isdir`, `fs.is_directory` | `fs.is_file`, `fs.is_dir` |
| `os.tryrm`, `fs.remove_all`, `os.remove` | `fs.remove` |
| `os.mkdir`, `fs.mkdir_p` | `fs.mkdir` |
| `os.mv`, `os.trymv`, `os.rename`, `os.cp`, `fs.copy_file` | `fs.move`, `fs.copy`: the destination is the final path (`path.join(dir, name)` where `os.mv` and `os.cp` went into a directory), and one already there is replaced only with `{ replace = true }` |
| `io.readfile`, `io.writefile`, `io.open` | `fs.read`, `fs.write` |
| `os.dirs`, `fs.files`, `fs.dirs`, `fs.entries` | `fs.list` |
| running `chmod` | `fs.chmod` |
| running `tar`, `unzip` | landing (§5.2), `fs.extract` |
| `os.exec` | `system.exec` (both fail the hook when the command fails) |
| `os.execute` | `system.run` (neither fails on the command's status); `system.exec` when a failure should fail the hook |
| `os.iorun`, `io.popen` | `system.run(cmd).out` (neither fails: a failed command's output is the answer); `system.exec(cmd, { capture = true })` when a failure should fail the hook |
| `os.cd`, `os.setenv`, `os.addenv` | `system.exec`'s `cwd` and `env` |
| `os.host()`, `is_host(x)`, `os.arch()` | `pkginfo.platform()`, `pkginfo.arch()` |
| the global `path` | `import("xim.libxpkg.path")` |
| `format`, `raise`, `cprint`, `try` | `string.format`, `error`, `log.info`, `pcall` |
| `system.run_in_script`, `sudo` | `system.exec`; `system.admin_step` for a step only an administrator takes |
| `utils.*` | no module: an index keeps what it needs in a lib |

The table is the interface manifest's (`compat`); the reference and the
checker read it from there.

### 6.4 spec `"1"` and `"2"`: the compatibility layer

A recipe of spec 1 or 2 runs with the compatibility layer: the names of the
left column above, the global `path`, `format`, `raise`, `is_host`, `try`,
`cprint`, the module names it lists, and an `import` that answers an unknown
module with a stub. Its behaviour is frozen: no name is added or changed, and
a recipe written against it keeps working. Two things differ since libxpkg
0.0.68:

1. `fs.symlink` and `fs.remove` are the module's, the same in both
   environments: they raise when not done, and `fs.remove` removes a whole
   tree.
2. A compatibility action that fails (`os.mv`, `os.cp`, `os.mkdir`,
   `os.exec`) still returns as before, and is reported with the hook's result
   (the call, its arguments, why). A host shows it as a finding, and §5.2's
   rule for an empty payload uses it.
3. `import("xim.pkgindex.<lib>")` of a lib the index does not have, or one
   that cannot be loaded, still answers with a stand-in, so the recipe can be
   read; a call to it in a hook raises (the member, the lib, why): the hook
   fails unless the recipe handles it, and what the stand-in would answer
   never reaches the next action. A host keeps a payload's recipe with the
   libs it imports (§8.1), so its hooks find the libs they ran with.

A recipe written for every client probes a capability newer than the oldest
it supports: a new function of a module an old client has, by its presence
(`if xvm.files then`); a function of a module an old client lacks, by its type
(`if type(subos.env) == "function" then`; the stub is a callable table).
spec 3 needs no probe: its reader has every name of its interface, and an
unknown module is an error.

## 7. Declarations

| Name | Declares | Since |
|---|---|---|
| `xvm.add`, `xvm.remove`, `xvm.use`, `xvm.setup`, `xvm.teardown` | programs, libraries and headers the home registers for the package, and which version is active | — |
| `xvm.files` | a file of the payload placed into the SubOS | libxpkg 0.0.47 |
| `xvm.add(name, { completion = { bash, zsh, fish, pwsh } })` | the program's shell completion: files of the payload (relative to its root), placed where each shell looks in the SubOS (`usr/share/bash-completion/completions/<name>`, `usr/share/zsh/site-functions/_<name>`, `usr/share/fish/vendor_completions.d/<name>.fish`, `usr/share/powershell/completions/<name>.ps1`) and bound to the program's release: they follow `use`, go with the package, and the host's shell profile loads them. A program that prints its completion writes it into the payload in `install()`. A package never edits the host's shell profiles | libxpkg 0.0.68 |
| `subos.env` | a variable every process in the SubOS gets: `{ var, op = "set" or "prepend", value, binding }`; `value` uses `${pkgdir}`, `${subosdir}`, `${home}`, `${xlings_home}`, never an absolute path; removed with the package, never by `uninstall()` | libxpkg 0.0.48 |
| `system.admin_step(argv, why)` | a step only an administrator may take; the host asks once after the install, through its one door for it, or keeps it for its repair command. A recipe never runs `sudo` | libxpkg 0.0.63 |
| `elfpatch.set(opt)`, `elfpatch.skip()` | how the host patches the payload after `install()` (§5.3) | — |
| `pkgmanager.install`, `pkgmanager.remove` | another package installed or removed after this one | — |
| `log.debug`, `log.info`, `log.warn`, `log.error` | §7.1 | — |

Registrations belong to the package: the host withdraws them when it removes
it.

### 7.1 What a recipe says (libxpkg 0.0.68)

| Level | Goes to | Means |
|---|---|---|
| `log.debug` | the hook's log | detail |
| `log.info` | the hook's log; the terminal with `-v` | progress |
| `log.warn` | the user, after the command's summary; the log; the host's events | something the user should know or do |
| `log.error` | the same | something went wrong that the user should know |

A warning is a notice, not a result: it changes no step's outcome. A hook
that has failed fails; a hook that warns and returns `true` succeeded. Use
`warn` for what the user acts on (a restart, a setting), `info` for progress.

## 8. The index

### 8.1 Layout

```
pkgs/<first letter>/<name>.lua   the recipes
libs/<lib>.lua                   libs the recipes import (xim.pkgindex.<lib>)
index-compat.json                what the index says about itself (optional)
xim-indexrepos.lua               sub-indexes it names (optional)
```

A host that keeps an installed package's recipe for its later hooks (an
uninstall) keeps it as an index of that one recipe: the recipe as it ran
(with the template, §8.2) at the same `pkgs/` path, and under `libs/` the
libs it imports, followed through the libs' own imports, as they were. Its
hooks then find what they ran with, whatever the index has become.

### 8.2 `index-compat.json`

```json
{
  "requires": { "xlings": { "min": "2026.10.10.2" } },
  "namespaces": { "reserved": ["xim", "luban", "subos", "config"] },
  "recipes": { "template": "template.lua" }
}
```

- `requires.<consumer>.min` (inclusive) and `max` (exclusive): the hosts that
  can read this snapshot of the index. A host older than `min` uses the newest
  snapshot it can and says why (the index version contract, xlings
  `docs/specs/index-version-contract.md`). An index whose recipes use a name
  or a `spec` a host lacks raises `min` to the first host that has it.
- `namespaces.reserved`: names a sub-index may not take.
- `recipes.template` (libxpkg 0.0.68): a file of the index read after every
  recipe of `pkgs/`, as if it were written at the recipe's end, in memory; the
  files on disk are not changed, and the index's build script
  (`pkgindex-build.lua`, which appended it) is not run. The recipe's `spec`
  applies to the whole. A host before 0.0.68 ignores the key and runs the
  script; an index keeps both until its `min` reaches a host with 0.0.68.

### 8.3 Sub-indexes

`xim-indexrepos.lua` names other indexes (`xim_indexrepos = { ["<ns>"] = "<url>" }`);
their packages are named `<ns>:<name>`.

### 8.4 The checker

libxpkg's checker (`check_recipe`, `check_index`; the host command
`xlings index check [path]`) says what is wrong with a recipe by the rules of
this format, as findings with their lines:

| Finding | Severity | |
|---|---|---|
| `load` | error | the recipe does not load |
| `spec-missing` | warning | no `spec`, or one naming none: read as `"1"` |
| `spec-newer` | error | a `spec` above the checker's |
| `spec-old` | info | spec 1 or 2 |
| `arch-unstated` | error in spec 3, else warning | §4.4 |
| `archs-union` | warning | `package.archs` is not the union of the entries' architectures |
| `compat-name` | error in spec 3, else info | a name of the compatibility layer, with what replaces it |
| `strip` | error in spec 3, else warning | a `strip` that is no component count, or one a host does not read |
| `build-hook` | warning | `build()` in spec 3 |
| `dropped` | warning | a field the loader dropped (§3.4, §3.5) |

An index's own rules (its review requirements, its asset admission) are the
index's, not the format's.

## 9. Evolution

- **Adding a name or a field** raises no `spec`. It says the libxpkg that adds
  it; an index either probes for it (§6.4) or raises its `requires.xlings.min`.
- **Changing what a name means, or removing one,** raises `spec`. The
  environment of a released `spec` only gains names.
- **Deprecating a name**: it moves to the compatibility layer, and the
  interface manifest records what replaces it. The compatibility layer exists
  only for the specs it belongs to (1 and 2).
- **The ceiling**: the highest `spec` a host reads; a higher one is refused
  package by package, naming the update (§2).

What each `spec` changed:

| `spec` | Changes |
|---|---|
| `"2"` | the multi-architecture entry shapes (per-architecture checksums, maps, `res`), `arch_alias` |
| `"3"` | the hook environment without the compatibility layer (§6.2); actions raise; the top level calls only pure names; the host lands the download, `strip` (§5.2, §4.6); an entry with an artifact enumerates its architectures (§4.4); four hooks |

## 10. A recipe of spec 3

```lua
package = {
    spec = "3",
    name = "foo",
    description = "A tool that does one thing",
    licenses = { "MIT" },
    repo = "https://github.com/acme/foo",
    type = "package",
    archs = { "x86_64", "aarch64" },
    programs = { "foo" },

    xpm = {
        linux = {
            strip = 1,
            ["latest"] = { ref = "1.2.0" },
            ["1.2.0"] = {
                url = "https://github.com/acme/foo/releases/download/v1.2.0/foo-1.2.0-linux-${arch_alias}.tar.gz",
                arch_alias = { x86_64 = "amd64", aarch64 = "arm64" },
                sha256 = { x86_64 = "<amd64-sha256>", aarch64 = "<arm64-sha256>" },
            },
        },
        windows = {
            ["1.2.0"] = {
                url = "https://github.com/acme/foo/releases/download/v1.2.0/foo-1.2.0-windows-amd64.zip",
                sha256 = "<sha256>",
            },
        },
    },
}

import("xim.libxpkg.fs")
import("xim.libxpkg.path")
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")

-- The download is already in install_dir; install() only adjusts.
function install()
    if pkginfo.platform() ~= "windows" then
        fs.chmod(path.join(pkginfo.install_dir(), "bin", "foo"), "755")
    end
    return true
end

function config()
    xvm.add("foo", { bindir = path.join(pkginfo.install_dir(), "bin") })
    return true
end

function uninstall()
    xvm.remove("foo")
    return true
end
```
