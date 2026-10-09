// A machine's drive image, written in-process (Luban design §A9): the GPT a
// firmware reads, and the FAT a loader reads. Read back here byte by byte,
// and by the machine's own tools where it has them.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import luban.image;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace image = luban::image;

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
