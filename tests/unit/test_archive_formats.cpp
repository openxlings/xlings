// The archives an image is written as (Luban design §A9): an initramfs the
// kernel can unpack, and a CD image a BIOS can boot, both in-process.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xim.extract;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xim = xlings::xim;

namespace {
std::string bytes(const fs::path& file, std::size_t offset, std::size_t n) {
    std::ifstream in(file, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::string out(n, '\0');
    in.read(out.data(), static_cast<std::streamsize>(n));
    return out;
}
}  // namespace

// Images are made on Linux (`subos export` refuses elsewhere); on Windows
// libarchive's ISO writer looks the boot file up by a native path.
XTEST(ArchiveFormats, ACdImageCarriesAnElToritoRecordForItsBootFile,
      .area = "luban", .covers = {"LUBAN-ISO"}, .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("iso-write");
    auto tree = home.root() / "tree";
    if (const char* from = std::getenv("XTEST_ISO_FROM"); from && *from) tree = from;   // a real one, by hand
    else {
        tk::write_file(tree / "boot/limine/limine-bios-cd.bin", std::string(2048, 'B'));
        tk::write_file(tree / "boot/vmlinuz", "kernel");
    }
    const auto iso = home.root() / "out.iso";
    auto written = xim::write_archive(tree, iso, xim::ArchiveFormat::Iso9660,
                                      xim::ArchiveOptions{.boot = "boot/limine/limine-bios-cd.bin"});
    ASSERT_TRUE(written) << written.error();
    if (const char* keep = std::getenv("XTEST_ISO_KEEP"); keep && *keep) fs::copy_file(iso, keep, fs::copy_options::overwrite_existing);
    // Sector 16: the primary volume descriptor; 17: the boot record.
    EXPECT_EQ(bytes(iso, 16 * 2048 + 1, 5), "CD001");
    EXPECT_EQ(bytes(iso, 17 * 2048, 1), std::string(1, '\0')) << "volume descriptor type 0: a boot record";
    EXPECT_EQ(bytes(iso, 17 * 2048 + 7, 23), "EL TORITO SPECIFICATION");
}

XTEST(ArchiveFormats, AnInitramfsIsNewcCpioWithAConsole, .area = "luban", .covers = {"LUBAN-ISO"},
      .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("cpio-write");
    const auto tree = home.root() / "tree";
    tk::write_file(tree / "sbin/init", "#!/bin/sh\n");
    const auto out = home.root() / "initramfs.img";
    auto written = xim::write_archive(tree, out, xim::ArchiveFormat::CpioNewcGz);
    ASSERT_TRUE(written) << written.error();
    // newc ("070701"), names relative with no "./", and a /dev/console --
    // read back with the machine's gzip, as the kernel would see it.
    auto listed = tk::run({.argv = {"/bin/sh", "-c", "gzip -dc '" + out.string() + "' | strings | head -40"},
                           .env = {{"PATH", "/usr/bin:/bin"}}});
    ASSERT_EQ(listed.exit_code, 0) << listed.transcript();
    EXPECT_TRUE(listed.out.starts_with("070701")) << listed.out;
    EXPECT_NE(listed.out.find("sbin/init"), std::string::npos) << listed.out;
    EXPECT_NE(listed.out.find("dev/console"), std::string::npos) << listed.out;
    EXPECT_EQ(listed.out.find("./sbin"), std::string::npos) << listed.out;
    EXPECT_EQ(bytes(out, 0, 2), std::string("\x1f\x8b", 2)) << "gzip, as the kernel unpacks";
}
