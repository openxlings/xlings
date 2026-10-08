#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.subos.rootfs;
import xlings.subos.roles;
import xlings.core.subos.root;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xvm = xlings::xvm;

XTEST(SubosRootRefresh, MachineLayoutFailureRestoresThePreviousProjection,
      .area = "subos", .covers = {"ROOT-REFRESH-FAIL"}, .requires_ = {"linux"}) {
    auto home = tk::Home::isolated("root-layout-rollback");
    const auto box = home.dir() / "subos/box";
    ASSERT_TRUE(xlings::subos_root::declare_kind(home.dir(), "box", xlings::subos::roles::Kind::Rootfs));
    const auto before = xlings::subos::rootfs::commit(box, {}, "prior empty projection");
    ASSERT_TRUE(before) << before.error();
    tk::write_file(box / "rootfs/var", "user data obstructing machine directories");
    const auto payload = home.dir() / "data/xpkgs/demo-x-probe/1/bin";
    tk::write_file(payload / "probe", "probe sentinel");
    xvm::VersionDB db;
    xvm::add_version(db, "probe", "1", payload.string());
    const auto failed = xlings::subos_root::refresh(home.dir(), "box", box,
        xvm::Workspace{{"probe", "1"}}, db, "failing machine layout");
    ASSERT_TRUE(failed.has_value());
    ASSERT_FALSE(failed->has_value());
    EXPECT_EQ(xlings::subos::rootfs::current(box), *before);
    EXPECT_FALSE(fs::exists(box / "root/usr/bin/probe"));
    EXPECT_EQ(tk::read_file(box / "rootfs/var"), "user data obstructing machine directories");
}

XTEST(SubosRootRefresh, UseReportsProjectionFailureAndRetryRepairsIt,
      .area = "subos", .covers = {"ROOT-REFRESH-FAIL"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("root-refresh");
    auto created = home.xlings({"subos", "new", "box", "--rootfs"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto payload = home.dir() / "data/xpkgs/demo-x-probe/1/bin";
    tk::write_file(payload / "probe", "#!/bin/sh\nexit 0\n");
    fs::permissions(payload / "probe", fs::perms::owner_all);
    xvm::VersionDB db;
    xvm::add_version(db, "probe", "1", payload.string());
    auto config = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json"));
    config["versions"] = xvm::versions_to_json(db);
    tk::write_file(home.dir() / ".xlings.json", config.dump(2));
    const auto box = home.dir() / "subos/box";
    xvm::SubosWorkspace scope;
    scope.installed["probe"] = {"1"};
    auto scope_config = nlohmann::json::parse(tk::read_file(box / ".xlings.json"));
    scope_config["workspace"] = xvm::subos_workspace_to_json(scope);
    tk::write_file(box / ".xlings.json", scope_config.dump(2));
    fs::rename(box / xlings::subos::rootfs::kGenerations, box / "saved-generations");
    tk::write_file(box / xlings::subos::rootfs::kGenerations, "foreign file obstructing generation storage");
    const std::map<std::string, std::string> env{{"XLINGS_ACTIVE_SUBOS", "box"}};
    auto failed = home.xlings({"use", "probe@1"}, env);
    EXPECT_NE(failed.exit_code, 0) << failed.transcript();
    EXPECT_NE(failed.transcript().find("was not updated"), std::string::npos) << failed.transcript();
    EXPECT_EQ(tk::read_file(box / xlings::subos::rootfs::kGenerations), "foreign file obstructing generation storage");
    EXPECT_FALSE(fs::exists(box / "root/usr/bin/probe"));
    const auto after = nlohmann::json::parse(tk::read_file(box / ".xlings.json"));
    EXPECT_EQ(after.at("workspace"), scope_config.at("workspace"));
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json")).at("versions"),
              config.at("versions"));

    fs::remove(box / xlings::subos::rootfs::kGenerations);
    fs::rename(box / "saved-generations", box / xlings::subos::rootfs::kGenerations);
    auto repaired = home.xlings({"use", "probe@1"}, env);
    ASSERT_EQ(repaired.exit_code, 0) << repaired.transcript();
    EXPECT_TRUE(fs::equivalent(box / "root/usr/bin/probe", payload / "probe"));
}
