// Local stream sockets and the lock a process holds while it lives (design
// 2026-10-19 §3.2, §3.3, §3.7): what the Luban OS tray and its windows speak
// over, on Linux, macOS and Windows.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace platform = xlings::platform;
using namespace std::chrono_literals;

namespace {

// A short directory: a socket path has ~100 bytes.
fs::path short_dir(std::string_view name) {
    const auto dir = fs::temp_directory_path() / std::format("xl-{}-{}", name, platform::current_pid());
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Whether a socket file is there: by its directory's listing (Windows'
// AF_UNIX socket is a reparse point that stat cannot open).
bool present(const fs::path& path) {
    std::error_code ec;
    for (fs::directory_iterator it(path.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() == path.filename()) return true;
    return false;
}

std::string read_line(platform::LocalStream& s) {
    std::string got;
    std::array<char, 256> buf{};
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!got.ends_with('\n') && std::chrono::steady_clock::now() < until) {
        auto n = s.poll_receive(buf, 200ms);
        if (!n || (*n && **n == 0)) break;
        if (*n) got.append(buf.data(), **n);
    }
    return got;
}

}  // namespace

XTEST(PlatformLocalSocket, TwoEndsTalkAndAClosedEndIsSeen, .area = "platform", .covers = {"LUBAN-OS-TRAY-PROCESS"}) {
    const auto dir = short_dir("sock");
    const auto path = dir / "t.sock";
    auto listener = platform::LocalListener::listen(path);
    ASSERT_TRUE(listener.has_value()) << listener.error();
    EXPECT_TRUE(present(path));
    if constexpr (tk::is_posix) {
        const auto perms = fs::status(path).permissions();
        EXPECT_EQ(perms & (fs::perms::group_all | fs::perms::others_all), fs::perms::none) << "owner-only";
    }
    auto client = platform::LocalStream::connect(path);
    ASSERT_TRUE(client.has_value()) << client.error();
    auto accepted = listener->accept(2s);
    ASSERT_TRUE(accepted.has_value() && accepted->has_value()) << (accepted ? "timed out" : accepted.error());
    auto server = std::move(**accepted);

    ASSERT_TRUE(client->send_all("{\"hello\":1}\n", 2s).has_value());
    const std::array<std::intptr_t, 2> handles{listener->native(), server.native()};
    const auto ready = platform::wait_readable(handles, 2s);
    ASSERT_EQ(ready, std::vector<std::size_t>{1}) << "the stream, not the listener";
    EXPECT_EQ(read_line(server), "{\"hello\":1}\n");
    ASSERT_TRUE(server.send_all("{\"raise\":{}}\n", 2s).has_value());
    EXPECT_EQ(read_line(*client), "{\"raise\":{}}\n");

    std::array<char, 16> buf{};
    auto quiet = server.poll_receive(buf, 50ms);
    ASSERT_TRUE(quiet.has_value());
    EXPECT_FALSE(quiet->has_value()) << "nothing sent: the wait expires";
    client->close();
    auto ended = server.poll_receive(buf, 2s);
    ASSERT_TRUE(ended.has_value() && ended->has_value());
    EXPECT_EQ(**ended, 0u) << "the other end closed";

    listener->close();
    EXPECT_FALSE(present(path)) << "the socket file goes with its listener";
    EXPECT_FALSE(platform::LocalStream::connect(path, 200ms).has_value());
    fs::remove_all(dir);
}

XTEST(PlatformLocalSocket, ALeftSocketFileIsReplacedAndALongPathIsSaid, .area = "platform", .covers = {"LUBAN-OS-TRAY-PROCESS"}) {
    const auto dir = short_dir("stale");
    const auto path = dir / "t.sock";
    // A listener that crashed leaves its socket file behind; the next one (it
    // holds the lock that says the old one is gone) listens there anyway.
    std::ofstream(path) << "stale";
    auto again = platform::LocalListener::listen(path);
    ASSERT_TRUE(again.has_value()) << again.error();
    EXPECT_TRUE(platform::LocalStream::connect(path).has_value());
    const auto longPath = dir / std::string(platform::local_socket_path_max() + 10, 'x');
    auto tooLong = platform::LocalListener::listen(longPath);
    ASSERT_FALSE(tooLong.has_value());
    EXPECT_NE(tooLong.error().find("longer than"), std::string::npos) << tooLong.error();
    fs::remove_all(dir);
}

XTEST(PlatformLocalSocket, ALockIsHeldByOneAndFreedWithIt, .area = "platform", .covers = {"LUBAN-OS-TRAY-PROCESS"}) {
    const auto dir = short_dir("lock");
    const auto path = dir / "tray.lock";
    std::string error;
    platform::FileLock held;
    ASSERT_TRUE(held.try_acquire(path, error)) << error;
    platform::FileLock other;
    EXPECT_FALSE(other.try_acquire(path, error));
    EXPECT_TRUE(error.empty()) << "held by another is not an error: " << error;
    held.release();
    EXPECT_TRUE(other.try_acquire(path, error)) << error;
    other.release();
    fs::remove_all(dir);
}
