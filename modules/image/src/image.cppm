// Disk and disc images, written in-process (Luban design §A9; the aarch64
// design §4): a GPT disk and the FAT system partition a firmware and limine
// read; an ISO 9660 image made to boot on UEFI from a disc and from a drive
// (El Torito EFI entry, hybrid GPT); a qcow2 of a raw disk. No partitioning,
// FAT, ISO or qemu-img tool is needed on the machine that makes them.
export module xlings.image;

import std;

export namespace xlings::image {

// The machine an image is for -- the root's own architecture. What differs
// between machines is here and nowhere else.
struct Arch {
    std::string_view name;              // "x86_64", "aarch64"
    std::string_view efi_loader;        // limine's UEFI loader under EFI/BOOT/, the name a firmware looks for
    bool bios;                          // the machine has a BIOS: limine-bios-*, `limine bios-install`
    std::string_view qemu;              // its system emulator (a tool of xlings.subos.tools)
    std::string_view machine;           // qemu -M ("" = qemu's default: a PC on x86_64)
    std::string_view cpu;               // qemu -cpu without KVM ("" = qemu's default)
    std::string_view firmware;          // the UEFI firmware (xim:edk2-firmware share/edk2/)
    bool firmware_required;             // no firmware, no boot (no BIOS to fall back on)
    bool ide_cdrom;                     // the machine has an IDE CD (-cdrom); else virtio-scsi
    std::string_view triple;            // a static build's target
};
const Arch* arch(std::string_view name);
// This build's own architecture (what a root made here is).
const Arch& native_arch();

using Guid = std::array<std::uint8_t, 16>;   // as stored on disk (mixed-endian)

Guid guid_from_string(std::string_view canonical);   // "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
std::string to_string(const Guid& g);                // the canonical, lowercase form
Guid random_guid();                                  // version 4

inline const Guid kEspType = guid_from_string("c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
inline const Guid kLinuxType = guid_from_string("0fc63daf-8483-4772-8e79-3d69d8477de4");
// Where a BIOS loader keeps its second stage on a GPT disk (limine bios-install).
inline const Guid kBiosBootType = guid_from_string("21686148-6449-6e6f-744e-656564454649");

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0);

// A file of a FAT image: its path inside ("EFI/BOOT/BOOTX64.EFI"), and its
// bytes -- from a file of this machine, or given.
struct File {
    std::string path;
    std::filesystem::path from;
    std::string content;
};

// A FAT16 filesystem of `bytes` (8 MiB .. 2 GiB) holding `files` -- long names
// as VFAT entries -- written to `out`. `hidden` is the partition's first
// sector on its disk (the BPB records it).
std::expected<void, std::string> write_fat(const std::filesystem::path& out, std::uint64_t bytes,
                                           std::span<const File> files, std::uint32_t hidden,
                                           std::string_view label);

struct Partition {
    std::string name;            // up to 36 characters
    Guid type;
    Guid uuid;                   // PARTUUID
    std::filesystem::path content;   // a filesystem image, copied in; empty: left zeroed
    std::uint64_t bytes;         // its size on the disk (>= the content's)
};

// Where each partition will start (in 512-byte sectors), laid out from 1 MiB,
// each at a 1 MiB boundary -- known before the contents are made, for a
// FAT's `hidden` and a kernel's root=PARTUUID.
std::vector<std::uint64_t> layout(std::span<const std::uint64_t> sizes);

// A GPT disk holding `parts` in order, with a protective MBR and the backup
// table at its end.
std::expected<void, std::string> write_gpt_disk(const std::filesystem::path& out, std::span<const Partition> parts);

// ── ISO 9660 ───────────────────────────────────────────────────────────

// Where a file of an ISO 9660 image is: its first 2048-byte block and its
// length (found through the Joliet tree, by its full name).
struct IsoExtent {
    std::uint64_t lba;
    std::uint64_t bytes;
};
std::expected<IsoExtent, std::string> iso_find(const std::filesystem::path& iso, std::string_view path);

// Make an ISO 9660 image (one El Torito default entry already in it) boot on
// UEFI, from a disc and from a drive: `efi` is the FAT image a firmware
// boots (limine-uefi-cd.bin). With `bios`, the default entry stays the BIOS
// one and a section for platform 0xEF is added; without (a machine with no
// BIOS), the default entry is made the EFI one. Then the system area gets a
// protective MBR and a GPT whose EFI system partition is that FAT image, the
// rest of the image as basic-data partitions, the backup table appended --
// what `xorriso -efi-boot-part --efi-boot-image --protective-msdos-label`
// writes, so a drive the image is written to boots it on UEFI, and
// `limine bios-install` still makes it boot on BIOS.
std::expected<void, std::string> make_iso_hybrid(const std::filesystem::path& iso, const IsoExtent& efi, bool bios);

// What El Torito boot entries an image has: (platform, LBA, 512-byte sector
// count) for the default entry and each section entry. For tests and
// `luban` reporting.
struct BootEntry {
    std::uint8_t platform;   // 0x00 BIOS, 0xEF UEFI
    std::uint32_t lba;
    std::uint16_t sectors;
};
std::expected<std::vector<BootEntry>, std::string> iso_boot_entries(const std::filesystem::path& iso);

// The partitions of a GPT disk (or hybrid image), read back and checked:
// header and table CRCs, the backup header and table at the end.
struct GptPartition {
    Guid type;
    std::uint64_t first;   // 512-byte sectors
    std::uint64_t last;
    std::string name;
};
std::expected<std::vector<GptPartition>, std::string> read_gpt(const std::filesystem::path& disk);

// ── qcow2 ──────────────────────────────────────────────────────────────

// A raw disk as a qcow2 (version 3, 64 KiB clusters, no backing file): a
// cluster of zeros is not stored. What `qemu-img convert -O qcow2` makes.
std::expected<void, std::string> write_qcow2(const std::filesystem::path& raw, const std::filesystem::path& out);

}  // namespace xlings::image
