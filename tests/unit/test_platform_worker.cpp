#include <gtest/gtest.h>

import std;
import xlings.platform;
import xlings.platform.worker;
import xlings.testkit;

namespace fs = std::filesystem;
namespace tk = xlings::testkit;
namespace worker = xlings::platform::worker;
namespace platform = xlings::platform;

TEST(PlatformWorker, RecipeCaptureIncludesBothStreamsAndExecChildren) {
    if constexpr (!tk::is_posix)
        GTEST_SKIP() << "fork-based child output test is POSIX only";
    auto home = tk::Home::isolated("worker-capture");
    const auto log = home.root() / "recipe.log";
    int code = -1;
    {
        worker::OutputCapture capture(log);
        std::cout << "recipe stdout\n";
        std::cerr << "recipe stderr\n";
        const auto pid = platform::fork_process();
        if (pid == 0) {
            (void)platform::exec_path(
                {"/bin/sh", "-c", "printf child_stdout; printf child_stderr >&2"}, home.env());
            platform::exit_now(127);
        }
        if (auto status = platform::wait_process(pid, true))
            code = platform::exit_code(*status);
    }
    EXPECT_EQ(code, 0);
    const auto text = tk::read_file(log);
    EXPECT_NE(text.find("recipe stdout"), std::string::npos);
    EXPECT_NE(text.find("recipe stderr"), std::string::npos);
    EXPECT_NE(text.find("child_stdout"), std::string::npos);
    EXPECT_NE(text.find("child_stderr"), std::string::npos);
    const auto next = home.root() / "next.log";
    {
        worker::OutputCapture capture(next);
        std::cout << "restored and recaptured\n";
    }
    EXPECT_EQ(tk::read_file(log), text);
    EXPECT_EQ(tk::read_file(next), "restored and recaptured\n");
}

TEST(PlatformWorker, RefusesNonRegularLogsAndRestoresStreamsAfterCaptureFailure) {
    auto home = tk::Home::isolated("worker-log-type");
    const auto directory = home.root() / "directory";
    fs::create_directory(directory);
    EXPECT_FALSE(worker::prepare_log(directory));
    EXPECT_THROW(worker::OutputCapture{directory}, std::runtime_error);
    const auto regular = home.root() / "regular.log";
    EXPECT_TRUE(worker::prepare_log(regular));
    {
        worker::OutputCapture capture(regular);
        std::cerr << "stderr still works\n";
    }
    EXPECT_EQ(tk::read_file(regular), "stderr still works\n");
    if constexpr (tk::is_posix) {
        const auto sentinel = home.root() / "user-file";
        tk::write_file(sentinel, "never truncate");
        const auto link = home.root() / "link.log";
        fs::create_symlink(sentinel, link);
        EXPECT_FALSE(worker::prepare_log(link));
        EXPECT_FALSE(worker::read_log(home.root(), "link.log"));
        EXPECT_EQ(tk::read_file(sentinel), "never truncate");
        fs::remove(link);
        fs::create_symlink(home.root() / "missing-user-file", link);
        EXPECT_FALSE(worker::prepare_log(link));
        EXPECT_FALSE(fs::exists(home.root() / "missing-user-file"));
    }
}

TEST(PlatformWorker, OwnedLogReadsRefuseTraversalAndNonRegularFiles) {
    auto home = tk::Home::isolated("worker-log-read");
    tk::write_file(home.root() / "output.log", "diagnostic output");
    const auto text = worker::read_log(home.root(), "output.log");
    ASSERT_TRUE(text) << text.error();
    EXPECT_EQ(*text, "diagnostic output");
    EXPECT_FALSE(worker::read_log(home.root(), "../output.log"));
    EXPECT_FALSE(worker::read_log(home.root(), "nested/output.log"));
    fs::create_directory(home.root() / "directory.log");
    EXPECT_FALSE(worker::read_log(home.root(), "directory.log"));
}
