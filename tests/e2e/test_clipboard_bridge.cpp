// The clipboard tools a sandbox is given (design 2026-10-24 §8.1, J60): in a
// desktop session they are the system's own -- the session's compositor is
// the clipboard -- and never the host's through the broker; whatever goes
// wrong is said on standard error, and nothing is pasted.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

XTEST(ClipboardBridge, InADesktopSessionTheToolsAreTheSystemsOwnAndAFailurePastesNothing, .area = "subos",
      .covers = {"LUBAN-CLIPBOARD-SESSION", "SUBOS-CLIPBOARD"}, .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("clipboard-bridge");
    const auto root = home.root();
    const auto bridge = root / "bridge", real = root / "real", runtime = root / "runtime";
    for (const auto& d : {bridge, real, runtime}) fs::create_directories(d);
    fs::create_symlink(tk::xlings_binary(), bridge / "wl-paste");
    tk::write_file(real / "wl-paste", "#!/bin/sh\necho \"the system's own $*\"\n");
    fs::permissions(real / "wl-paste", fs::perms::owner_all);
    tk::write_file(runtime / "wayland-0", "");   // the session's compositor answers here
    auto env = home.env();
    env["XLINGS_CLIPBOARD_BRIDGE"] = bridge.string();
    env["PATH"] = bridge.string() + ":" + real.string() + ":/usr/bin:/bin";
    env["WAYLAND_DISPLAY"] = "wayland-0";
    env["XDG_RUNTIME_DIR"] = runtime.string();
    auto r = tk::run({.argv = {(bridge / "wl-paste").string(), "--no-newline"}, .env = env, .timeout = 20s});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(r.out, "the system's own --no-newline\n");
    // A desktop without one: said, and nothing on standard output to paste.
    fs::remove(real / "wl-paste");
    r = tk::run({.argv = {(bridge / "wl-paste").string(), "--no-newline"}, .env = env, .timeout = 20s, .pty = true});
    EXPECT_NE(r.exit_code, 0);
    EXPECT_EQ(r.out.find("the system"), std::string::npos);
    EXPECT_NE(r.transcript().find("no wl-paste of its own"), std::string::npos) << r.transcript();
    // Not in a desktop and no broker: said on standard error only.
    env.erase("WAYLAND_DISPLAY");
    r = tk::run({.argv = {(bridge / "wl-paste").string(), "--no-newline"}, .env = env, .timeout = 20s});
    EXPECT_NE(r.exit_code, 0);
    EXPECT_TRUE(r.out.empty()) << r.transcript();
    EXPECT_FALSE(r.err.empty());
}
