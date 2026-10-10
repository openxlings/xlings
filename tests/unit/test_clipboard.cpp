// The clipboard without the display: OSC 52 through the terminal, and the
// host's clipboard through the broker under grants of their own.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

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
    auto copy = b::classify(V{"clipboard", "copy"}, "box");
    auto paste = b::classify(V{"clipboard", "paste"}, "box");
    ASSERT_EQ(copy.route, b::Route::Broker);
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
}
