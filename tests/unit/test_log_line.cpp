// A log line is one line, written once.
//
// It used to be two or three separate writes -- prefix, optional context tag,
// message -- with no lock, while download worker threads and a 200ms progress
// redraw wrote to the same console. Anything landing between them stranded the
// `[xlings] ` prefix at the start of somebody else's line, which is what
// "换行很奇怪总是前面有缩进" describes: weird line breaks with a stray indent.
//
// These tests capture the real stdout/stderr through a file, so they assert
// what a terminal would receive rather than what an injected sink was told.
#include <gtest/gtest.h>

#include <cstdio>

// The POSIX descriptor calls, spelled once per platform. MSVC has them all
// under underscore-prefixed names in <io.h>; the semantics are the same.
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#define FILENO(s)      ::_fileno(s)
#define DUP(fd)        ::_dup(fd)
#define DUP2(a, b)     ::_dup2((a), (b))
#define CLOSE(fd)      ::_close(fd)
#define OPEN_WRITE(p)  ::_open((p), _O_WRONLY | _O_TRUNC | _O_BINARY)
#else
#include <unistd.h>
#include <fcntl.h>
#define FILENO(s)      ::fileno(s)
#define DUP(fd)        ::dup(fd)
#define DUP2(a, b)     ::dup2((a), (b))
#define CLOSE(fd)      ::close(fd)
#define OPEN_WRITE(p)  ::open((p), O_WRONLY | O_TRUNC)
#endif

import std;
import xlings.core.log;

namespace {

struct CaptureFile {
    std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / std::format("xlings-log-capture-{}",
                      std::chrono::steady_clock::now().time_since_epoch().count());
    ~CaptureFile() { std::error_code ec; std::filesystem::remove(path, ec); }

    std::string read() const {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
};

// Redirect a stdio stream to a file for the duration of `body`, then put it
// back exactly as it was.
//
// At the FILE DESCRIPTOR level, via dup/dup2, not with freopen. freopen can
// redirect, but it cannot restore: putting the stream back means naming a
// destination, and there is no name that is right everywhere. `/dev/tty` does
// not exist on a CI runner, `CONOUT$` does not exist when Windows CI captures
// output to a pipe, and when the reattach fails every LATER test in this
// binary writes into the temp file -- taking gtest's own report with it. A
// saved descriptor is the original destination, whatever it was.
//
// Descriptors rather than a stringstream because the code under test writes to
// the FILE* directly, which is the entire point: a test that swapped in an
// iostream sink would exercise a path production does not take.
std::string capture(std::FILE* stream, const std::filesystem::path& path,
                    const std::function<void()>& body) {
    const int fd = FILENO(stream);
    std::fflush(stream);

    const int saved = DUP(fd);
    if (saved < 0) return {};

    {
        std::ofstream create(path, std::ios::trunc);
    }
    const int target = OPEN_WRITE(path.string().c_str());
    if (target < 0) { CLOSE(saved); return {}; }

    DUP2(target, fd);
    CLOSE(target);

    body();

    std::fflush(stream);
    DUP2(saved, fd);
    CLOSE(saved);

    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (const auto part : std::views::split(text, '\n')) {
        out.emplace_back(part.begin(), part.end());
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

}  // namespace

TEST(LogLine, SingleLineCarriesItsPrefix) {
    CaptureFile file;
    xlings::log::enable_color(false);
    xlings::log::clear_context();

    const auto out = capture(stdout, file.path, [] {
        xlings::log::info("hello {}", "world");
    });

    ASSERT_EQ(lines_of(out).size(), 1u);
    EXPECT_EQ(lines_of(out)[0], "[xlings] hello world");
}

// The reported symptom, tested directly: an embedded newline used to put the
// second line at column 0 with no prefix, so a multi-line message read as
// ragged unattributed text. Continuations are now aligned under the message.
//
// The downloader's mirror-candidate list is a real multi-line message and was
// hand-indenting with "\n    " to compensate. This is that behaviour, done in
// one place for every caller.
TEST(LogLine, ContinuationLinesAreIndentedToThePrefixWidth) {
    CaptureFile file;
    xlings::log::enable_color(false);
    xlings::log::clear_context();

    const auto out = capture(stdout, file.path, [] {
        xlings::log::info("candidates:\nhttps://a/x.tgz\nhttps://b/x.tgz");
    });

    const auto got = lines_of(out);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "[xlings] candidates:");
    // "[xlings] " is nine columns.
    EXPECT_EQ(got[1], "         https://a/x.tgz");
    EXPECT_EQ(got[2], "         https://b/x.tgz");
}

TEST(LogLine, ContextTagWidensTheIndent) {
    CaptureFile file;
    xlings::log::enable_color(false);
    xlings::log::set_context("gcc");

    const auto out = capture(stdout, file.path, [] {
        xlings::log::info("one\ntwo");
    });
    xlings::log::clear_context();

    const auto got = lines_of(out);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "[xlings] [gcc] one");
    EXPECT_EQ(got[1], "               two");
}

TEST(LogLine, WarningsGoToStderrAsOneLine) {
    CaptureFile file;
    xlings::log::enable_color(false);
    xlings::log::clear_context();

    const auto out = capture(stderr, file.path, [] {
        xlings::log::warn("careful: {}", 42);
    });

    ASSERT_EQ(lines_of(out).size(), 1u);
    EXPECT_EQ(lines_of(out)[0], "[warn] careful: 42");
}

// Interleaving is the actual defect, so it gets an actual concurrency test.
//
// Many threads logging at once must produce whole lines: every line either
// starts with a known prefix or is a continuation indent. A torn line -- a
// prefix with another thread's message welded onto it -- is what the lock
// exists to prevent, and what this fails on.
TEST(LogLine, ConcurrentLoggersDoNotTearEachOthersLines) {
    CaptureFile file;
    xlings::log::enable_color(false);
    xlings::log::clear_context();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 40;

    const auto out = capture(stdout, file.path, [] {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t] {
                for (int i = 0; i < kPerThread; ++i) {
                    xlings::log::info("thread-{}-message-{}", t, i);
                }
            });
        }
    });

    const auto got = lines_of(out);
    ASSERT_EQ(got.size(), static_cast<std::size_t>(kThreads * kPerThread));
    for (const auto& line : got) {
        ASSERT_TRUE(line.starts_with("[xlings] thread-"))
            << "torn line: " << line;
        // Exactly one prefix per line. Two means one write landed inside
        // another, which is the bug in its original form.
        EXPECT_EQ(line.find("[xlings]", 1), std::string::npos)
            << "two prefixes on one line: " << line;
    }
}

// A message that already ends in a newline must not gain a line of spaces.
//
// The indenter inserts the continuation indent after every '\n' it sees. On a
// trailing newline that produces a final line consisting only of spaces --
// invisible on a terminal, and trailing whitespace everywhere the output is
// captured. Found by re-reading the loop, not by any test failing.
TEST(LogLine, TrailingNewlineDoesNotProduceALineOfSpaces) {
    CaptureFile file;
    xlings::log::enable_color(false);
    xlings::log::clear_context();

    const auto out = capture(stdout, file.path, [] {
        xlings::log::info("done\n");
    });

    EXPECT_EQ(out, "[xlings] done\n");
}

// And the message keeps exactly one terminator either way.
TEST(LogLine, MessageWithoutNewlineGetsExactlyOne) {
    CaptureFile file;
    xlings::log::enable_color(false);
    xlings::log::clear_context();

    const auto out = capture(stdout, file.path, [] {
        xlings::log::info("done");
    });

    EXPECT_EQ(out, "[xlings] done\n");
}
