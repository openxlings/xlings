// The clipboard without the display: OSC 52 through the terminal, and the
// host's clipboard through the broker under grants of their own.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
#if !defined(_WIN32)   // setenv: the bridge is a Linux sandbox's
#include <stdlib.h>
#endif

import std;
import xlings.libs.json;
import xlings.core.clipboard;
import xlings.subos.broker;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.home_view;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace pol = xlings::subos::policy;

XTEST(Clipboard, ThroughTheTerminalIsOsc52WithBase64, .area = "subos", .covers = {"SUBOS-CLIPBOARD"}) {
    EXPECT_EQ(xlings::clipboard::osc52("hello"), "\x1b]52;c;aGVsbG8=\x07");
    EXPECT_EQ(xlings::clipboard::osc52(""), "\x1b]52;c;\x07");
    EXPECT_EQ(xlings::clipboard::osc52("ab"), "\x1b]52;c;YWI=\x07");
    EXPECT_EQ(xlings::clipboard::osc52("abc"), "\x1b]52;c;YWJj\x07");
    EXPECT_EQ(xlings::clipboard::osc52(std::string("\xff\x00\x10", 3)), "\x1b]52;c;/wAQ\x07");
}

XTEST(Clipboard, TheGrantsAreTheirOwnNeverImpliedAndAPolicyHoldingOneSaysWhichClientReadsIt,
      .area = "subos", .covers = {"SUBOS-CLIPBOARD"}) {
    using V = std::vector<std::string>;
    namespace b = xlings::subos::broker;
    auto copy = b::classify(V{"__clipboard", "copy"}, "box");
    auto paste = b::classify(V{"__clipboard", "paste"}, "box");
    ASSERT_EQ(copy.touch, xlings::subos::situation::Touch::Instance);
    auto p = pol::preset(pol::Preset::Private);
    EXPECT_EQ(b::decide(p, copy).action, pol::Action::Deny);
    p.grants.insert("clipboard");
    EXPECT_EQ(b::decide(p, copy).action, pol::Action::Allow);
    EXPECT_EQ(b::decide(p, paste).action, pol::Action::Deny);
    EXPECT_FALSE(pol::legacy().grants_allowed.contains("clipboard"));

    // A policy file without the clipboard grants is readable by the clients
    // before them; one with them says it needs the client that knows them.
    auto home = tk::Home::isolated("clipboard-policy");
    const xlings::subos::HomeView view{home.dir()};
    ASSERT_TRUE(xlings::subos::policy_store::write(view, "plain", pol::preset(pol::Preset::Private)));
    auto plain = nlohmann::json::parse(tk::read_file(view.policy_file("plain")));
    EXPECT_EQ(plain["min_client"], std::string(pol::kPolicyMinClient));
    ASSERT_TRUE(xlings::subos::policy_store::write(view, "granted", p));
    auto granted = nlohmann::json::parse(tk::read_file(view.policy_file("granted")));
    EXPECT_EQ(granted["min_client"], std::string(pol::kClipboardMinClient));
    // desktop-control (Luban OS design 2026-10-24 §3.4): explicit too, and newer.
    EXPECT_FALSE(pol::legacy().grants_allowed.contains("desktop-control"));
    p.grants.insert("desktop-control");
    ASSERT_TRUE(xlings::subos::policy_store::write(view, "driven", p));
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(view.policy_file("driven")))["min_client"], std::string(pol::kDesktopControlMinClient));
}

XTEST(Clipboard, TheBridgeIsTheToolsNamesInAGrantedSandboxNeverAShim,
      .area = "subos", .covers = {"SUBOS-CLIPBOARD"}) {
#if defined(_WIN32)
    GTEST_SKIP() << "the bridge lives in a Linux sandbox";
#else
    using xlings::clipboard::bridge_tool;
    ::setenv("XLINGS_CLIPBOARD_BRIDGE", "/run/xlings/bin", 1);
    EXPECT_EQ(bridge_tool("wl-copy"), std::optional<std::string>("wl-copy")) << "started by name from PATH";
    EXPECT_EQ(bridge_tool("/run/xlings/bin/xclip"), std::optional<std::string>("xclip"));
    EXPECT_FALSE(bridge_tool("/home/u/.xlings/subos/box/bin/xclip")) << "a package's shim of the same name";
    EXPECT_FALSE(bridge_tool("xlings"));
    ::unsetenv("XLINGS_CLIPBOARD_BRIDGE");
    EXPECT_FALSE(bridge_tool("wl-copy")) << "not granted: no bridge";
#endif
}
