// A symbolic link to a directory is a directory link, whatever the order of the
// archive's entries.
//
// A tar entry does not say whether a link names a directory, and Windows fixes
// that when the link is made: `usr/include/machine -> amd64` of a NetBSD sysroot
// was extracted as a file link, and no header under it could be opened. The
// archive is written here byte by byte, so the fixture exists on every host and
// holds the link BEFORE the directory it names. On a system with one kind of
// link these pass as they always did; the Windows lane is where they prove
// something.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xim.extract;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;

namespace {

std::string ustar_entry(std::string_view name, char type, std::string_view link, std::string_view data) {
    std::string h(512, '\0');
    auto put = [&](std::size_t at, std::string_view v) { std::copy(v.begin(), v.end(), h.begin() + static_cast<std::ptrdiff_t>(at)); };
    put(0, name);
    put(100, type == '5' ? "0000755" : "0000644");
    put(108, "0000000");
    put(116, "0000000");
    put(124, std::format("{:011o}", data.size()));
    put(136, "00000000000");
    put(148, "        ");
    h[156] = type;
    put(157, link);
    put(257, "ustar");
    put(263, "00");
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    put(148, std::format("{:06o}", sum));
    h[154] = '\0';
    h[155] = ' ';
    std::string out = h + std::string(data);
    out.resize(512 + (data.size() + 511) / 512 * 512, '\0');
    return out;
}

}  // namespace

XTEST(ExtractLinks, ALinkToADirectoryIsTraversedWhateverTheOrderOfTheEntries,
      .area = "xim", .covers = {"EXTRACT-DIR-LINKS"}) {
    auto home = tk::Home::isolated("extract-dir-links");
    const auto archive = home.root() / "links-to-directories.tar";
    {
        std::ofstream out(archive, std::ios::binary);
        out << ustar_entry("inc/", '5', "", "")
            << ustar_entry("inc/machine", '2', "amd64", "")       // before what it names
            << ustar_entry("inc/arch", '2', "machine", "")        // a link to that link
            << ustar_entry("inc/amd64/", '5', "", "")
            << ustar_entry("inc/amd64/types.h", '0', "", "typedef int t;\n")
            << ustar_entry("inc/alias.h", '2', "amd64/types.h", "")
            << ustar_entry("inc/nothing", '2', "absent", "")
            << std::string(1024, '\0');
    }
    auto r = xlings::xim::extract_archive_detailed(archive, home.root() / "out");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_TRUE(fs::is_symlink(*r / "inc/machine")) << "the link of the archive is not a link on disk";
    EXPECT_EQ(tk::read_file(*r / "inc/machine/types.h"), "typedef int t;\n");
    EXPECT_EQ(tk::read_file(*r / "inc/arch/types.h"), "typedef int t;\n");
    EXPECT_EQ(tk::read_file(*r / "inc/alias.h"), "typedef int t;\n");
    EXPECT_EQ(fs::read_symlink(*r / "inc/machine"), fs::path("amd64"));
    std::error_code ec;
    EXPECT_FALSE(fs::directory_iterator(*r / "inc/machine", ec) == fs::directory_iterator{});
    EXPECT_FALSE(ec) << ec.message();
    // A link that names nothing stays the link the archive holds.
    EXPECT_TRUE(fs::is_symlink(*r / "inc/nothing"));
}
