// What xlings gives Luban (design 2026-10-20 WuXian §16.2): the contracts
// a Luban test cannot check without importing xlings' frontend, which
// luban/ never does -- an ISO written in-process the way a machine's image
// needs it, and luban released as the xlings it drives.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.image;
import xlings.core.xim.extract;
import xlings.core.config;
import luban.cli;

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
bool have(const char* tool) {
    for (const auto* dir : {"/usr/sbin/", "/sbin/", "/usr/bin/", "/bin/"})
        if (fs::exists(std::string(dir) + tool)) return true;
    return false;
}
}  // namespace

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

XTEST(LubanCli, LubanIsReleasedAsTheXlingsItDrives, .area = "luban", .covers = {"LUBAN-TOOL-SHIPPED"}) {
    EXPECT_EQ(luban::cli::kVersion, xlings::Info::VERSION)
        << "bump luban/os/cli/src/cli.cppm kVersion with mcpp.toml and src/core/config.cppm";
}
