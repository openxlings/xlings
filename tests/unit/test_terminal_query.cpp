// Unit tests for the OSC-11 terminal background-color query path.
//
// Regression coverage for issue #368: on terminals whose OSC-11 reply is
// slow (Tabby/Electron/xterm.js) or fragmented across reads, the old
// "single 50ms select + single read" implementation gave up early, restored
// ECHO, and let the terminal reply leak into the parent shell's input.
//
// The fix reframes the reply as a byte stream read until a deterministic
// DSR/CPR fence (or a monotonic deadline). Two pure, fd-based helpers are
// extracted so the logic is testable without a controlling tty:
//
//   parse_terminal_bg_is_light(buf)      — parse an accumulated reply buffer
//   read_terminal_query_reply(fd, timeout) — framed read until CPR 'R'/deadline
//
// The read loop is exercised over a socketpair whose far end plays the
// terminal: replying immediately, after >50ms, fragmented, or not at all.
#include <gtest/gtest.h>

// POSIX-only: exercises socketpair(2) and the POSIX-only helpers
// (parse_terminal_bg_is_light / read_terminal_query_reply). On Windows the
// background query goes through windows.cppm's own stub, so there is nothing
// to test here.
#if !defined(_WIN32)

#include <sys/socket.h>
#include <unistd.h>

import std;
import xlings.platform;

using xlings::platform::parse_terminal_bg_is_light;
using xlings::platform::read_terminal_query_reply;

namespace {

// A complete OSC-11 reply for the given 8-bit gray value, ST-terminated,
// followed by a CPR (cursor position report) that acts as the fence.
std::string osc_reply_st(int gray8) {
    char hx[3];
    std::snprintf(hx, sizeof(hx), "%02x", gray8 & 0xff);
    std::string h4 = std::string(hx) + hx;  // 8-bit 0xNN -> 16-bit 0xNNNN
    return "\033]11;rgb:" + h4 + "/" + h4 + "/" + h4 + "\033\\" + "\033[2;1R";
}

// Same, but BEL-terminated OSC reply.
std::string osc_reply_bel(int gray8) {
    char hx[3];
    std::snprintf(hx, sizeof(hx), "%02x", gray8 & 0xff);
    std::string h4 = std::string(hx) + hx;
    return "\033]11;rgb:" + h4 + "/" + h4 + "/" + h4 + "\a" + "\033[1;1R";
}

void write_all(int fd, std::string_view s) {
    while (!s.empty()) {
        auto n = ::write(fd, s.data(), s.size());
        if (n <= 0) break;
        s.remove_prefix(static_cast<std::size_t>(n));
    }
}

}  // namespace

// ── Pure parser ────────────────────────────────────────────────

TEST(TerminalQueryParse, DarkBackground) {
    // 0x17 gray (Tabby's #171717) -> luma well below the mid threshold.
    auto r = parse_terminal_bg_is_light("\033]11;rgb:1717/1717/1717\033\\");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r);  // dark
}

TEST(TerminalQueryParse, LightBackground) {
    auto r = parse_terminal_bg_is_light("\033]11;rgb:eeee/eeee/eeee\033\\");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);  // light
}

TEST(TerminalQueryParse, BelTerminator) {
    auto r = parse_terminal_bg_is_light("\033]11;rgb:ffff/ffff/ffff\a");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
}

TEST(TerminalQueryParse, LeadingGarbageBeforeReply) {
    // Unrelated bytes before the OSC reply must not defeat parsing.
    auto r = parse_terminal_bg_is_light("junk\033[2;1R\033]11;rgb:0000/0000/0000\033\\");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r);
}

TEST(TerminalQueryParse, MalformedHexRejected) {
    EXPECT_FALSE(parse_terminal_bg_is_light("\033]11;rgb:zzzz/0000/0000\033\\").has_value());
}

TEST(TerminalQueryParse, NoRgbRejected) {
    EXPECT_FALSE(parse_terminal_bg_is_light("\033[2;1R").has_value());
    EXPECT_FALSE(parse_terminal_bg_is_light("").has_value());
}

TEST(TerminalQueryParse, TruncatedReplyRejected) {
    // "rgb:" present but not enough channel bytes follow.
    EXPECT_FALSE(parse_terminal_bg_is_light("\033]11;rgb:1717/17").has_value());
}

// ── Framed read loop over a socketpair ─────────────────────────

class FramedRead : public ::testing::Test {
protected:
    int rd() const { return sv[0]; }  // our (reader) end
    int wr() const { return sv[1]; }  // terminal (writer) end

    void SetUp() override {
        ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    }
    void TearDown() override {
        if (sv[0] >= 0) ::close(sv[0]);
        if (sv[1] >= 0) ::close(sv[1]);
    }
    int sv[2] { -1, -1 };
};

TEST_F(FramedRead, ImmediateCompleteReply) {
    write_all(wr(), osc_reply_st(0x17));
    auto buf = read_terminal_query_reply(rd(), std::chrono::milliseconds{500});
    auto r = parse_terminal_bg_is_light(buf);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(*r);
}

TEST_F(FramedRead, DelayedReplyPast50ms) {
    std::string payload = osc_reply_st(0xee);
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        write_all(wr(), payload);
    });
    auto buf = read_terminal_query_reply(rd(), std::chrono::milliseconds{800});
    t.join();
    auto r = parse_terminal_bg_is_light(buf);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);  // light — old 50ms impl would have missed this
}

TEST_F(FramedRead, FragmentedAcrossReads) {
    std::thread t([&] {
        write_all(wr(), "\033]11;rgb:");
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        write_all(wr(), "eeee/eeee");
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        write_all(wr(), "/eeee\033\\");
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        write_all(wr(), "\033[2;1R");  // CPR fence arrives last
    });
    auto buf = read_terminal_query_reply(rd(), std::chrono::milliseconds{800});
    t.join();
    auto r = parse_terminal_bg_is_light(buf);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
}

TEST_F(FramedRead, BelTerminatedReply) {
    write_all(wr(), osc_reply_bel(0xff));
    auto buf = read_terminal_query_reply(rd(), std::chrono::milliseconds{500});
    auto r = parse_terminal_bg_is_light(buf);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);
}

TEST_F(FramedRead, StopsAtCprFence) {
    // Everything the terminal sends is consumed up to and including the CPR,
    // so nothing is left to leak back into the shell.
    write_all(wr(), osc_reply_st(0x40));
    auto buf = read_terminal_query_reply(rd(), std::chrono::milliseconds{500});
    EXPECT_NE(buf.find('R'), std::string::npos);  // consumed through CPR
}

TEST_F(FramedRead, NoResponseReturnsWithinDeadline) {
    // Terminal answers neither OSC-11 nor DSR: must return bounded, not hang.
    auto t0 = std::chrono::steady_clock::now();
    auto buf = read_terminal_query_reply(rd(), std::chrono::milliseconds{150});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT_LT(elapsed.count(), 600);  // bounded by the deadline (+slack)
    EXPECT_FALSE(parse_terminal_bg_is_light(buf).has_value());
}

#endif  // !defined(_WIN32)
