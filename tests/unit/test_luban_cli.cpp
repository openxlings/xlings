// luban (Luban design §B3): each command is an xlings command; edition names,
// image formats and the version are luban's own.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import luban.cli;
import xlings.core.config;

namespace cli = luban::cli;
using V = std::vector<std::string>;

namespace {
V x(std::initializer_list<std::string> args) {
    const V a(args);
    auto r = cli::to_xlings(a);
    return r ? *r : V{"ERROR", r.error()};
}
}  // namespace

XTEST(LubanCli, EachCommandIsTheXlingsCommandItNames, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    EXPECT_EQ(x({"new", "box"}), (V{"subos", "new", "box", "--from", "subos:luban-core"}));
    EXPECT_EQ(x({"new", "agent", "agent-workspace", "--proxy", "socks5h://127.0.0.1:7897"}),
              (V{"subos", "new", "agent", "--from", "subos:luban-agent-workspace", "--proxy", "socks5h://127.0.0.1:7897"}));
    EXPECT_EQ(x({"new", "box", "--from=tiny@2026.10.20.1"}),
              (V{"subos", "new", "box", "--from", "subos:luban-tiny@2026.10.20.1"}));
    EXPECT_EQ(x({"new", "box", "acme:my-os"}), (V{"subos", "new", "box", "--from", "acme:my-os"}));
    EXPECT_EQ(x({"enter", "box"}), (V{"subos", "use", "box"}));
    EXPECT_EQ(x({"run", "box", "--", "ls", "-l"}), (V{"subos", "exec", "box", "--", "ls", "-l"}));
    EXPECT_EQ(x({"ls"}), (V{"subos", "list"}));
    EXPECT_EQ(x({"status", "box"}), (V{"subos", "status", "box"}));
    EXPECT_EQ(x({"config", "box", "proxy", "socks5h://p:1"}),
              (V{"subos", "config", "box", "--net", "proxy", "--proxy", "socks5h://p:1"}));
    EXPECT_EQ(x({"config", "box", "proxy", "off"}), (V{"subos", "config", "box", "--net", "host"}));
    EXPECT_EQ(x({"config", "box", "tz", "utc"}), (V{"subos", "config", "box", "--tz", "utc"}));
    EXPECT_EQ(x({"config", "box", "policy", "xim:agent-private"}),
              (V{"subos", "config", "box", "--sandbox", "xim:agent-private"}));
    EXPECT_EQ(x({"history", "box"}), (V{"subos", "rollback", "box", "--list"}));
    EXPECT_EQ(x({"rollback", "box", "--to", "3"}), (V{"subos", "rollback", "box", "--to", "3"}));
    EXPECT_EQ(x({"rm", "box"}), (V{"subos", "remove", "box"}));
    EXPECT_EQ(x({"setup"}), (V{"self", "doctor", "--isolation", "--fix"}));
}

XTEST(LubanCli, AnImageFormatFollowsItsNameAndAnUnknownNameIsRefused, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    EXPECT_EQ(x({"export", "box", "box.iso"}), (V{"subos", "export", "box", "--iso", "box.iso"}));
    EXPECT_EQ(x({"export", "box", "box.img", "--size", "8G"}),
              (V{"subos", "export", "box", "--disk", "box.img", "--size", "8G"}));
    EXPECT_EQ(x({"export", "box", "box.qcow2"}), (V{"subos", "export", "box", "--qcow2", "box.qcow2"}));
    EXPECT_EQ(x({"export", "box", "box.tar.zst"}), (V{"subos", "export", "box", "--tar", "box.tar.zst"}));
    EXPECT_EQ(x({"export", "box", "out/"}), (V{"subos", "export", "box", "--rootfs", "out/"}));
    EXPECT_EQ(x({"export", "box", "box.bin", "--format", "img"}), (V{"subos", "export", "box", "--disk", "box.bin"}));
    const auto refused = x({"export", "box", "box.bin"});
    ASSERT_EQ(refused.front(), "ERROR");
    EXPECT_NE(refused[1].find("--format"), std::string::npos) << refused[1];
}

XTEST(LubanCli, AMissingWordIsAUsageErrorThatSaysWhatIsMissing, .area = "luban", .covers = {"LUBAN-CLI-MAP"}) {
    EXPECT_EQ(x({"enter"}).front(), "ERROR");
    EXPECT_EQ(x({"run", "box"}).front(), "ERROR");
    const auto noValue = x({"config", "box", "proxy"});
    ASSERT_EQ(noValue.front(), "ERROR");
    EXPECT_NE(noValue[1].find("luban config box"), std::string::npos) << noValue[1];
    EXPECT_EQ(x({"config", "box", "colour", "red"}).front(), "ERROR");
}

XTEST(LubanCli, LubanIsReleasedAsTheXlingsItDrives, .area = "luban", .covers = {"LUBAN-TOOL-SHIPPED"}) {
    EXPECT_EQ(cli::kVersion, xlings::Info::VERSION)
        << "bump luban/src/cli/cli.cppm kVersion with mcpp.toml and src/core/config.cppm";
}
