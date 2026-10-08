#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

void expect_no_staging(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".xlings-stage-"));
}

}  // namespace

XTEST(SubosOutputSafety, PackPreservesExistingDirectoriesAndRefusesExistingArchives,
      .area = "subos", .covers = {"OUTPUT-PRESERVE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("pack-safety");
    const auto created = home.xlings({"subos", "new", "box"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto out = home.root() / "out";
    tk::write_file(out / "bundle-1/user.txt", "keep directory");
    auto packed = home.xlings({"subos", "pack", "box", "--as", "demo:bundle@1", "--out", out.string()});
    ASSERT_EQ(packed.exit_code, 0) << packed.transcript();
    EXPECT_EQ(tk::read_file(out / "bundle-1/user.txt"), "keep directory");
    EXPECT_TRUE(fs::is_regular_file(out / "bundle-1.tar.gz"));
    const auto archive = tk::read_file(out / "bundle-1.tar.gz");
    auto again = home.xlings({"subos", "pack", "box", "--as", "demo:bundle@1", "--out", out.string()});
    EXPECT_NE(again.exit_code, 0) << again.transcript();
    EXPECT_NE(again.transcript().find("output already exists"), std::string::npos);
    EXPECT_EQ(tk::read_file(out / "bundle-1.tar.gz"), archive);
    expect_no_staging(out);
}

XTEST(SubosOutputSafety, PackRejectsPathTraversalAndCleansUpAfterToolFailure,
      .area = "subos", .covers = {"OUTPUT-PRESERVE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("pack-errors");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto out = home.root() / "out";
    fs::create_directory(out);
    for (const auto& spec : {"demo:../escape@1", "demo:bundle@../escape", "demo:bundle@1@2"}) {
        const auto r = home.xlings({"subos", "pack", "box", "--as", spec, "--out", out.string()});
        EXPECT_NE(r.exit_code, 0) << spec << ": " << r.transcript();
    }
    EXPECT_TRUE(fs::is_empty(out));
    if constexpr (tk::is_posix) {
        const auto tools = home.root() / "tools";
        tk::write_file(tools / "tar", "#!/bin/sh\nexit 37\n");
        fs::permissions(tools / "tar", fs::perms::owner_all);
        const auto r = home.xlings({"subos", "pack", "box", "--as", "demo:bundle@1", "--out", out.string()},
                                   {{"PATH", tools.string()}});
        EXPECT_NE(r.exit_code, 0) << r.transcript();
        EXPECT_FALSE(fs::exists(out / "bundle-1.tar.gz"));
        expect_no_staging(out);
    }
}

XTEST(SubosOutputSafety, ExportRefusesFilesDirectoriesAndDanglingLinksBeforeStaging,
      .area = "subos", .covers = {"OUTPUT-PRESERVE"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("export-safety");
    const auto created = home.xlings({"subos", "new", "box", "--rootfs"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto out = home.root() / "out";
    fs::create_directory(out);
    const auto empty = out / "existing-root";
    fs::create_directory(empty);
    const auto archive = out / "existing.tar.gz";
    tk::write_file(archive, "keep archive");
    const auto disk = out / "existing.img";
    fs::create_symlink("missing-image", disk);
    for (const auto& [flag, path] : std::vector<std::pair<std::string, fs::path>>{
             {"--rootfs", empty}, {"--tar", archive}, {"--disk", disk}}) {
        const auto r = home.xlings({"subos", "export", "box", flag, path.string()});
        EXPECT_NE(r.exit_code, 0) << r.transcript();
        EXPECT_NE(r.transcript().find("output already exists"), std::string::npos) << r.transcript();
    }
    EXPECT_TRUE(fs::is_empty(empty));
    EXPECT_EQ(tk::read_file(archive), "keep archive");
    EXPECT_EQ(fs::read_symlink(disk), fs::path("missing-image"));
    expect_no_staging(out);
}
