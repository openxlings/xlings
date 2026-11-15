// A state file is replaced while other processes read it (#715): the
// replacement does not fail because of a reader, and xlings' own readers
// never make it fail. On POSIX rename(2) never waits for a reader, so these
// pass trivially there; the Windows lane is where they prove something.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace p = xlings::platform;
using namespace std::chrono_literals;

namespace {

std::size_t staging_files(const fs::path& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().string().find(".xlings-tmp.") != std::string::npos) ++n;
    return n;
}

}  // namespace

// A reader that opened the file with FILE_SHARE_DELETE (as xlings' own
// readers do) keeps its handle and the old content; the name is replaced at
// once (POSIX-semantics rename on Windows -- a plain MoveFileExW is refused
// for as long as any handle is open).
XTEST(SharedReplace, AReaderSharingDeleteDoesNotStopTheReplacement,
      .area = "platform", .covers = {"ATOMIC-REPLACE-SHARED"}) {
    auto home = tk::Home::isolated("replace-shared-delete");
    const auto target = home.root() / ".xlings.json";
    tk::write_file(target, "old");
    auto held = tk::HeldFile::open(target, /*share_delete=*/true);
    ASSERT_TRUE(held);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(p::write_file_atomic(target.string(), "new"));
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_LT(took, 1500ms) << "the replacement waited for a reader that allows it";
    EXPECT_EQ(held->read_all(), "old") << "the reader keeps the file it opened";
    EXPECT_EQ(tk::read_file(target), "new");
    held->close();
    EXPECT_EQ(staging_files(home.root()), 0u);
}

// A reader that did not share delete (std::ifstream, _wfopen, another
// program) blocks the replacement on Windows while it holds the file; the
// replacement waits for it, within its bound, and then succeeds.
XTEST(SharedReplace, AReaderThatLetsGoInTimeIsWaitedFor,
      .area = "platform", .covers = {"ATOMIC-REPLACE-SHARED"}) {
    auto home = tk::Home::isolated("replace-waits");
    const auto target = home.root() / ".xlings.json";
    tk::write_file(target, "old");
    auto held = tk::HeldFile::open(target, /*share_delete=*/false);
    ASSERT_TRUE(held);
    std::thread releaser([&held] {
        std::this_thread::sleep_for(300ms);
        held->close();
    });
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(p::write_file_atomic(target.string(), "new"));
    const auto took = std::chrono::steady_clock::now() - started;
    releaser.join();
    EXPECT_EQ(tk::read_file(target), "new");
    if constexpr (tk::is_windows)
        EXPECT_GE(took, 200ms) << "the reader without FILE_SHARE_DELETE was expected to block the first attempt";
    EXPECT_EQ(staging_files(home.root()), 0u);
}

// A reader that never lets go: the replacement gives up after its bound,
// reports the failure, and leaves the previous content and no staging file.
XTEST(SharedReplace, AReaderThatNeverLetsGoFailsTheReplacementWithinItsBound,
      .area = "platform", .covers = {"ATOMIC-REPLACE-SHARED"}, .requires_ = {"windows"}) {
    auto home = tk::Home::isolated("replace-bound");
    const auto target = home.root() / ".xlings.json";
    tk::write_file(target, "old");
    auto held = tk::HeldFile::open(target, /*share_delete=*/false);
    ASSERT_TRUE(held);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(p::write_file_atomic(target.string(), "new"), std::runtime_error);
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_GE(took, 1500ms) << "gave up before its bound";
    EXPECT_LT(took, 15s) << "the bound is about 2 s";
    held->close();
    EXPECT_EQ(tk::read_file(target), "old");
    EXPECT_EQ(staging_files(home.root()), 0u);
}

// The #715 shape: one process rewrites the file while others read it in a
// loop through xlings' own reader. No write fails, and every read sees one
// whole version.
XTEST(SharedReplace, ConcurrentReadersAndAWriterNeverFail,
      .area = "platform", .covers = {"ATOMIC-REPLACE-SHARED"}) {
    auto home = tk::Home::isolated("replace-concurrent");
    const auto target = home.root() / ".xlings.json";
    const std::string a(4096, 'a');
    const std::string b(8192, 'b');
    tk::write_file(target, a);
    std::atomic<bool> done{false};
    std::atomic<int> badReads{0};
    std::atomic<int> failedReads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r)
        readers.emplace_back([&] {
            while (!done.load()) {
                auto text = p::read_file(target);
                if (!text) ++failedReads;
                else if (*text != a && *text != b) ++badReads;
            }
        });
    int failedWrites = 0;
    for (int i = 0; i < 200; ++i) {
        try { p::write_file_atomic(target.string(), i % 2 ? a : b); }
        catch (const std::exception&) { ++failedWrites; }
    }
    done = true;
    for (auto& t : readers) t.join();
    EXPECT_EQ(failedWrites, 0);
    EXPECT_EQ(badReads.load(), 0) << "a read saw a partial file";
    // A read may race a rename on Windows (the name briefly being replaced),
    // which is the reader's to retry; what matters here is the writer.
    ::testing::Test::RecordProperty("failed_reads", failedReads.load());
    EXPECT_EQ(staging_files(home.root()), 0u);
}

XTEST(SharedReplace, ReadFileReadsWholeOrAPrefixAndNamesAMissingFile,
      .area = "platform", .covers = {"ATOMIC-REPLACE-SHARED"}) {
    auto home = tk::Home::isolated("read-file");
    const auto file = home.root() / "f";
    tk::write_file(file, "0123456789");
    EXPECT_EQ(p::read_file(file).value_or("?"), "0123456789");
    EXPECT_EQ(p::read_file(file, 4).value_or("?"), "0123");
    const auto missing = p::read_file(home.root() / "absent");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error(), std::make_error_code(std::errc::no_such_file_or_directory));
    EXPECT_THROW((void)p::read_file_to_string((home.root() / "absent").string()), std::runtime_error);
}
