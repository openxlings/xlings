#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.root_mount;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace p = xlings::platform;
namespace rm = xlings::platform::root_mount;
using nlohmann::json;

namespace {
std::optional<p::Message> receive(int fd) {
    p::PollFd descriptor{fd};
    if (p::poll_fds(std::span(&descriptor, 1), 5000) <= 0) return std::nullopt;
    return p::receive_message(fd);
}
struct CaptureChild {
    CaptureChild() {
        const auto* value = std::getenv("XLINGS_TEST_ROOT_MOUNT_FD");
        if (!value || !*value) return;
        const int fd = std::atoi(value);
        const auto target = rm::capture();
        if (!target || !p::send_message(fd, "ready", *target)) std::_Exit(125);
        for (const int captured : *target) p::close_fd(captured);
        for (;;) {
            const auto packet = receive(fd);
            if (!packet) std::_Exit(125);
            if (packet->data == "stop") std::_Exit(0);
            const auto request = json::parse(packet->data);
            const fs::path path = request.at("path").get<std::string>();
            const auto content = tk::read_file(path);
            std::ofstream writer(path, std::ios::app);
            const bool writable = writer.good();
            writer.close();
            if (!p::send_message(fd, json{{"content", content}, {"writable", writable},
                {"exists", fs::exists(path)}}.dump())) std::_Exit(125);
        }
    }
} captureChild;

struct Session {
    int child{-1}, control{-1};
    std::array<int, 3> target{-1, -1, -1};
    ~Session() {
        if (control >= 0) { (void)p::send_message(control, "stop"); p::close_fd(control); }
        if (child > 0) { p::send_signal(child, p::sig::kill); (void)p::wait_process(child, true); }
        for (const int fd : target) if (fd >= 0) p::close_fd(fd);
    }
    json read(const fs::path& path) {
        if (!p::send_message(control, json{{"path", path.string()}}.dump())) throw std::runtime_error("send mount query");
        const auto reply = receive(control);
        if (!reply) throw std::runtime_error("mount query timed out");
        return json::parse(reply->data);
    }
};
}

XTEST(RootMount, UpdatesOnlyTheCapturedSessionAndRollsBackAPartialMountBatch,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"ROOT-NO-HOST"}, .requires_ = {"linux", "bwrap"}, .resources = {"sandbox"}, .proves = "isolation") {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux mount namespaces";
    if (const auto why = tk::probe("bwrap")) GTEST_SKIP() << *why;
    std::string backend;
    for (const auto& candidate : {std::string(std::getenv("XDEV_BWRAP") ? std::getenv("XDEV_BWRAP") : ""),
        std::string("/usr/lib/xlings/bwrap"), std::string("/usr/bin/bwrap"), std::string("/usr/local/bin/bwrap")}) {
        if (candidate.empty() || !fs::exists(candidate)) continue;
        if (tk::run({.argv = {candidate, "--ro-bind", "/", "/", "--", "/bin/true"}}).exit_code == 0) {
            backend = candidate; break;
        }
    }
    ASSERT_FALSE(backend.empty());
    auto home = tk::Home::isolated("root-mount");
    const auto source = home.root() / "payload";
    const auto point = home.root() / "private-mountpoint";
    fs::create_directories(point / "hidden-point");
    tk::write_file(source / "data", "checked payload");
    const auto pair = p::unix_pair();
    ASSERT_TRUE(pair);
    Session session;
    auto env = tk::inherited_env();
    env["XLINGS_TEST_ROOT_MOUNT_FD"] = std::to_string((*pair)[1]);
    const std::vector<std::string> argv{backend, "--unshare-user", "--ro-bind", "/", "/", "--proc", "/proc",
        "--die-with-parent", "--", p::get_executable_path().string()};
    session.child = p::fork_process();
    if (session.child == 0) {
        p::close_fd((*pair)[0]);
        p::set_inheritable((*pair)[1], true);
        (void)p::exec_program(argv, env);
        p::exit_now(125);
    }
    p::close_fd((*pair)[1]);
    ASSERT_GT(session.child, 0);
    session.control = (*pair)[0];
    auto ready = receive(session.control);
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->data, "ready");
    ASSERT_EQ(ready->fds.size(), 3);
    std::copy(ready->fds.begin(), ready->fds.end(), session.target.begin());

    // Both points existed during preflight. Publishing the first payload
    // hides the second point; the second mount fails and the first rolls back.
    const std::vector<rm::Binding> partial{{source, point}, {source, point / "hidden-point"}};
    const auto failed = rm::update(session.target, partial, {});
    ASSERT_FALSE(failed);
    EXPECT_FALSE(session.read(point / "data").at("exists").get<bool>());
    EXPECT_TRUE(fs::is_directory(point / "hidden-point"));

    const std::vector<rm::Binding> full{{source, point}};
    const auto applied = rm::update(session.target, full, {});
    ASSERT_TRUE(applied) << applied.error();
    const auto observed = session.read(point / "data");
    EXPECT_EQ(observed.at("content"), "checked payload");
    EXPECT_FALSE(observed.at("writable").get<bool>());
    EXPECT_FALSE(fs::exists(point / "data")) << "the host namespace never received the mount";
    const auto metadata = home.root() / "metadata";
    const auto metadataPoint = home.root() / "metadata-point";
    tk::write_file(metadata, "new metadata");
    tk::write_file(metadataPoint, "old metadata");
    const std::vector<rm::Binding> files{{metadata, metadataPoint}};
    ASSERT_TRUE(rm::update(session.target, files, {}));
    EXPECT_EQ(session.read(metadataPoint).at("content"), "new metadata");
    EXPECT_EQ(tk::read_file(metadataPoint), "old metadata");
    const std::array<fs::path, 2> removed{point, metadataPoint};
    ASSERT_TRUE(rm::update(session.target, {}, removed));
    EXPECT_FALSE(session.read(point / "data").at("exists").get<bool>());
    EXPECT_EQ(session.read(metadataPoint).at("content"), "old metadata");
}
