// An entry's `strip` (design 2026-11-27 §9.7): the extractor drops the first n
// components of every member's path, a tar and a zip land the same, and a
// member that has no more than n components is an error that names it --
// nothing is dropped without a word. Then the landing of a spec "3" package:
// the download in install_dir, laid out, before install() runs.
//
// The archives are written byte by byte (ustar, and a zip of stored members),
// so the fixtures exist on every host without tar or zip installed.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.xim.extract;
import xlings.core.xim.installer;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace xim = xlings::xim;

namespace {

struct Member {
    std::string path;
    std::string data;
    bool directory { false };
    std::string hardlink;   // tar only: the member this one is a link to
};

std::string ustar_header(const Member& m) {
    std::string h(512, '\0');
    auto put = [&](std::size_t at, std::string_view v) {
        std::copy(v.begin(), v.end(), h.begin() + static_cast<std::ptrdiff_t>(at));
    };
    const char type = m.directory ? '5' : (m.hardlink.empty() ? '0' : '1');
    const auto size = (m.directory || !m.hardlink.empty()) ? std::size_t{0} : m.data.size();
    put(0, m.path);
    put(100, m.directory ? "0000755" : "0000755");
    put(108, "0000000");
    put(116, "0000000");
    put(124, std::format("{:011o}", size));
    put(136, "00000000000");
    put(148, "        ");
    h[156] = type;
    put(157, m.hardlink);
    put(257, "ustar");
    put(263, "00");
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    put(148, std::format("{:06o}", sum));
    h[154] = '\0';
    h[155] = ' ';
    return h;
}

fs::path write_tar(const fs::path& file, const std::vector<Member>& members) {
    std::string out;
    for (const auto& m : members) {
        out += ustar_header(m);
        if (!m.directory && m.hardlink.empty()) {
            out += m.data;
            out.resize(out.size() + (512 - m.data.size() % 512) % 512, '\0');
        }
    }
    out.append(1024, '\0');
    tk::write_file(file, out);
    return file;
}

std::uint32_t crc32(std::string_view data) {
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char c : data) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// A zip of stored (uncompressed) members: local headers, then the central
// directory and its end record.
fs::path write_zip(const fs::path& file, const std::vector<Member>& members) {
    std::string out, central;
    auto u16 = [](std::string& s, unsigned v) { s.push_back(char(v & 0xff)); s.push_back(char((v >> 8) & 0xff)); };
    auto u32 = [&](std::string& s, std::uint32_t v) { u16(s, v & 0xffff); u16(s, v >> 16); };
    for (const auto& m : members) {
        const std::string name = m.directory && !m.path.ends_with('/') ? m.path + "/" : m.path;
        const auto data = m.directory ? std::string{} : m.data;
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto crc = crc32(data);
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0x21);
        u32(out, crc); u32(out, std::uint32_t(data.size())); u32(out, std::uint32_t(data.size()));
        u16(out, unsigned(name.size())); u16(out, 0);
        out += name;
        out += data;
        u32(central, 0x02014b50); u16(central, 0x0314); u16(central, 20); u16(central, 0); u16(central, 0);
        u16(central, 0); u16(central, 0x21);
        u32(central, crc); u32(central, std::uint32_t(data.size())); u32(central, std::uint32_t(data.size()));
        u16(central, unsigned(name.size())); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0);
        u32(central, m.directory ? (040755u << 16) | 0x10 : (0100755u << 16));
        u32(central, offset);
        central += name;
    }
    const auto at = static_cast<std::uint32_t>(out.size());
    out += central;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, unsigned(members.size()));
    u16(out, unsigned(members.size())); u32(out, std::uint32_t(central.size())); u32(out, at); u16(out, 0);
    tk::write_file(file, out);
    return file;
}

// The layout an upstream release has: one top-level directory.
std::vector<Member> release() {
    return {{.path = "probe-1.0.0/", .directory = true},
            {.path = "probe-1.0.0/bin/", .directory = true},
            {.path = "probe-1.0.0/bin/probe", .data = "#!/bin/sh\necho probe\n"},
            {.path = "probe-1.0.0/README", .data = "read me\n"}};
}

std::set<std::string> tree(const fs::path& root) {
    std::set<std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(root))
        out.insert(fs::relative(e.path(), root).generic_string() + (e.is_directory() ? "/" : ""));
    return out;
}

struct Temp {
    fs::path root = fs::weakly_canonical(fs::temp_directory_path()) /
        std::format("xlings-strip-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    Temp() { fs::create_directories(root); }
    ~Temp() { std::error_code ec; fs::remove_all(root, ec); }
};

}  // namespace

XTEST(ExtractStrip, TheLeadingDirectoryIsDroppedAndATarAndAZipLandTheSame,
      .area = "xim", .covers = {"LAND-STRIP"}) {
    Temp t;
    const std::set<std::string> expected{"bin/", "bin/probe", "README"};
    for (const auto& archive : {write_tar(t.root / "a.tar", release()), write_zip(t.root / "a.zip", release())}) {
        xim::ExtractOptions options;
        options.strip = 1;
        const auto dest = t.root / ("out-" + archive.extension().string().substr(1));
        auto r = xim::extract_archive_detailed(archive, dest, options);
        ASSERT_TRUE(r.has_value()) << archive << ": " << r.error().message;
        EXPECT_EQ(tree(*r), expected) << archive;
        EXPECT_EQ(tk::read_file(*r / "bin" / "probe"), "#!/bin/sh\necho probe\n");
    }
    // strip = 0 is the archive's own layout.
    auto whole = xim::extract_archive_detailed(t.root / "a.tar", t.root / "whole");
    ASSERT_TRUE(whole.has_value());
    EXPECT_TRUE(fs::is_regular_file(*whole / "probe-1.0.0" / "bin" / "probe"));
}

XTEST(ExtractStrip, AMemberWithNoMoreThanTheStrippedComponentsIsAnErrorThatNamesIt,
      .area = "xim", .covers = {"LAND-STRIP"}) {
    Temp t;
    auto members = release();
    members.push_back({.path = "LICENSE", .data = "MIT\n"});
    for (const auto& archive : {write_tar(t.root / "b.tar", members), write_zip(t.root / "b.zip", members)}) {
        xim::ExtractOptions options;
        options.strip = 1;
        auto r = xim::extract_archive_detailed(archive, t.root / ("out" + archive.extension().string()), options);
        ASSERT_FALSE(r.has_value()) << archive;
        EXPECT_EQ(r.error().kind, xim::ExtractErrorKind::InvalidInputArchive);
        EXPECT_NE(r.error().message.find("LICENSE"), std::string::npos) << r.error().message;
        EXPECT_NE(r.error().message.find("strip = 1"), std::string::npos) << r.error().message;
    }
    // Two components asked of a member that has two: the file itself would go.
    xim::ExtractOptions two;
    two.strip = 2;
    auto r = xim::extract_archive_detailed(write_tar(t.root / "c.tar", release()), t.root / "out-c", two);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("probe-1.0.0/README"), std::string::npos) << r.error().message;
}

XTEST(ExtractStrip, AHardLinksTargetIsStrippedWithIt, .area = "xim", .covers = {"LAND-STRIP"}) {
    Temp t;
    auto members = release();
    members.push_back({.path = "probe-1.0.0/bin/probe2", .hardlink = "probe-1.0.0/bin/probe"});
    xim::ExtractOptions options;
    options.strip = 1;
    auto r = xim::extract_archive_detailed(write_tar(t.root / "d.tar", members), t.root / "out", options);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(tk::read_file(*r / "bin" / "probe2"), "#!/bin/sh\necho probe\n");
}

XTEST(LandDownload, TheContentIsInTheInstallDirectoryLaidOutAndLeftoversAreGone,
      .area = "xim", .covers = {"LAND-BEFORE-INSTALL"}) {
    Temp t;
    const auto install = t.root / "store" / "probe" / "1.0.0";
    tk::write_file(install / "leftover", "from an attempt that did not finish");
    auto landed = xim::detail_::land_download_(write_tar(t.root / "probe-1.0.0.tar", release()), install,
                                               t.root / "runtime", "xim:probe@1.0.0", 1);
    ASSERT_TRUE(landed.has_value()) << landed.error().message;
    EXPECT_EQ(tree(install), (std::set<std::string>{"bin/", "bin/probe", "README"}));
    EXPECT_FALSE(fs::exists(t.root / "runtime" / ".stage" ) && !fs::is_empty(t.root / "runtime" / ".stage"))
        << "the private extraction is removed";

    // A download that is not an archive is placed as it is, under its name.
    tk::write_file(t.root / "probe.exe", "MZ");
    landed = xim::detail_::land_download_(t.root / "probe.exe", install, t.root / "runtime", "xim:probe@1.0.0", 0);
    ASSERT_TRUE(landed.has_value()) << landed.error().message;
    EXPECT_EQ(tree(install), (std::set<std::string>{"probe.exe"}));
    // ... and `strip` is for an archive.
    landed = xim::detail_::land_download_(t.root / "probe.exe", install, t.root / "runtime", "xim:probe@1.0.0", 1);
    ASSERT_FALSE(landed.has_value());
    EXPECT_NE(landed.error().message.find("is not one"), std::string::npos) << landed.error().message;
}
