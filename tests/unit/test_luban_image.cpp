// A machine's drive image, written in-process (Luban design §A9): the GPT a
// firmware reads, and the FAT a loader reads. Read back here byte by byte,
// and by the machine's own tools where it has them.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.image;
import xlings.core.xim.extract;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace image = xlings::image;

namespace {
std::string read_at(const fs::path& f, std::uint64_t offset, std::size_t n) {
    std::ifstream in(f, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::string s(n, '\0');
    in.read(s.data(), static_cast<std::streamsize>(n));
    return s;
}
std::uint64_t le(std::string_view b) {
    std::uint64_t v = 0;
    for (std::size_t i = b.size(); i-- > 0;) v = (v << 8) | static_cast<std::uint8_t>(b[i]);
    return v;
}
bool have(const char* tool) {
    for (const auto* dir : {"/usr/sbin/", "/sbin/", "/usr/bin/", "/bin/"})
        if (fs::exists(std::string(dir) + tool)) return true;
    return false;
}
}  // namespace

XTEST(LubanImage, AGuidReadsBackAsItWasWritten, .area = "luban", .covers = {"LUBAN-DRIVE"}) {
    EXPECT_EQ(image::to_string(image::kEspType), "c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
    // On disk the first three fields are little-endian.
    EXPECT_EQ(image::kEspType[0], 0x28);
    EXPECT_EQ(image::kEspType[3], 0xc1);
    const auto g = image::random_guid();
    EXPECT_EQ(image::guid_from_string(image::to_string(g)), g);
    EXPECT_EQ(image::to_string(g)[14], '4') << "version 4";
    EXPECT_EQ(image::crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("123456789"), 9)),
              0xCBF43926u);
}

XTEST(LubanImage, AGptDriveWithAFatSystemPartitionReadsBack, .area = "luban", .covers = {"LUBAN-DRIVE"}) {
    auto home = tk::Home::isolated("luban-image");
    const auto esp = home.root() / "esp.fat";
    const auto root = home.root() / "root.ext4";
    tk::write_file(root, std::string(4096, 'R'));
    const std::array<std::uint64_t, 3> sizes{1ull << 20, 32ull << 20, 64ull << 20};
    const auto starts = image::layout(sizes);
    ASSERT_EQ(starts, (std::vector<std::uint64_t>{2048, 4096, 69632}));
    const std::string big(300000, 'K');
    std::vector<image::File> files{{.path = "EFI/BOOT/BOOTX64.EFI", .content = "efi"},
                                   {.path = "boot/vmlinuz", .content = big},
                                   {.path = "boot/limine/limine.conf", .content = "timeout: 3\n"},
                                   {.path = "boot/limine/limine-bios.sys", .content = "stage2"}};
    auto fat = image::write_fat(esp, 32ull << 20, files, static_cast<std::uint32_t>(starts[1]), "LUBANESP");
    ASSERT_TRUE(fat) << fat.error();
    EXPECT_EQ(read_at(esp, 54, 8), "FAT16   ");
    EXPECT_EQ(read_at(esp, 510, 2), "\x55\xAA");
    EXPECT_EQ(le(read_at(esp, 28, 4)), starts[1]) << "the BPB records where its partition starts";
    if (have("fsck.fat")) {
        auto checked = tk::run({.argv = {"/bin/sh", "-c", "fsck.fat -n '" + esp.string() + "'"},
                                .env = {{"PATH", "/usr/sbin:/sbin:/usr/bin:/bin"}}});
        EXPECT_EQ(checked.exit_code, 0) << checked.transcript();
    }
    const auto uuid = image::random_guid();
    const auto disk = home.root() / "drive.img";
    std::array<image::Partition, 3> parts{
        image::Partition{.name = "BIOS boot", .type = image::kBiosBootType, .uuid = image::random_guid(), .bytes = sizes[0]},
        image::Partition{.name = "EFI system", .type = image::kEspType, .uuid = image::random_guid(), .content = esp, .bytes = sizes[1]},
        image::Partition{.name = "luban", .type = image::kLinuxType, .uuid = uuid, .content = root, .bytes = sizes[2]}};
    auto written = image::write_gpt_disk(disk, parts);
    ASSERT_TRUE(written) << written.error();
    const auto total = fs::file_size(disk) / 512;
    EXPECT_EQ(static_cast<std::uint8_t>(read_at(disk, 446 + 4, 1)[0]), 0xEE) << "a protective MBR";
    EXPECT_EQ(read_at(disk, 512, 8), "EFI PART");
    EXPECT_EQ(read_at(disk, (total - 1) * 512, 8), "EFI PART") << "the backup header, last";
    // The header's CRC over its 92 bytes with the field zeroed.
    auto header = read_at(disk, 512, 92);
    const auto stored = static_cast<std::uint32_t>(le(header.substr(16, 4)));
    std::fill(header.begin() + 16, header.begin() + 20, '\0');
    EXPECT_EQ(image::crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(header.data()), 92)), stored);
    const auto entries = read_at(disk, 1024, 128 * 128);
    EXPECT_EQ(image::crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(entries.data()), entries.size())),
              static_cast<std::uint32_t>(le(read_at(disk, 512 + 88, 4))));
    EXPECT_EQ(le(entries.substr(2 * 128 + 32, 8)), starts[2]) << "the root where layout() said";
    EXPECT_EQ(entries.substr(2 * 128 + 16, 16), std::string(reinterpret_cast<const char*>(uuid.data()), 16));
    EXPECT_EQ(read_at(disk, starts[1] * 512 + 54, 8), "FAT16   ") << "the system partition's bytes, in place";
    EXPECT_EQ(read_at(disk, starts[2] * 512, 4), "RRRR");
    if (have("sfdisk")) {
        auto listed = tk::run({.argv = {"/bin/sh", "-c", "sfdisk -d '" + disk.string() + "'"},
                               .env = {{"PATH", "/usr/sbin:/sbin:/usr/bin:/bin"}}});
        EXPECT_EQ(listed.exit_code, 0) << listed.transcript();
        EXPECT_NE(listed.out.find("uuid=" + [&] { auto s = image::to_string(uuid); for (auto& c : s) c = static_cast<char>(std::toupper(c)); return s; }()),
                  std::string::npos) << listed.out;
    }
}

// What differs between machines is one table (the aarch64 design §4.1).
XTEST(LubanImage, TheMachineTableSaysWhatDiffersBetweenMachines, .area = "luban", .covers = {"LUBAN-DRIVE", "LUBAN-TRY"}) {
    const auto* pc = image::arch("x86_64");
    const auto* arm = image::arch("aarch64");
    ASSERT_TRUE(pc && arm);
    EXPECT_EQ(pc->efi_loader, "BOOTX64.EFI");
    EXPECT_TRUE(pc->bios);
    EXPECT_FALSE(pc->firmware_required) << "a PC boots on its BIOS without one";
    EXPECT_TRUE(pc->ide_cdrom);
    EXPECT_EQ(arm->efi_loader, "BOOTAA64.EFI");
    EXPECT_FALSE(arm->bios);
    EXPECT_TRUE(arm->firmware_required) << "virt has nothing to start without UEFI firmware";
    EXPECT_EQ(arm->machine, "virt");
    EXPECT_FALSE(arm->ide_cdrom) << "virt has no IDE: a CD is SCSI";
    EXPECT_EQ(arm->qemu, "qemu-system-aarch64");
    EXPECT_EQ(image::arch("riscv64"), nullptr);
    EXPECT_FALSE(image::native_arch().name.empty());
}

namespace {

std::string pattern(std::size_t n, char seed) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(seed + (i * 7 + i / 512) % 61);
    return s;
}

// An ISO of a limine-shaped tree, the default entry `boot`.
fs::path iso_of(tk::Home& home, std::string_view boot, bool info_table) {
    const auto tree = home.root() / "tree";
    tk::write_file(tree / "boot/limine/limine-bios-cd.bin", pattern(2048 * 3, 'B'));
    tk::write_file(tree / "boot/limine/limine-uefi-cd.bin", pattern(1474560, 'E'));
    tk::write_file(tree / "boot/limine/limine.conf", "timeout: 3\n");
    tk::write_file(tree / "boot/vmlinuz", pattern(700000, 'K'));
    const auto iso = home.root() / "t.iso";
    auto written = xlings::xim::write_archive(tree, iso, xlings::xim::ArchiveFormat::Iso9660,
                                              xlings::xim::ArchiveOptions{.boot = std::string(boot), .boot_info_table = info_table});
    EXPECT_TRUE(written) << written.error();
    return iso;
}

}  // namespace

// The ISO boots on UEFI from a disc and from a drive, written in-process
// (no xorriso): an El Torito EFI entry, and the hybrid GPT whose EFI system
// partition is that same FAT image -- the layout xorriso's
// -efi-boot-part --efi-boot-image --protective-msdos-label writes.
XTEST(LubanImage, AnIsoGetsAUefiEntryAndAHybridGptBesideItsBiosOne, .area = "luban",
      .covers = {"LUBAN-ISO-UEFI", "LUBAN-ISO-HYBRID"}) {
    auto home = tk::Home::isolated("luban-iso-bios");
    const auto iso = iso_of(home, "boot/limine/limine-bios-cd.bin", true);
    const auto before = fs::file_size(iso);
    auto efi = image::iso_find(iso, "boot/limine/limine-uefi-cd.bin");
    ASSERT_TRUE(efi) << efi.error();
    EXPECT_EQ(efi->bytes, 1474560u);
    EXPECT_EQ(read_at(iso, efi->lba * 2048, 4096), pattern(4096, 'E')) << "the extent the Joliet tree names";
    EXPECT_FALSE(image::iso_find(iso, "boot/limine/nothing.bin"));
    EXPECT_FALSE(image::iso_find(iso, "boot/limine")) << "a directory is not a file";

    auto hybrid = image::make_iso_hybrid(iso, *efi, /*bios=*/true);
    ASSERT_TRUE(hybrid) << hybrid.error();
    auto entries = image::iso_boot_entries(iso);
    ASSERT_TRUE(entries) << entries.error();
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_EQ((*entries)[0].platform, 0x00) << "the default entry stays BIOS";
    EXPECT_EQ((*entries)[0].sectors, 4);
    EXPECT_EQ((*entries)[1].platform, 0xEF);
    EXPECT_EQ((*entries)[1].lba, efi->lba);
    EXPECT_EQ((*entries)[1].sectors, 1474560 / 512);

    EXPECT_EQ(fs::file_size(iso), before + 64 * 512) << "room for the backup table";
    auto parts = image::read_gpt(iso);
    ASSERT_TRUE(parts) << parts.error();
    const auto esp = std::ranges::find_if(*parts, [](const auto& p) { return p.type == image::kEspType; });
    ASSERT_NE(esp, parts->end());
    EXPECT_EQ(esp->first, efi->lba * 4) << "the EFI system partition is the El Torito image";
    EXPECT_EQ(esp->last, efi->lba * 4 + 1474560 / 512 - 1);
    for (const auto& p : *parts) {
        EXPECT_GE(p.first, 64u);
        EXPECT_LT(p.last, before / 512) << p.name << ": inside the image's own data";
    }
    EXPECT_EQ(read_at(iso, 32768 + 1, 5), "CD001") << "still an ISO 9660 image";
    if (have("sfdisk")) {
        auto listed = tk::run({.argv = {"/bin/sh", "-c", "sfdisk -d '" + iso.string() + "'"},
                               .env = {{"PATH", "/usr/sbin:/sbin:/usr/bin:/bin"}}});
        EXPECT_EQ(listed.exit_code, 0) << listed.transcript();
        EXPECT_NE(listed.out.find("type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B"), std::string::npos) << listed.out;
    }
}

XTEST(LubanImage, AnIsoForAMachineWithNoBiosBootsOnlyItsEfiImage, .area = "luban",
      .covers = {"LUBAN-ISO-UEFI", "LUBAN-ISO-HYBRID"}) {
    auto home = tk::Home::isolated("luban-iso-efi");
    const auto iso = iso_of(home, "boot/limine/limine-uefi-cd.bin", false);
    auto efi = image::iso_find(iso, "boot/limine/limine-uefi-cd.bin");
    ASSERT_TRUE(efi) << efi.error();
    EXPECT_EQ(read_at(iso, efi->lba * 2048, 64), pattern(64, 'E')) << "no boot-info table patched into a FAT image";
    auto hybrid = image::make_iso_hybrid(iso, *efi, /*bios=*/false);
    ASSERT_TRUE(hybrid) << hybrid.error();
    auto entries = image::iso_boot_entries(iso);
    ASSERT_TRUE(entries) << entries.error() << " (the validation checksum is recomputed)";
    ASSERT_EQ(entries->size(), 1u);
    EXPECT_EQ((*entries)[0].platform, 0xEF);
    EXPECT_EQ((*entries)[0].lba, efi->lba);
    EXPECT_EQ((*entries)[0].sectors, 1474560 / 512);
    EXPECT_TRUE(image::read_gpt(iso));
}

namespace {

std::uint64_t be(std::string_view b) {
    std::uint64_t v = 0;
    for (auto c : b) v = (v << 8) | static_cast<std::uint8_t>(c);
    return v;
}

// An independent qcow2 v3 reader: the virtual disk's bytes at `offset`.
std::string qcow2_read(const fs::path& q, std::uint64_t offset, std::size_t n) {
    const auto header = read_at(q, 0, 104);
    const auto cluster = 1ull << be(header.substr(20, 4));
    const auto l1_at = be(header.substr(40, 8));
    std::string out;
    while (n > 0) {
        const auto c = offset / cluster, in = offset % cluster;
        const auto take = std::min<std::uint64_t>(n, cluster - in);
        const auto per_l2 = cluster / 8;
        const auto l2 = be(read_at(q, l1_at + (c / per_l2) * 8, 8)) & 0x00FFFFFFFFFFFE00ull;
        std::uint64_t data = 0;
        if (l2) data = be(read_at(q, l2 + (c % per_l2) * 8, 8)) & 0x00FFFFFFFFFFFE00ull;
        out += data ? read_at(q, data + in, take) : std::string(take, '\0');
        offset += take;
        n -= take;
    }
    return out;
}

}  // namespace

XTEST(LubanImage, AQcow2ReadsBackAsTheRawDiskItWasMadeFrom, .area = "luban", .covers = {"LUBAN-QCOW2"}) {
    auto home = tk::Home::isolated("luban-qcow2");
    const auto raw = home.root() / "disk.raw";
    // 600 MiB, sparse, data in three places -- one past the first L2 table's reach (512 MiB).
    {
        std::ofstream o(raw, std::ios::binary);
        o.seekp(600ll << 20);
        o.put('\0');
    }
    const std::vector<std::pair<std::uint64_t, std::string>> data{
        {0, pattern(70000, 'a')}, {5ull << 20, pattern(3, 'b')}, {(550ull << 20) + 12345, pattern(100000, 'c')}};
    {
        std::fstream f(raw, std::ios::binary | std::ios::in | std::ios::out);
        for (const auto& [at, bytes] : data) {
            f.seekp(static_cast<std::streamoff>(at));
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
    }
    const auto q = home.root() / "disk.qcow2";
    auto written = image::write_qcow2(raw, q);
    ASSERT_TRUE(written) << written.error();
    EXPECT_EQ(read_at(q, 0, 4), "QFI\xfb");
    EXPECT_EQ(be(read_at(q, 4, 4)), 3u);
    EXPECT_EQ(be(read_at(q, 24, 8)), fs::file_size(raw)) << "the virtual size";
    EXPECT_LT(fs::file_size(q), 2ull << 20) << "zeros are not stored";
    for (const auto& [at, bytes] : data) EXPECT_EQ(qcow2_read(q, at, bytes.size()), bytes) << at;
    EXPECT_EQ(qcow2_read(q, 100ull << 20, 4096), std::string(4096, '\0'));
    EXPECT_EQ(qcow2_read(q, 600ull << 20, 1), std::string(1, '\0'));
    if (have("qemu-img")) {
        auto checked = tk::run({.argv = {"/bin/sh", "-c", "qemu-img check '" + q.string() + "'"},
                                .env = {{"PATH", "/usr/local/bin:/usr/bin:/bin"}}});
        EXPECT_EQ(checked.exit_code, 0) << checked.transcript();
    }
}
