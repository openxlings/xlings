# Payload rules

> Status: in force. Rules A, B, D and E since 2026.8; M and P since the client
> with libxpkg 0.0.68. Implemented in xlings (`src/core/closure_check.cppm`,
> `src/core/elf_same_source.cppm`, `src/core/xim/machine_rule.cppm`,
> `src/core/xim/build_paths.cppm`) and, for A and B, in mcpp. Checked by
> `tests/e2e/test_payload_rules.cpp`.

What an installed payload -- the files a package puts in its install
directory -- may contain, so that what the host runs from it works on this
machine and nothing in it names a machine it was not made on. One statement
of the rules; each place that enforces one says how hard.

## 1. Three kinds of path

| Kind | Example | Who may write it into a file |
|---|---|---|
| the environment's runtime and the payload itself | `<home>/abi/<family>/...`, `<store>/<pkg>/<ver>/lib` | the host, at install, into the payload's own ELF files (`elfpatch`) and compiler `specs` |
| a SubOS's view | `<subos>/lib` | only registrations (aliases, environment) and what a user builds; never a shared payload |
| the build machine's | a build's temporary directory, another home | nothing that is published |

## 2. The rules

| Rule | A payload ... | Files |
|---|---|---|
| **A** | needs no newer glibc than the one the host recorded | its ELF files' versioned glibc symbols |
| **B** | gives a program its loader and its libc from the same payload | the ELF programs it registers |
| **D** | has a provider for each `DT_NEEDED` of what it registers | the ELF programs and libraries it registers |
| **E** | gives an executable's search path as `DT_RPATH`, which reaches the libraries it loads | the ELF programs it registers |
| **M** | is of the host's machine where the host runs or loads it | its *surface* (below): ELF `e_machine` and class |
| **P** | names no path of the machine it was built on | ELF `DT_RPATH`/`DT_RUNPATH`, `.pc` and `.la` files, a compiler's sysroot |

**The surface (rule M)**: the files the package registers as programs, the
shared libraries directly in its exported runtime library directories
(`exports.runtime.libdirs`, else `lib64` and `lib`), and its exported loader.
Anything else -- a cross toolchain's target libraries, a sysroot, prebuilt
binaries for several machines, firmware -- is the recipe's to answer for; the
rule does not judge it. A payload not yet registered (an asset before it is
published) is judged on a static surface: the files directly in `bin/` and
`sbin/`, and `lib64/` and `lib/`. Only ELF is judged: macOS and Windows have
one host architecture each.

**A path a payload may name (rule P)**: one under this home, the store, the
system layer or the payload itself; one under the operating system's
directories (`/usr`, `/lib`, `/lib64`, `/etc`, `/opt`, `/System`, `/Library`,
...); one with a `$` (`$ORIGIN`). A Windows path names a drive (`C:/...`);
an ELF search path cannot.

## 3. Where each is enforced

| Rule | Publish (the index's CI) | Install (xlings) | Build (mcpp, `ld`) | `self doctor` |
|---|---|---|---|---|
| A | — | finding `closure-glibc-floor` | refused (mcpp) | reported |
| B | — | refused | refused (mcpp) | reported |
| D | refused | finding `closure-missing` | — | reported |
| E | — | finding `closure-non-transitive` | given by the `ld` wrapper | reported |
| **M** | refused: each artifact on each architecture its entry lists it under | **refused**, `E_FOREIGN_MACHINE`: the install fails, nothing is registered | — | reported, `payload-foreign-machine` |
| **P** | refused | finding `payload-build-path` | `mcpp pack` removes them | reported |

- A *finding* is counted in the command's summary and listed with `-v`, in
  the install's record and in `<home>/logs/findings.ndjson`; it does not
  change the outcome. *Refused* is the step's failure.
- Rule P at install only reports: assets published before it exist with
  such paths. The index refuses new ones.
- `self doctor` reads the findings an install recorded; `--deep` scans again.
  Rule M reads its surface's headers each time.
- There is no switch. A false refusal is fixed in the recipe: what the host
  runs is not another machine's.

## 4. The index's check

An index holds an asset to rules M and P before it publishes it, with the
client's own implementation:

```
xlings __payload-check <payload dir> [--arch <name>] [--json]
```

`--arch` is the architecture the entry lists the artifact under (the
format's names, [xpkg](xpkg.md) §4.2; default this machine's); the check
reads headers, runs nothing, and judges an aarch64 artifact on an x86_64
runner alike. Exit 0 when both hold, 1 when one does not (each file named),
2 for a path that is no directory or an unknown architecture. `--json`:
`{ "arch", "ok", "M": [{ "file", "machine" }], "P": { "files", "examples": [{ "file", "path" }] } }`.

## 5. Cases

What each implementation must find (`tests/e2e/test_payload_rules.cpp`
builds them with testkit's ELF writer):

| Payload | Check | Finding |
|---|---|---|
| `bin/tool` an x86_64 ELF program | `--arch x86_64` | none |
| the same | `--arch aarch64` | M: `bin/tool` is for x86_64 |
| `lib/libx.so` an aarch64 ELF library, `share/firmware.bin` an x86_64 ELF | `--arch aarch64` | none (the firmware is off the surface) |
| `lib/sub/libdeep.so` an x86_64 ELF | `--arch aarch64` | none (not directly in a library directory) |
| `bin/tool` with `DT_RUNPATH=/srv/ci/work/lib` | any | P: names `/srv/ci/work/lib` |
| `bin/tool` with `DT_RUNPATH=$ORIGIN/../lib`, `/usr/lib` | any | none |
| `lib/pkgconfig/x.pc` with `prefix=/tmp/build-1/x` | any | P: names `/tmp/build-1/x` |
| `lib/libx.la` with `libdir='D:/a/x/lib'` | any | P: names `D:/a/x/lib` |
