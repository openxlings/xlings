// The guest agent (luban.vm) on a stream instead of its port: what the Luban
// host machine on a Mac runs every command through (`luban __vm exec`). A
// command on pipes keeps its bytes as written (an NDJSON stream: no echo, no
// changed line ends), its standard error apart, its input closed when the
// host says so, and its exit code; one on a terminal passes its exit code.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.platform;
import luban.vm;
import luban.vm.protocol;

namespace platform = xlings::platform;
namespace proto = luban::vm::protocol;
using Json = nlohmann::json;

namespace {

struct Seen {
    std::string out, err;
    std::optional<int> code;
};

// Frames on `channel` until it exits (or 20 s).
Seen read_until_exit_(int fd, proto::Decoder& d, std::uint32_t channel) {
    Seen s;
    std::array<char, 65536> buf{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!s.code && std::chrono::steady_clock::now() < deadline) {
        std::array<platform::PollFd, 1> p{{{.fd = fd}}};
        platform::poll_fds(p, 200);
        if (p[0].readable || p[0].closed) {
            auto n = platform::read_some(fd, buf);
            if (!n || *n == 0) break;
            d.feed(std::string_view(buf.data(), *n));
        }
        for (;;) {
            auto f = d.next();
            if (!f || !*f) break;
            const auto& frame = **f;
            if (frame.channel != channel) continue;
            if (frame.type == 'D') s.out += frame.payload;
            else if (frame.type == 'E') s.err += frame.payload;
            else if (frame.type == 'X') s.code = Json::parse(frame.payload, nullptr, false).value("code", -1);
        }
    }
    return s;
}

}  // namespace

XTEST(LubanAgent, ACommandOnPipesKeepsItsBytesItsErrorsApartAndItsExitCode, .area = "luban",
      .covers = {"CARRIER-VZ-HELPER"}, .requires_ = {"linux"}) {
    auto pair = platform::unix_stream_pair();
    ASSERT_TRUE(pair);
    const int pid = platform::fork_process();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        std::vector<int> theirs{(*pair)[0]};
        platform::close_fds(theirs);
        luban::vm::serve_agent((*pair)[1]);
    }
    std::vector<int> mine{(*pair)[1]};
    platform::close_fds(mine);
    const int fd = (*pair)[0];
    proto::Decoder d;
    // The agent says it is up first.
    bool up = false;
    std::array<char, 4096> buf{};
    for (int i = 0; i < 100 && !up; ++i) {
        std::array<platform::PollFd, 1> p{{{.fd = fd}}};
        platform::poll_fds(p, 100);
        if (p[0].readable)
            if (auto n = platform::read_some(fd, buf); n && *n > 0) d.feed(std::string_view(buf.data(), *n));
        for (auto f = d.next(); f && *f; f = d.next()) up = up || (**f).type == 'H';
    }
    ASSERT_TRUE(up) << "the agent did not say hello";

    ASSERT_TRUE(platform::write_all(fd, proto::encode_json('O', 1, Json{
        {"argv", {"/bin/sh", "-c", "printf '{\"kind\":\"a\"}\\n'; printf 'warned\\n' >&2; cat; exit 7"}}, {"pipes", true}})));
    ASSERT_TRUE(platform::write_all(fd, proto::encode('D', 1, "line one\nline two\n")));
    ASSERT_TRUE(platform::write_all(fd, proto::encode('F', 1)));
    const auto piped = read_until_exit_(fd, d, 1);
    ASSERT_TRUE(piped.code) << piped.out << piped.err;
    EXPECT_EQ(*piped.code, 7);
    EXPECT_EQ(piped.out, "{\"kind\":\"a\"}\nline one\nline two\n") << "no echo, no changed line ends";
    EXPECT_EQ(piped.err, "warned\n");

    ASSERT_TRUE(platform::write_all(fd, proto::encode_json('O', 2, Json{{"argv", {"/bin/sh", "-c", "exit 3"}}, {"rows", 24}, {"cols", 80}})));
    const auto tty = read_until_exit_(fd, d, 2);
    ASSERT_TRUE(tty.code);
    EXPECT_EQ(*tty.code, 3);

    platform::send_signal(pid, platform::sig::kill);
    (void)platform::wait_process(pid, true);
    std::vector<int> rest{fd};
    platform::close_fds(rest);
}
