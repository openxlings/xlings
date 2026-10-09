// A child run with a deadline gets its own process group. When the caller
// holds the terminal, the child's group must hold it too while it runs:
// otherwise the kernel stops the child (SIGTTOU) the moment it touches the
// terminal, and only the deadline ends it. `subos new --from subos:luban-core`
// hung that way -- the declared-packages install probes the terminal.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

#if !defined(_WIN32)   // a terminal job is a POSIX notion (Windows: a Job Object, no foreground group)
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

import std;
import xlings.platform;

XTEST(ProcessTerminal, AChildInItsOwnGroupMayUseTheTerminalAndTheTerminalComesBack,
      .area = "platform", .covers = {"PROCESS-TERMINAL-JOB"}, .requires_ = {"posix"}) {
#if defined(_WIN32)
    GTEST_SKIP() << "a POSIX terminal's process groups";
#else
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_GE(master, 0);
    ASSERT_EQ(::grantpt(master), 0);
    ASSERT_EQ(::unlockpt(master), 0);
    const std::string slave = ::ptsname(master);

    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // A session whose controlling terminal is the pty, in its foreground:
        // a user at a shell prompt.
        ::setsid();
        const int fd = ::open(slave.c_str(), O_RDWR);
        if (fd < 0 || ::ioctl(fd, TIOCSCTTY, 0) != 0) ::_exit(90);
        ::dup2(fd, 0); ::dup2(fd, 1); ::dup2(fd, 2);
        const int rc = xlings::platform::run_argv_with_timeout(
            {"/bin/sh", "-c", "stty -echo && stty echo && exit 7"}, std::chrono::seconds(5));
        const bool back = ::tcgetpgrp(0) == ::getpgrp();
        ::_exit(rc == 7 && back ? 0 : (back ? 100 + (rc & 0x3f) : 99));
    }
    ::close(::open(slave.c_str(), O_RDWR | O_NOCTTY));   // keep the pty from hanging up early
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    while (::waitpid(pid, &status, WNOHANG) == 0) {
        char drain[256];
        while (::read(master, drain, sizeof drain) > 0) {}
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            FAIL() << "the run did not end";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::close(master);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "99: the terminal did not come back; 100+N: the child exited N (124: stopped until the deadline)";
#endif
}
