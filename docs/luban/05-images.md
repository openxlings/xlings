# Images

**English** | [简体中文](../zh/luban/05-images.md)

An environment becomes an image with `luban export`; the format follows the
file name.

## Formats

```bash
luban export box box.iso              # a live ISO: the root runs in memory
luban export box box.img              # a drive: GPT, an EFI system partition, an ext4 root, persistent
luban export box box.qcow2            # the same drive as qcow2
luban export box box.tar.zst          # a root filesystem (docker import, wsl --import)
luban export box ./box-root/          # a directory
```

| | boots on | written by |
|---|---|---|
| `.iso` | BIOS and UEFI on x86_64, UEFI on aarch64; from a CD and from a drive it is written to (a hybrid partition table) | in-process: ISO 9660, El Torito, GPT |
| `.img` | BIOS and UEFI on x86_64, UEFI on aarch64 | in-process: GPT, FAT; `mkfs.ext4` (xim:e2fsprogs) for the root |
| `.qcow2` | the same | in-process |
| `.tar.*` | — (a container or WSL imports it) | in-process |

No partitioning, ISO, FAT or qemu-img tool is needed on the machine making an
image; `limine` (xim:limine) is the boot loader, and on x86_64 makes the image
boot on BIOS from a drive.

## Boot profiles

How an image boots — its kernel, the boot loader and the kernel's command line —
is a **boot profile**, a package installed into the environment only to make an
image:

| `--boot` | for | kernel |
|---|---|---|
| `generic` | machines (x86_64) | `linux-kernel`, with the drivers a machine needs |
| `virt` | virtual machines (x86_64, aarch64) | `linux-kernel-virt` (6.12 LTS): virtio, ext4 and the console built in |
| `ns:name` | a profile of your own | whatever it names |

```bash
luban export box box.iso --boot virt
```

Without `--boot`, the edition's profile is used. Without any profile, the
kernel already in the environment (`xlings install linux-kernel --subos box`),
or `--kernel <vmlinuz>` with `xlings subos export`.

## Trying an image

```bash
luban try box                       # export and boot in a local VM
luban try box.iso                   # an image you already have
luban try box --proxy socks5h://127.0.0.1:7897   # its only network is the proxy
luban try box --nographic           # its console in this terminal
```

`luban try` boots the `virt` profile by default (falling back to the
environment's own kernel), with KVM when this user may use it and emulated
otherwise. On aarch64 it boots UEFI firmware on qemu's `virt` machine. What it
needs comes from the index — `xlings install qemu-system`, one package with
both emulators, user networking and the UEFI firmware; luban names it when it is
missing.

## Writing a drive

```bash
luban write box.iso /dev/sdb        # a boot stick from an image
luban write box /dev/sdb            # an environment straight onto a drive
```

`luban write` writes whole drives only, and refuses a partition, a mounted
drive, or one in use (including the one the system runs from). It asks you to
type the drive's name; in agent mode it needs `-y` and `--serial <serial>`.
