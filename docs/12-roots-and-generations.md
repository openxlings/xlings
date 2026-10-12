# Roots and Generations

**English** | [简体中文](zh/12-roots-and-generations.md)

A SubOS can be presented **as `/`**: its `/usr` is a view of package payloads,
and its `/etc`, `/home` and `/var` are the machine's own state. The same
declaration can be entered as a sandbox, exported as a container image or a disk,
or booted by a kernel. This is what [Luban](luban/README.md) builds on; this
chapter is the xlings side.

## A root

```bash
xlings subos new box --rootfs --from subos:luban-core   # a root; its packages installed into it
xlings subos use box                                    # enter: uid 0, its own /usr and /etc
xlings subos exec box -- sh -c 'gcc hello.c && ./a.out'
xlings subos exec box -- xlings install -y nginx        # installed inside: in its /usr/bin at once
xlings subos status box                                 # the generation, the history, name conflicts
```

| Part | What |
|---|---|
| `/usr` | a generation: a tree of links into payloads — the registered programs and libraries first, then each active package's `bin/` and `lib/` (busybox's applets land in `/usr/bin` this way) |
| `/etc` | filled where missing from the packages' factory files (`usr/share/factory/etc`); system users only added |
| `/home`, `/var`, `/root` | the machine's; no generation changes them |

## Generations

Every change to a root's workspace (`install`, `use`, `remove`) is a new
generation: `<subos>/root` points at `root.gen/<k>`, and a switch is one rename.
A placed generation is never edited.

```bash
xlings subos rollback box --list    # the generations
xlings subos rollback box           # back one: only the pointer moves
xlings subos rollback box --to 3
```

A broken glibc does not prevent a rollback: xlings is static.

## Exporting

```bash
xlings subos export box --tar box.tar.gz        # docker import / podman import / wsl --import
xlings subos export box --rootfs ./box/         # a directory
xlings subos export box --disk box.img --size 8G   # an ext4 filesystem image
luban export box box-drive.img --size 8G        # a bootable drive: GPT, UEFI (and BIOS on x86_64)
luban export box box.qcow2 --size 8G            # the same drive as qcow2
luban export box box.iso                        # a live ISO: the root in memory
```

The images a machine boots are Luban's (`luban export`); `xlings subos export`
writes the root as a directory, a tarball or a filesystem image. An exported image is self-contained: the instance becomes the image's default
SubOS, with every payload of its closure (workspace, installed packages, each ELF's
loader and search path), a static xlings, and the boot stage. Images are written
in-process — GPT, FAT, ISO 9660 with El Torito entries for BIOS and UEFI, the
hybrid partition table that lets the same ISO boot from a drive, qcow2 — so the
machine making them needs no partitioning, ISO or qemu-img tool. What boots it
(the kernel, limine, the command line) is a boot profile; see
[Luban: images](luban/05-images.md).

The output must not exist; a reused target is refused and left alone.

## Booting

A kernel started with `init=<home>/boot/luban-init` (`xlings-init` is its older
name) runs stage-0: it mounts the kernel filesystems, picks a SubOS from `boot.json`, points
`/usr` at it and executes its init; an init that does not come up falls back to
the next.

```bash
luban boot box --once       # try box on the next boot; back to the default unless confirmed
luban boot box --mark-good  # confirm it
luban boot box              # make it the default
luban boot box --now        # switch userland without restarting the kernel (when the init supports it)
```

## Domains: where the home is inside the image

On Linux an ordinary user can name the home path an image will have (`--domain`):
xlings builds in a private namespace the user owns, installs the packages at that
logical path, and keeps the data in the user's home.

```bash
xlings subos new mydistro --rootfs --domain /xlings --from subos:luban-tiny
xlings subos exec mydistro -- sh
xlings subos export mydistro --tar mydistro.tar
```

This needs a working bwrap and user namespaces.

## A distribution of your own

```bash
xlings subos new mydistro --rootfs --from subos:luban-core
xlings subos exec mydistro -- xlings install -y nginx
xlings subos pack mydistro --as myns:mydistro@1.0      # a SubOS package for your index
xlings subos diff box mydistro                         # what differs
```

[32](32-authoring-a-subos-template.md) and [Luban: editions](luban/02-editions.md)
cover writing one as a template.

## The system layer

On a multi-user machine an administrator can install packages once for everyone;
each user's own versions come first:

```bash
sudo xlings install --system gcc cmake      # into /xlings, owned by root
cmake --version                              # a user: after their own packages, before the host's
```
