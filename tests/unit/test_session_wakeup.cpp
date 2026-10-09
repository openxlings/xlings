#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.subos.session;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace platform = xlings::platform;
namespace session = xlings::subos::session;
using Json = nlohmann::json;

namespace {

struct Init {
    int fd { -1 };
    int pid { -1 };

    explicit Init(std::vector<std::string> args = {}) {
        const auto pair = platform::unix_pair();
        if (!pair) throw std::runtime_error("cannot create the session control socket");
        pid = platform::fork_process();
        if (pid == 0) {
            platform::close_fd((*pair)[0]);
            platform::reset_signals();
            for (const auto* name : {"XLINGS_SESSION_ROOT_VIEW", "XLINGS_SESSION_LANDLOCK_RW",
                                     "XLINGS_SESSION_NET_TRACE", "XLINGS_SESSION_TRACE"})
                platform::unset_env_variable(name);
            platform::set_env_variable(std::string(session::kControlFdEnv), std::to_string((*pair)[1]));
            platform::set_env_variable(std::string(session::kTtlEnv), "0");
            platform::exit_now(session::session_init(args));
        }
        platform::close_fd((*pair)[1]);
        if (pid < 0) {
            platform::close_fd((*pair)[0]);
            throw std::runtime_error("cannot start session-init");
        }
        fd = (*pair)[0];
    }

    ~Init() {
        platform::close_fd(fd);
        if (pid > 0) {
            platform::send_signal(pid, platform::sig::kill);
            (void)platform::wait_process(pid, true);
        }
    }

    std::optional<Json> reply(int timeoutMs) const {
        platform::PollFd watched{fd};
        if (platform::poll_fds(std::span(&watched, 1), timeoutMs) <= 0) return std::nullopt;
        auto message = platform::receive_message(fd);
        if (!message) return std::nullopt;
        platform::close_fds(message->fds);
        return Json::parse(message->data);
    }

    bool exec(int id, std::vector<std::string> argv) const {
        const int null = platform::open_null();
        const int stdio[]{null, null, null};
        const bool sent = platform::send_message(fd,
            Json{{"op", "exec"}, {"id", id}, {"argv", argv}, {"cwd", "/"}}.dump(), stdio);
        platform::close_fd(null);
        return sent;
    }
};

}  // namespace

XTEST(SessionMessages, PacketSizingPreservesBoundariesAndDescriptorsAndRejectsTruncation,
      .area = "subos", .covers = {"SES-JOIN"}, .requires_ = {"linux"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux sequenced packet sockets";
    const auto sockets = platform::unix_pair();
    const auto pipe = platform::make_pipe();
    ASSERT_TRUE(sockets);
    ASSERT_TRUE(pipe);
    struct Close {
        std::vector<int> fds;
        ~Close() { platform::close_fds(fds); }
    } channels{{(*sockets)[0], (*sockets)[1], (*pipe)[0], (*pipe)[1]}};
    ASSERT_TRUE(platform::write_fd((*pipe)[1], "marker"));
    const int descriptor[]{(*pipe)[0]};
    ASSERT_TRUE(platform::send_message((*sockets)[0], "small", descriptor));
    const std::string large(64 * 1024, 'x');
    ASSERT_TRUE(platform::send_message((*sockets)[0], large));
    auto first = platform::receive_message((*sockets)[1]);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->data, "small");
    ASSERT_EQ(first->fds.size(), 1);
    Close received{first->fds};
    char marker[6];
    ASSERT_TRUE(platform::read_exact(first->fds[0], marker, sizeof(marker)));
    EXPECT_EQ(std::string(marker, sizeof(marker)), "marker");
    const auto second = platform::receive_message((*sockets)[1]);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->data, large);
    const auto fdCount = [] {
        std::size_t count{};
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) ++count;
        return count;
    };
    const auto before = fdCount();
    for (int i = 0; i != 16; ++i) {
        ASSERT_TRUE(platform::send_message((*sockets)[0], "too large", descriptor));
        EXPECT_FALSE(platform::receive_message((*sockets)[1], 4));
    }
    const std::vector<int> tooMany(9, (*pipe)[0]);
    ASSERT_TRUE(platform::send_message((*sockets)[0], "fds", tooMany));
    EXPECT_FALSE(platform::receive_message((*sockets)[1]));
    EXPECT_EQ(fdCount(), before);
    ASSERT_TRUE(platform::send_message((*sockets)[0], "last"));
    const auto last = platform::receive_message((*sockets)[1]);
    ASSERT_TRUE(last);
    EXPECT_EQ(last->data, "last");
}

XTEST(SessionWakeup, AColdChildExitsWithoutWaitingForTheControlPollTick,
      .area = "subos", .covers = {"SES-SUPERVISOR"}, .requires_ = {"linux"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "session-init is implemented on Linux";
    Init init{{"--", "/bin/sh", "-c", "/bin/sleep 0.025; exit 7"}};
    platform::PollFd watched{init.fd};
    // Keep the control peer open without sending traffic. The child exits
    // after the first reap and must wake init instead of waiting its 200 ms tick.
    ASSERT_GT(platform::poll_fds(std::span(&watched, 1), 150), 0);
    EXPECT_TRUE(watched.closed);
    const auto status = platform::wait_process(init.pid, true);
    init.pid = -1;
    ASSERT_TRUE(status);
    EXPECT_EQ(platform::exit_code(*status), 7);
}

XTEST(SessionWakeup, JoinedChildrenReportCompletionSignalsAndExecFailureImmediately,
      .area = "subos", .covers = {"SES-JOIN"}, .requires_ = {"linux"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "session-init is implemented on Linux";
    Init init;
    ASSERT_TRUE(init.exec(1, {"/bin/sh", "-c", "/bin/sleep 0.025; exit 9"}));
    const auto started = init.reply(2000);
    ASSERT_TRUE(started);
    EXPECT_EQ((*started)["id"], 1);
    EXPECT_EQ((*started)["started"], true);
    const auto done = init.reply(150);
    ASSERT_TRUE(done) << "the child finished without control traffic; SIGCHLD must wake init";
    EXPECT_EQ((*done)["id"], 1);
    EXPECT_EQ((*done)["exit"], 9);

    ASSERT_TRUE(init.exec(2, {"/bin/sh", "-c", "kill -TERM $$"}));
    ASSERT_TRUE(init.reply(2000));
    const auto signaled = init.reply(150);
    ASSERT_TRUE(signaled);
    EXPECT_EQ((*signaled)["id"], 2);
    EXPECT_EQ((*signaled)["signal"], platform::sig::terminate);

    ASSERT_TRUE(init.exec(3, {"/xlings-wakeup-test/no-such-program"}));
    const auto failure = init.reply(2000);
    ASSERT_TRUE(failure);
    EXPECT_EQ((*failure)["id"], 3);
    EXPECT_TRUE(failure->contains("error"));
    EXPECT_FALSE(failure->contains("started"));
}
