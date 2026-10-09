// The terminal-injection filter (C11): its shape, and -- on a host that can
// install a seccomp filter -- what it does to a process that carries it.
#include <gtest/gtest.h>
#if defined(__linux__)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <cerrno>
#include <csignal>
#endif

import std;
import xlings.platform;

// Linux only: the filter is a Linux mechanism, and elsewhere there is none.
TEST(SubosSeccomp, TheProgramIsNonEmptyOnSupportedArchitectures) {
    auto p = xlings::platform::seccomp::block_terminal_injection();
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    ASSERT_FALSE(p.empty());
    EXPECT_EQ(p.size() % 8, 0u);
    EXPECT_LT(xlings::platform::seccomp::instruction_count(p), 64u);
#else
    EXPECT_TRUE(p.empty());
#endif
}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
TEST(SubosSeccomp, AProcessCarryingTheFilterCannotInjectIntoATerminal) {
    // In a child: install the filter, then try TIOCSTI (and the 64-bit
    // spelling with a high bit set) on stdin, and an unrelated ioctl.
    auto program = xlings::platform::seccomp::block_terminal_injection();
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

TEST(SubosSeccomp, ExecRemainsBlockedUntilTheSupervisorAllowsOrDeniesIt) {
    for (bool allow : {false, true}) {
        auto ctl = xlings::platform::unix_pair();
        ASSERT_TRUE(ctl);
        const int pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            ::close((*ctl)[0]);
            const int listener = xlings::platform::seccomp::exec_listener();
            const int fds[] = {listener};
            (void)xlings::platform::send_message((*ctl)[1], "listener",
                listener >= 0 ? std::span<const int>(fds) : std::span<const int>{});
            if (listener < 0) ::_exit(77);
            ::close(listener);
            ::close((*ctl)[1]);
            ::execl("/bin/true", "true", static_cast<char*>(nullptr));
            ::_exit(!allow && errno == EACCES ? 0 : 91);
        }
        struct Cleanup {
            int pid;
            int socket;
            int listener = -1;
            ~Cleanup() {
                if (pid > 0) {
                    ::kill(pid, SIGKILL);
                    (void)::waitpid(pid, nullptr, 0);
                }
                ::close(socket);
                if (listener >= 0) ::close(listener);
            }
        } cleanup{pid, (*ctl)[0]};
        ::close((*ctl)[1]);
        xlings::platform::PollFd ready{cleanup.socket};
        ASSERT_GT(xlings::platform::poll_fds(std::span(&ready, 1), 2000), 0);
        auto msg = xlings::platform::receive_message(cleanup.socket);
        ASSERT_TRUE(msg);
        if (msg->fds.empty()) GTEST_SKIP() << "kernel cannot install an exec notification filter";
        cleanup.listener = msg->fds.front();
        xlings::platform::PollFd notification{cleanup.listener};
        ASSERT_GT(xlings::platform::poll_fds(std::span(&notification, 1), 2000), 0);
        auto ex = xlings::platform::seccomp::next_exec(cleanup.listener);
        ASSERT_TRUE(ex);
        EXPECT_EQ(ex->path, "/bin/true");
        int status = 0;
        ASSERT_EQ(::waitpid(pid, &status, WNOHANG), 0) << "exec was released before its audit";
        ASSERT_TRUE(xlings::platform::seccomp::complete_exec(cleanup.listener, ex->id, allow));
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        cleanup.pid = -1;
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0) << "allow=" << allow;
    }
}
TEST(SubosSeccomp, OneListenerBlocksBothExecAndDatagramsUntilTheirAuditCompletes) {
    for (bool allowExec : {false, true}) {
        auto control = xlings::platform::unix_pair();
        ASSERT_TRUE(control);
        const int pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            ::close((*control)[0]);
            std::promise<int> prepared;
            auto ready = prepared.get_future();
            std::jthread helper([&] {
                const int listener = ready.get();
                const std::array descriptors{listener};
                (void)xlings::platform::send_message((*control)[1], "listener",
                    listener >= 0 ? std::span<const int>(descriptors) : std::span<const int>{});
                if (listener >= 0) ::close(listener);
            });
            const int listener = xlings::platform::net_notify::listener({.network = true, .exec = true});
            prepared.set_value(listener);
            helper.join();
            ::close((*control)[1]);
            if (listener < 0) ::_exit(77);
            const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_port = htons(9);
            destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const char payload = 'x';
            if (::sendto(socket, &payload, 1, 0, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) != -1 ||
                errno != EACCES) ::_exit(90);
            ::close(socket);
            ::execl("/bin/true", "true", static_cast<char*>(nullptr));
            ::_exit(!allowExec && errno == EACCES ? 0 : 91);
        }
        struct Cleanup {
            int pid, channel;
            int listener{-1};
            ~Cleanup() {
                if (pid > 0) { ::kill(pid, SIGKILL); (void)::waitpid(pid, nullptr, 0); }
                ::close(channel);
                if (listener >= 0) ::close(listener);
            }
        } cleanup{pid, (*control)[0]};
        ::close((*control)[1]);
        xlings::platform::PollFd ready{cleanup.channel};
        ASSERT_GT(xlings::platform::poll_fds(std::span(&ready, 1), 2000), 0);
        const auto handoff = xlings::platform::receive_message(cleanup.channel);
        ASSERT_TRUE(handoff);
        ASSERT_EQ(handoff->fds.size(), 1u) << "one combined listener must be installed, with no EBUSY fallback";
        cleanup.listener = handoff->fds.front();
        for (const auto expected : {xlings::platform::net_notify::Kind::Network, xlings::platform::net_notify::Kind::Exec}) {
            xlings::platform::PollFd pending{cleanup.listener};
            ASSERT_GT(xlings::platform::poll_fds(std::span(&pending, 1), 2000), 0);
            const auto notification = xlings::platform::net_notify::next(cleanup.listener);
            ASSERT_TRUE(notification);
            EXPECT_EQ(notification->kind, expected);
            int status{};
            ASSERT_EQ(::waitpid(pid, &status, WNOHANG), 0) << "the call ran before its audit acknowledgement";
            if (expected == xlings::platform::net_notify::Kind::Network) {
                EXPECT_EQ(notification->syscall, "sendto");
                EXPECT_EQ(notification->address, "127.0.0.1");
                EXPECT_EQ(notification->port, 9u);
                EXPECT_TRUE(notification->address_readable);
            } else EXPECT_EQ(notification->execPath, "/bin/true");
            ASSERT_TRUE(xlings::platform::net_notify::complete(cleanup.listener, notification->id,
                expected == xlings::platform::net_notify::Kind::Exec && allowExec));
        }
        int status{};
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        cleanup.pid = -1;
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0);
    }
}

#endif
