// What a machine boots is Luban's (design §2): xlings says where the moved
// commands went, keeps their implementation behind plumbing names, and a home
// set up when stage-0 was xlings itself boots luban-init from the next upgrade.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

XTEST(Layering, TheMachinesCommandsSayTheyAreLubans, .area = "subos", .covers = {"LAYER-LUBAN-OWNS-MACHINE"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("layering-moved");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    auto boot = home.xlings({"subos", "boot", "box", "--once"});
    EXPECT_EQ(boot.exit_code, 2) << boot.transcript();
    EXPECT_NE(boot.transcript().find("luban boot box --once"), std::string::npos) << boot.transcript();
    auto iso = home.xlings({"subos", "export", "box", "--iso", (home.dir() / "x.iso").string()});
    EXPECT_EQ(iso.exit_code, 2) << iso.transcript();
    EXPECT_NE(iso.transcript().find("luban export"), std::string::npos) << iso.transcript();
    // The implementation answers under its plumbing name.
    auto entries = home.xlings({"subos", "__boot"});
    EXPECT_EQ(entries.exit_code, 0) << entries.transcript();
    EXPECT_NE(entries.transcript().find("default"), std::string::npos) << entries.transcript();
}

XTEST(Layering, AnOlderHomesStageZeroBecomesLubanInit, .area = "subos", .covers = {"LAYER-LUBAN-OWNS-MACHINE", "HOME-FORMAT-STEPS"},
      .requires_ = {"xlings-bin"}) {
    if constexpr (!tk::is_posix) GTEST_SKIP() << "stage-0 is a Linux machine's";
    auto home = tk::Home::isolated("layering-stage0");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    tk::write_file(home.dir() / "bin" / "luban-init", "#!/bin/sh\n");
    fs::create_directories(home.dir() / "boot");
    fs::create_symlink("../bin/xlings", home.dir() / "boot" / "xlings-init");
    auto doc = Json::parse(tk::read_file(home.dir() / ".xlings.json"));
    doc["upgrade"] = {{"to", "2026.1.1.1"}};
    tk::write_file(home.dir() / ".xlings.json", doc.dump());
    auto first = home.xlings({"subos", "new", "probe"});
    EXPECT_EQ(first.exit_code, 0) << first.transcript();
    EXPECT_EQ(fs::read_symlink(home.dir() / "boot" / "xlings-init").filename(), "luban-init");
}
