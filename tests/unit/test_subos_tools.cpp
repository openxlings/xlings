// Every external program the SubOS code runs comes from one table
// (xlings.subos.tools), and a tarball is written in-process (part 3 §6.4).
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.tools;
import xlings.core.xim.extract;
import xlings.platform;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace tools = xlings::subos::tools;
using xlings::subos::HomeView;
using xlings::subos::Ports;

XTEST(SubosTools, APayloadComesFromTheHomeAndAnUnknownNameFromNowhere,
      .area = "subos", .covers = {"TOOL-RESOLVE"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("tools-payload");
    const HomeView view{home.dir()};
    EXPECT_TRUE(tools::candidates("no-such-tool", view, Ports{}).empty());
    EXPECT_EQ(tools::install_hint("pasta"), "xlings install passt");

    const auto pasta = home.dir() / "data/xpkgs/xim-x-passt/2025.01/bin/pasta";
    tk::write_file(pasta, "#!/bin/sh\n");
    auto found = tools::first("pasta", view, Ports{});
    ASSERT_TRUE(found);
    EXPECT_EQ(found->bin, pasta);
    EXPECT_EQ(found->source, tools::Source::Payload) << "the home's payload before the machine's";
    EXPECT_EQ(tools::to_string(found->source), "payload");

    // mkfs.ext4 lives in sbin in e2fsprogs.
    const auto mkfs = home.dir() / "data/xpkgs/xim-x-e2fsprogs/1.47/sbin/mkfs.ext4";
    tk::write_file(mkfs, "#!/bin/sh\n");
    auto fs_tool = tools::first("mkfs.ext4", view, Ports{});
    ASSERT_TRUE(fs_tool);
    EXPECT_EQ(fs_tool->bin, mkfs);
}

XTEST(SubosTools, AMachineBinaryThatIsAnotherHomesShimIsNotATool,
      .area = "subos", .covers = {"TOOL-RESOLVE"}, .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("tools-shim");
    const HomeView view{home.dir()};
    auto machine = tools::candidates("curl", view, Ports{});
    if (machine.empty()) GTEST_SKIP() << "no /usr/bin/curl on this machine";
    EXPECT_EQ(machine.front().source, tools::Source::Host);
    EXPECT_EQ(tools::to_string(machine.front().source), "system");
    Ports shims;
    shims.shim_owner = [](const fs::path&) -> std::optional<fs::path> { return fs::path("/elsewhere"); };
    EXPECT_TRUE(tools::candidates("curl", view, shims).empty());
}

// The three layers (the aarch64 design §2.1): every tool is a package of the
// index unless the operating system itself is the tool, and then it says
// why; no hint ever names a machine's package manager.
XTEST(SubosTools, EveryToolIsAPackageOrTheOperatingSystemAndSaysWhich,
      .area = "subos", .covers = {"TOOLS-PROVENANCE"}) {
    for (const auto& tool : tools::known()) {
        if (tool.package.empty()) {
            EXPECT_FALSE(tool.os.empty()) << tool.name << ": no package and no reason the OS provides it";
            EXPECT_TRUE(tools::install_hint(tool.name).empty()) << tool.name;
        } else {
            EXPECT_TRUE(tool.os.empty()) << tool.name << ": a package and the OS's at once";
            EXPECT_EQ(tools::install_hint(tool.name), "xlings install " + std::string(tool.package)) << tool.name;
        }
        EXPECT_TRUE(tool.platform.empty() || tool.platform == "linux" || tool.platform == "macos" ||
                    tool.platform == "windows") << tool.name;
    }
    for (const auto gone : {"cp", "xorriso", "qemu-img", "mksquashfs", "xclip"})
        EXPECT_TRUE(std::ranges::none_of(tools::known(), [&](const auto& t) { return t.name == gone; }))
            << gone << " is written in process, or another tool covers it";
    EXPECT_EQ(tools::install_hint("qemu-system-x86_64"), "xlings install qemu-system");
    EXPECT_EQ(tools::install_hint("qemu-system-aarch64"), "xlings install qemu-system");
    EXPECT_EQ(tools::install_hint("wl-copy"), "xlings install wl-clipboard");
}

XTEST(SubosTools, ATreeIsCopiedInProcessWithLinksModesAndTimes,
      .area = "subos", .covers = {"TOOLS-PROVENANCE"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("tools-copy-tree");
    const auto from = home.root() / "from";
    tk::write_file(from / "usr/bin/tool", "#!/bin/sh\necho tool\n");
    fs::permissions(from / "usr/bin/tool", fs::perms::owner_all | fs::perms::group_read);
    fs::create_directory_symlink("usr/bin", from / "bin");
    tk::write_file(from / "etc/ro/file", "x");
    fs::permissions(from / "etc/ro", fs::perms::owner_read | fs::perms::owner_exec);
    const auto to = home.root() / "to";
    tk::write_file(to / "already", "kept");
    auto copied = xlings::platform::copy_tree(from, to);
    fs::permissions(from / "etc/ro", fs::perms::owner_all);
    ASSERT_TRUE(copied) << copied.error();
    EXPECT_EQ(tk::read_file(to / "usr/bin/tool"), "#!/bin/sh\necho tool\n");
    EXPECT_EQ(fs::status(to / "usr/bin/tool").permissions() & fs::perms::all,
              fs::perms::owner_all | fs::perms::group_read);
    EXPECT_TRUE(fs::is_symlink(to / "bin"));
    EXPECT_EQ(fs::read_symlink(to / "bin"), fs::path("usr/bin"));
    EXPECT_EQ(tk::read_file(to / "already"), "kept") << "merged into, not replaced";
    EXPECT_EQ(fs::status(to / "etc/ro").permissions() & fs::perms::owner_write, fs::perms::none)
        << "a read-only directory's mode is applied after its contents";
    fs::permissions(to / "etc/ro", fs::perms::owner_all);
}

XTEST(SubosTools, ATarballIsWrittenInProcessWithLinksKeptAndRootOwnedEntries,
      .area = "subos", .covers = {"TOOL-RESOLVE"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("tools-tar");
    const auto tree = home.root() / "tree";
    tk::write_file(tree / "usr/bin/hello", "#!/bin/sh\necho hello\n");
    fs::create_directory_symlink("usr/bin", tree / "bin");
    tk::write_file(tree / "etc/hostname", "luban\n");
    const auto archive = home.root() / "out.tar.gz";
    auto written = xlings::xim::write_tar_gz(tree, archive, xlings::xim::ArchiveOwner::Root);
    ASSERT_TRUE(written) << written.error();

    const auto back = home.root() / "back";
    auto extracted = xlings::xim::extract_archive(archive, back);
    ASSERT_TRUE(extracted) << extracted.error();
    EXPECT_EQ(tk::read_file(back / "usr/bin/hello"), "#!/bin/sh\necho hello\n");
    EXPECT_TRUE(fs::is_symlink(back / "bin")) << "a link is stored as a link";
    EXPECT_EQ(fs::read_symlink(back / "bin"), fs::path("usr/bin"));

    // Who the entries belong to, as the tar format records it: 0/0.
    const fs::path tar = fs::exists("/usr/bin/tar") ? "/usr/bin/tar" : "/bin/tar";
    if (fs::exists(tar)) {
        auto listed = tk::run({.argv = {tar.string(), "--numeric-owner", "-tvzf", archive.string()},
                               .env = {{"PATH", "/usr/bin:/bin"}}});
        ASSERT_EQ(listed.exit_code, 0) << listed.transcript();
        std::istringstream lines(listed.out);
        int entries = 0;
        for (std::string line; std::getline(lines, line);) {
            if (line.empty()) continue;
            ++entries;
            // GNU tar: "-rw-r--r-- 0/0 22 ..."; bsdtar (macOS): "-rw-r--r--  0 0 0 22 ..."
            // (link count, uid, gid).
            std::istringstream fields(line);
            std::vector<std::string> f;
            for (std::string w; fields >> w && f.size() < 4;) f.push_back(w);
            ASSERT_GE(f.size(), 3u) << line;
            if (f[1].find('/') != std::string::npos) EXPECT_EQ(f[1], "0/0") << line;
            else EXPECT_TRUE(f.size() == 4 && f[2] == "0" && f[3] == "0") << line;
        }
        EXPECT_GE(entries, 5);
    }
}
