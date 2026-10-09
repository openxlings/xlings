// A machine's drive image, written in-process (Luban design §A9): a GPT
// disk, and the FAT system partition a firmware and limine read. No
// partitioning or FAT tool is needed on the machine that makes it.
export module luban.image;

import std;

export namespace luban::image {

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

}  // namespace luban::image
