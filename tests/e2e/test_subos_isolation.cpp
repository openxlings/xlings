// Isolation, measured from inside a real sandbox (L4, design §24.4).
//
// Each case runs `xlings subos use <s> --sandbox --cmd ...` with the backend
// this host has and asserts what the process inside can and cannot see.
// These are the only tests that may cover an isolation requirement
// (`proves = "isolation"`): the spec goldens show what was asked for; these
// show the kernel did it.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

// A home with a sandbox backend and one instance.
struct Box {
    tk::Home home = tk::Home::isolated("iso");
    std::string name = "box";

    Box() {
        home.seed_sandbox_backend();
        auto r = home.xlings({"subos", "new", name});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }

    tk::RunResult run(const std::string& cmd, std::map<std::string, std::string> env = {}) const {
        return home.xlings({"subos", "use", name, "--sandbox", "--cmd", cmd}, std::move(env));
    }
};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

XTEST(SubosIsolation, OnlyAllowListedEnvironmentEnters,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F3"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    auto r = box.run("env", {{"GITHUB_TOKEN", "do-not-leak"}, {"SSH_AUTH_SOCK", "/tmp/agent"},
                             {"EDITOR", "vi"}, {"LANG", "C.UTF-8"}});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(r.out.find("do-not-leak"), std::string::npos) << r.out;
    EXPECT_EQ(r.out.find("SSH_AUTH_SOCK"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("EDITOR=vi"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("XLINGS_SUBOS_MODE=sandbox"), std::string::npos) << r.out;
}

XTEST(SubosIsolation, HostProcessesAreInvisible,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F4"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    auto r = box.run("ls /proc | grep -c '^[0-9][0-9]*$'");
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    // bwrap's init, the shell, ls and grep -- not the host's hundreds. The
    // count is the last line; what xlings itself said comes before it.
    auto out = trim(r.out);
    auto last = out.substr(out.rfind('\n') == std::string::npos ? 0 : out.rfind('\n') + 1);
    ASSERT_FALSE(last.empty()) << r.transcript();
    ASSERT_TRUE(std::isdigit(static_cast<unsigned char>(last[0]))) << r.transcript();
    EXPECT_LT(std::stoi(last), 10) << r.out;
}

XTEST(SubosIsolation, ANonInteractiveCommandHasNoControllingTerminal,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F8"},
      .requires_ = {"linux", "xlings-bin", "sandbox", "pty"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    // The case that matters: run from a terminal with stdin redirected. The
    // caller HAS a controlling terminal; the command inside must not, or it
    // could open /dev/tty and push input into the user's shell.
    // Field 7 of /proc/<pid>/stat is the controlling terminal (0 = none).
    tk::RunOptions o;
    o.argv = {"/bin/sh", "-c", "exec \"$0\" \"$@\" < /dev/null",
              tk::xlings_binary().string(), "subos", "use", box.name, "--sandbox", "--cmd",
              R"(set -- $(cat /proc/$$/stat); echo "TTY_NR=$7")"};
    o.env = box.home.env();
    o.pty = true;
    o.timeout = std::chrono::seconds(60);
    auto r = tk::run(o);
    EXPECT_NE(r.out.find("TTY_NR=0"), std::string::npos) << r.transcript();
}

XTEST(SubosIsolation, AnInteractiveSandboxCannotInjectIntoTheTerminal,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F8"},
      .requires_ = {"linux", "xlings-bin", "sandbox", "pty"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    tk::RunOptions o;
    o.argv = {"subos", "use", box.name, "--sandbox", "--cmd",
              "python3 -c 'import fcntl,termios\n"
              "try:\n fcntl.ioctl(0, termios.TIOCSTI, b\"x\"); print(\"TIOCSTI-ALLOWED\")\n"
              "except OSError as e: print(\"TIOCSTI-ERRNO\", e.errno)'"};
    o.pty = true;
    o.timeout = std::chrono::seconds(60);
    auto r = box.home.xlings(o);
    if (r.out.find("python3: not found") != std::string::npos
        || r.out.find("No such file") != std::string::npos)
        GTEST_SKIP() << "no python3 on this host";
    // EPERM (1) is the filter. A kernel with legacy_tiocsti=0 would answer
    // EIO on its own; EPERM proves it was ours.
    EXPECT_NE(r.out.find("TIOCSTI-ERRNO 1"), std::string::npos) << r.transcript();
}

XTEST(SubosIsolation, AFailedProbeQuotesBwrapAndNeverAdvisesASysctl,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F12"},
      .requires_ = {"linux", "xlings-bin"}) {
    auto home = tk::Home::isolated("probe-text");
    // A root-owned or system bwrap is tried before the payload, so the fake
    // below is only reached on a host where none of those works.
    for (const char* host : {"/usr/lib/xlings/bwrap", "/usr/bin/bwrap", "/usr/local/bin/bwrap"}) {
        if (!fs::exists(host)) continue;
        if (tk::run({.argv = {host, "--ro-bind", "/", "/", "--", "/bin/true"}}).exit_code == 0)
            GTEST_SKIP() << host << " works here; the isolation-fix lane covers the failing case";
    }
    // A bwrap that fails the way Ubuntu 24.04's AppArmor makes it fail.
    auto bin = home.dir() / "data" / "xpkgs" / "xim-x-bwrap" / "0.0.0" / "bin" / "bwrap";
    tk::write_file(bin, "#!/bin/sh\necho 'bwrap: setting up uid map: Permission denied' >&2\nexit 1\n");
    fs::permissions(bin, fs::perms::owner_all);
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    auto r = home.xlings({"subos", "use", "box", "--sandbox", "bwrap", "--cmd", "true"});
    EXPECT_NE(r.exit_code, 0);
    auto all = r.transcript();
    EXPECT_NE(all.find("setting up uid map: Permission denied"), std::string::npos) << all;
    EXPECT_NE(all.find("self doctor --isolation --fix"), std::string::npos) << all;
    EXPECT_EQ(all.find("sysctl -w"), std::string::npos) << all;
}
