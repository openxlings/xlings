#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer;
import xlings.subos.rootfs;
import xlings.subos.home_view;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(SubosStateE2E, PrivateCopyAndRemovalCheckTheProducerRoleBeforeTouchingUserData,
      .area = "subos", .covers = {"DOM-LAYOUTS", "ROOT-ROLE-TABLE"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux root instances";
    auto home = tk::Home::isolated("domain-adapter-state");
    auto domain = xlings::home::prefix_domain::resolve(home.dir(), "/xlings", home.root() / "absent-system");
    ASSERT_TRUE(domain) << domain.error();
    ASSERT_TRUE(xlings::home::prefix_domain::prepare_private(*domain));
    const xlings::subos::HomeView producer{domain->physicalHome};
    const auto instance = producer.instance("box");
    tk::write_file(instance / ".xlings.json", R"({"workspace":{}})");
    tk::write_file(producer.instance_file("box"), R"({"kind":"rootfs"})");
    fs::create_directories(instance / std::string(xlings::subos::rootfs::kTree) / "tmp");
    ASSERT_TRUE(xlings::subos::rootfs::commit(instance, {}, "domain adapter fixture"));
    ASSERT_TRUE(xlings::home::domain_producer::publish_scope(*domain, "box"));
    const auto source = home.root() / "source.txt";
    tk::write_file(source, "user content");
    const auto copied = home.xlings({"subos", "cp", source.string(), "box:/tmp/copied.txt"});
    ASSERT_EQ(copied.exit_code, 0) << copied.transcript();
    const auto target = instance / std::string(xlings::subos::rootfs::kTree) / "tmp/copied.txt";
    EXPECT_EQ(tk::read_file(target), "user content");
    auto unconfirmed = home.xlings({"subos", "remove", "box"});
    EXPECT_EQ(unconfirmed.exit_code, 2) << unconfirmed.transcript();
    EXPECT_EQ(tk::read_file(target), "user content");
    tk::write_file(domain->physicalHome / "boot.json", R"({"default":"box"})");
    auto protectedRemoval = home.xlings({"interface", "remove_subos", "--args", R"({"name":"box","yes":true})"});
    EXPECT_NE(protectedRemoval.exit_code, 0) << protectedRemoval.transcript();
    EXPECT_NE(protectedRemoval.transcript().find("boot entry"), std::string::npos) << protectedRemoval.transcript();
    EXPECT_EQ(tk::read_file(target), "user content");
    tk::write_file(domain->physicalHome / "boot.json", "{broken");
    const auto corruptRole = home.xlings({"subos", "cp", source.string(), "box:/tmp/blocked.txt"});
    EXPECT_NE(corruptRole.exit_code, 0) << corruptRole.transcript();
    EXPECT_FALSE(fs::exists(target.parent_path() / "blocked.txt"));
    fs::remove(domain->physicalHome / "boot.json");
    const auto foreign = home.dir() / "subos/box/user-owned";
    tk::write_file(foreign, "foreign control data");
    const auto unknownControl = home.xlings({"subos", "remove", "box", "-y"});
    EXPECT_NE(unknownControl.exit_code, 0) << unknownControl.transcript();
    EXPECT_NE(unknownControl.transcript().find("unowned data"), std::string::npos) << unknownControl.transcript();
    EXPECT_EQ(tk::read_file(foreign), "foreign control data");
    EXPECT_EQ(tk::read_file(target), "user content");
}

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
             {"subos", "remove", "box", "-y"},
             {"interface", "remove_subos", "--args", R"({"name":"box","yes":true})"},
             {"subos", "boot", "--mark-good"}}) {
        const auto result = home.xlings(args);
        EXPECT_NE(result.exit_code, 0) << result.transcript();
        EXPECT_NE(result.transcript().find("boot.json"), std::string::npos) << result.transcript();
        EXPECT_EQ(tk::read_file(boot), "{broken");
    }
    EXPECT_FALSE(fs::exists(home.dir() / "subos/box/tmp/copied.txt"));
    EXPECT_FALSE(fs::exists(home.dir() / "config/subos/box/policy.json"));
    EXPECT_TRUE(fs::is_directory(home.dir() / "subos/box"));
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
