#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(SubosStateE2E, InvalidInstanceMetadataBlocksStatusCopyConfigAndPackWithoutMutation,
      .area = "subos", .covers = {"STATE-STRICT"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("instance-state");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto file = home.dir() / "config/subos/box/instance.json";
    const auto source = home.root() / "source.txt";
    tk::write_file(source, "copy content");
    for (auto bad : {"{broken", R"({"kind":"unknown"})", R"({"kind":false})"}) {
        tk::write_file(file, bad);
        for (const auto& args : std::vector<std::vector<std::string>>{
                 {"subos", "status", "box", "--json"},
                 {"subos", "cp", source.string(), "box:/tmp/copied.txt"},
                 {"subos", "config", "box", "--sandbox=dev"},
                 {"subos", "pack", "box", "--as", "demo:bundle@1", "--out", (home.root() / "out").string()}}) {
            const auto result = home.xlings(args);
            EXPECT_NE(result.exit_code, 0) << result.transcript();
            EXPECT_NE(result.transcript().find("instance.json"), std::string::npos) << result.transcript();
            EXPECT_EQ(tk::read_file(file), bad);
        }
        EXPECT_FALSE(fs::exists(home.dir() / "subos/box/tmp/copied.txt"));
        EXPECT_FALSE(fs::exists(home.dir() / "config/subos/box/policy.json"));
    }
}

XTEST(SubosStateE2E, CorruptBootStateCannotRemoveProtectionOrBeOverwrittenByABootCommand,
      .area = "subos", .covers = {"STATE-STRICT"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("boot-state");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto boot = home.dir() / "boot.json";
    tk::write_file(boot, "{broken");
    const auto source = home.root() / "source.txt";
    tk::write_file(source, "copy content");
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"subos", "status", "box", "--json"},
             {"subos", "cp", source.string(), "box:/tmp/copied.txt"},
             {"subos", "config", "box", "--sandbox=dev"},
             {"subos", "boot", "--mark-good"}}) {
        const auto result = home.xlings(args);
        EXPECT_NE(result.exit_code, 0) << result.transcript();
        EXPECT_NE(result.transcript().find("boot.json"), std::string::npos) << result.transcript();
        EXPECT_EQ(tk::read_file(boot), "{broken");
    }
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box/tmp/copied.txt"));
    EXPECT_FALSE(fs::exists(home.dir() / "config/subos/box/policy.json"));
}

XTEST(SubosStateE2E, LegacyInstancesRemainViewsAndDanglingMetadataIsAnError,
      .area = "subos", .covers = {"STATE-STRICT"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("legacy-state");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto status = home.xlings({"subos", "status", "box", "--json"});
    ASSERT_EQ(status.exit_code, 0) << status.transcript();
    EXPECT_NE(status.out.find("view"), std::string::npos) << status.out;
    if constexpr (tk::is_posix) {
        const auto file = home.dir() / "config/subos/box/instance.json";
        fs::create_directories(file.parent_path());
        fs::create_symlink("missing-instance", file);
        status = home.xlings({"subos", "status", "box", "--json"});
        EXPECT_NE(status.exit_code, 0) << status.transcript();
        EXPECT_NE(status.transcript().find("instance.json"), std::string::npos);
        EXPECT_EQ(fs::read_symlink(file), fs::path("missing-instance"));
    }
}
