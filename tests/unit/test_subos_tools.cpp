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
    auto machine = tools::candidates("cp", view, Ports{});
    if (machine.empty()) GTEST_SKIP() << "no /bin/cp or /usr/bin/cp on this machine";
    EXPECT_EQ(machine.front().source, tools::Source::Host);
    EXPECT_EQ(tools::to_string(machine.front().source), "system");
    Ports shims;
    shims.shim_owner = [](const fs::path&) -> std::optional<fs::path> { return fs::path("/elsewhere"); };
    EXPECT_TRUE(tools::candidates("cp", view, shims).empty());
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
            EXPECT_NE(line.find(" 0/0 "), std::string::npos) << line;
        }
        EXPECT_GE(entries, 5);
    }
}
