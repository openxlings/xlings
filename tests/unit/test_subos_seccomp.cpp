// The terminal-injection filter (C11): its shape, and -- on a host that can
// install a seccomp filter -- what it does to a process that carries it.
#include <gtest/gtest.h>
#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <cerrno>
#endif

import std;
import xlings.subos.seccomp;

TEST(SubosSeccomp, TheProgramIsNonEmptyOnSupportedArchitectures) {
    auto p = xlings::subos::seccomp::block_terminal_injection();
#if defined(__x86_64__) || defined(__aarch64__)
    ASSERT_FALSE(p.empty());
    EXPECT_EQ(p.size() % 8, 0u);
    EXPECT_LT(xlings::subos::seccomp::instruction_count(p), 64u);
#else
    EXPECT_TRUE(p.empty());
#endif
}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
TEST(SubosSeccomp, AProcessCarryingTheFilterCannotInjectIntoATerminal) {
    // In a child: install the filter, then try TIOCSTI (and the 64-bit
    // spelling with a high bit set) on stdin, and an unrelated ioctl.
    auto program = xlings::subos::seccomp::block_terminal_injection();
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        struct sock_fprog prog{};
        prog.len = static_cast<unsigned short>(program.size() / 8);
        prog.filter = reinterpret_cast<struct sock_filter*>(program.data());
        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) ::_exit(10);
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) ::_exit(11);
        char c = 'x';
        errno = 0;
        int r1 = ::ioctl(0, TIOCSTI, &c);
        int e1 = errno;
        errno = 0;
        int r2 = ::syscall(SYS_ioctl, 0, 0x100000000UL | TIOCSTI, &c);
        int e2 = errno;
        errno = 0;
        int winsz[4];
        ::ioctl(0, TIOCGWINSZ, winsz);   // allowed (may fail with ENOTTY, never EPERM)
        int e3 = errno;
        if (r1 != -1 || e1 != EPERM) ::_exit(20);
        if (r2 != -1 || e2 != EPERM) ::_exit(21);
        if (e3 == EPERM) ::_exit(22);
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "10/11: could not install; 20: TIOCSTI allowed; "
                                         "21: high-bit spelling allowed; 22: other ioctl blocked";
}
#endif
