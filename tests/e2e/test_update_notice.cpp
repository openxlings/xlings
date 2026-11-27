// `xlings config --update-notice <BOOL>` turns the newer-xlings line off and
// on (#425, design 2026-11-27 §9.2); anything but true or false is a usage
// error that names the fix.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;

XTEST(UpdateNotice, TheHomeTurnsItOffAndOn, .area = "xself", .covers = {"UPDATE-NOTICE"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("update-notice-config");
    const auto off = home.xlings({"config", "--update-notice", "false"});
    ASSERT_EQ(off.exit_code, 0) << off.transcript();
    auto json = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json"));
    EXPECT_EQ(json.value("updateNotice", true), false) << json.dump(2);

    const auto bad = home.xlings({"config", "--update-notice", "maybe"});
    EXPECT_EQ(bad.exit_code, 2) << bad.transcript();
    EXPECT_NE(bad.transcript().find("xlings config --update-notice false"), std::string::npos) << bad.transcript();

    const auto on = home.xlings({"config", "--update-notice", "true"});
    ASSERT_EQ(on.exit_code, 0) << on.transcript();
    json = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json"));
    EXPECT_EQ(json.value("updateNotice", false), true) << json.dump(2);
}
