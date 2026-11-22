# The store's references and roots

> Status: in force since xlings 2026.11.23.1. Implemented in the module
> `xlings.store` of the `modules/home` package (the link walk, the roots) and
> `src/core/profile.cpp` (`collect_subos_references_`, `gc`).

`xlings self clean` deletes the payloads of a home's store
(`<home>/data/xpkgs/<store-name>/<version>`) that nothing references. This
specification states what counts as a reference, and the format of a **root**:
the way a program other than xlings states that it uses a payload. A program
that wires store paths into its own output (mcpp writes payload paths into its
builds) registers a root for each payload it uses, and the collection keeps
those payloads. The collection has one implementation, xlings'; when a root
goes is the owner's decision.

## 1. The reference set

A payload is referenced when it is in the union of four parts:

| Part | What it reads |
|---|---|
| 1. Registrations | every SubOS's workspace (`active` and `installed`), each target mapped through the versions database to the payloads its versions name |
| 2. Views | every link under a SubOS's `usr`, `etc`, `share`, `lib`, `lib64` and `bin`; the record of every root generation (`root.gen/<k>`); the environment's runtime links (`<home>/abi.gen`). A link's payload is the `xpkgs/<store-name>/<version>` its target lies in |
| 3. Dependencies | for every payload of parts 1, 2 and 4, the `install_dir` of each dependency its install record (`.xlings-resolution.json`) states, and so on to the closure |
| 4. Roots | every root under `<home>/data/roots/` (§2) |

**Could not read is not "references nothing".** When any part cannot be read --
a SubOS's state file, the versions database, a directory of a view, a root
generation's record, a roots directory or a root, a payload's install record --
the collection is refused, says which part and why, and deletes nothing. A
payload without an install record (installed before records existed) states no
dependencies; that is not an error.

A dangling link in a view references nothing and is left to `self doctor`.

**Layers.** Each home reads its own registrations, views and roots. In a
multi-user system (Luban), a user's home does not delete a payload of the system
layer (`/xlings`); a user's root may name a system-layer payload, which the
user's collection ignores and the system layer's ownership rules protect. A
store shared with read-only borrowers is not collected.

## 2. Roots

```
<home>/data/roots/<owner>/<store-name>@<version>  ->  <home>/data/xpkgs/<store-name>/<version>
```

| Part | Rule |
|---|---|
| Owner | a directory under `<home>/data/roots`, named for the program that holds the roots: lowercase letters, digits, `-`, `_` and `.` (`mcpp`). One owner's roots are one directory; no other program writes into it |
| Root | a link in the owner's directory: a symbolic link, or on Windows a symbolic link or a directory junction. A regular file or a directory there is not a root; the collection names it and ignores it |
| Name | `<store-name>@<version>`, the payload's two path components (`xim-x-gcc@16.1.0`). The name is the owner's key, so that registering the same payload twice is one root |
| Target | the payload directory itself, `<home>/data/xpkgs/<store-name>/<version>`: an absolute path, or a path relative to the owner's directory (`../../xpkgs/xim-x-gcc/16.1.0`). Relative is recommended: it survives a move of the home. The collection reads the target, not the name: a link to a file inside a payload, or to something outside a store, is not a root |
| Last use | the modification time of the link itself (`lstat`), set by the owner when it uses the payload (`touch -h`, `utimensat(..., AT_SYMLINK_NOFOLLOW)`, or replacing the link). xlings does not read it; the owner's policy decides when an unused root is removed |
| Dangling | a root whose target does not exist holds nothing. The collection removes it (a dry run lists it) |

**Registering, using and dropping a root needs no lock and no xlings process.**
The owner creates the link (atomically: create it under a temporary name in the
same directory, then rename it over the root), touches it, and deletes it, with
the file system's own operations. A collection that runs meanwhile reads each
root once: a root created before the collection read the directory keeps its
payload; a payload whose root is created after it may be deleted, so an owner
registers the root before it starts to use a payload and checks that the payload
is still there afterwards.

A home written before 2026.11.23.1 has no roots; its collection behaves as
before for every payload no root names. An owner that starts registering roots
on such a home registers one for every payload it uses before its next
collection.

## 3. The command

```bash
xlings self clean --dry-run   # what would be removed, and the dangling roots
xlings self clean             # remove it
```

A refusal prints `gc refused: <which part> could not be read ...` and exits 1.
