#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.platform;
import xlings.platform.loopback_http;
import xlings.testkit.fixture;

namespace tk = xlings::testkit;
namespace http = xlings::platform::loopback_http;
namespace platform = xlings::platform;
namespace fs = std::filesystem;

XTEST(LoopbackHttp, DownloadsExactBytesAndAttributesADestinationFailure,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE"}) {
    auto home = tk::Home::isolated("loopback-transfer");
    const std::string body{"payload\0binary", 14};
    tk::write_file(home.root() / "served/data.bin", body);
    auto server = tk::fixture::Server::start(home.root() / "served");
    ASSERT_TRUE(server) << server.error();
    std::vector<double> progress;
    const auto result = http::download(server->url() + "/data.bin", home.root() / "download",
        std::chrono::seconds(2), [&](double total, double received) {
            EXPECT_EQ(total, body.size()); progress.push_back(received);
        });
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(tk::read_file(home.root() / "download"), body);
    EXPECT_EQ(result.written, body.size());
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back(), body.size());
    const auto badDestination = http::download(server->url() + "/data.bin", home.root(), std::chrono::seconds(2));
    EXPECT_EQ(badDestination.failure, http::Failure::Local);
    const auto missing = http::download(server->url() + "/missing", home.root() / "download", std::chrono::seconds(2));
    EXPECT_EQ(missing.failure, http::Failure::Source);
    EXPECT_EQ(tk::read_file(home.root() / "download"), body);
}

XTEST(LoopbackHttp, RejectsExternalAddressesAndMalformedSourcesBeforeWriting,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE"}) {
    auto home = tk::Home::isolated("loopback-protocol");
    const auto destination = home.root() / "user-file";
    tk::write_file(destination, "keep");
    for (const auto* url : {"http://example.com/a", "http://localhost:80/a", "http://127.0.0.1:0/a",
                            "http://127.0.0.1:80@other/a", "http://127.0.0.1:80/a\r\nInjected: true"}) {
        const auto result = http::download(url, destination, std::chrono::seconds(1));
        EXPECT_EQ(result.failure, http::Failure::Source);
        EXPECT_EQ(tk::read_file(destination), "keep");
    }
    for (const auto* response : {
        "HTTP/1.1 302 Found\r\nLocation: https://example.com\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"}) {
        auto listener = platform::LoopbackListener::open();
        ASSERT_TRUE(listener);
        std::jthread owner([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                auto accepted = listener->accept(std::chrono::milliseconds(50));
                if (!accepted) return;
                if (!*accepted) continue;
                std::array<char, 1024> request{};
                (void)(**accepted).receive(request, std::chrono::seconds(1));
                (void)(**accepted).send_all(response, std::chrono::seconds(1));
                return;
            }
        });
        const auto result = http::download(std::format("http://127.0.0.1:{}/data", listener->port()),
            destination, std::chrono::seconds(2));
        EXPECT_EQ(result.failure, http::Failure::Source) << result.error;
        EXPECT_EQ(tk::read_file(destination), "keep");
    }
}

XTEST(LoopbackHttp, CancellationInterruptsAStalledBodyWithoutWaitingForTheDeadline,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE"}) {
    auto home = tk::Home::isolated("loopback-cancel");
    auto listener = platform::LoopbackListener::open();
    ASSERT_TRUE(listener);
    std::jthread owner([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto accepted = listener->accept(std::chrono::milliseconds(50));
            if (!accepted) return;
            if (!*accepted) continue;
            std::array<char, 1024> request{};
            (void)(**accepted).receive(request, std::chrono::seconds(1));
            (void)(**accepted).send_all("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", std::chrono::seconds(1));
            while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return;
        }
    });
    const auto start = std::chrono::steady_clock::now();
    const auto result = http::download(std::format("http://127.0.0.1:{}/data", listener->port()),
        home.root() / "download", std::chrono::seconds(30), {}, [&] {
            return std::chrono::steady_clock::now() - start > std::chrono::milliseconds(200);
        });
    EXPECT_EQ(result.failure, http::Failure::Cancelled) << result.error;
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}
