#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.platform;
import xlings.xdev.fixture;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fixture = xlings::xdev::fixture;
namespace platform = xlings::platform;

namespace {
std::expected<std::string, std::string> fetch(std::uint16_t port, std::string request) {
    auto connection = platform::TcpStream::connect_loopback(port);
    if (!connection) return std::unexpected(connection.error());
    auto sent = connection->send_all(request, std::chrono::seconds(2));
    if (!sent) return std::unexpected(sent.error());
    std::string response;
    std::array<char, 4096> bytes{};
    for (;;) {
        auto read = connection->receive(bytes, std::chrono::seconds(2));
        if (!read) return std::unexpected(read.error());
        if (*read == 0) return response;
        response.append(bytes.data(), *read);
    }
}
}

XTEST(XdevFixture, RealLoopbackServerServesExactBytesAndStopsWithItsOwner,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE"}) {
    auto home = tk::Home::isolated("http-fixture");
    const std::string payload{"payload\0binary", 14};
    tk::write_file(home.root() / "fixture/payload.bin", payload);
    auto server = fixture::Server::start(home.root() / "fixture");
    ASSERT_TRUE(server) << server.error();
    const auto port = server->port();
    EXPECT_NE(port, 0);
    EXPECT_EQ(server->url(), "http://127.0.0.1:" + std::to_string(port));
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto response = fetch(port, "GET /payload.bin HTTP/1.1\r\nHost: localhost\r\n\r\n");
        ASSERT_TRUE(response) << response.error();
        const auto header = response->find("\r\n\r\n");
        ASSERT_NE(header, std::string::npos);
        EXPECT_TRUE(response->starts_with("HTTP/1.1 200 OK\r\n"));
        EXPECT_EQ(response->substr(header + 4), payload);
        EXPECT_NE(response->find("Content-Length: 14\r\n"), std::string::npos);
    }
    const auto head = fetch(port, "HEAD /payload.bin HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(head) << head.error();
    EXPECT_TRUE(head->ends_with("\r\n\r\n"));
    EXPECT_NE(head->find("Content-Length: 14\r\n"), std::string::npos);
    server->stop();
    EXPECT_FALSE(platform::TcpStream::connect_loopback(port, std::chrono::milliseconds(200)));
}

XTEST(XdevFixture, RejectsTraversalMalformedPathsAndSymlinkEscapes,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE"}) {
    auto home = tk::Home::isolated("http-boundary");
    tk::write_file(home.root() / "fixture/ok.txt", "ok");
    tk::write_file(home.root() / "secret.txt", "private");
    const auto root = home.root() / "fixture";
    EXPECT_EQ(fixture::respond(root, "GET /ok.txt?query=1 HTTP/1.1\r\n\r\n").body, "ok");
    for (const auto* url : {"/../secret.txt", "/%2e%2e/secret.txt", "/%2Fetc/passwd", "/bad%00name",
                            "/bad%xxname", "/C:%5csecret.txt", "/%5csecret.txt"})
        EXPECT_EQ(fixture::respond(root, std::string("GET ") + url + " HTTP/1.1\r\n\r\n").status, 403) << url;
    EXPECT_EQ(fixture::respond(root, "POST /ok.txt HTTP/1.1\r\n\r\n").status, 405);
    EXPECT_EQ(fixture::respond(root, "GET /ok.txt nonsense\r\n\r\n").status, 400);
    EXPECT_EQ(fixture::respond(root, "GET /absent.txt HTTP/1.1\r\n\r\n").status, 404);
    std::error_code ec;
    std::filesystem::create_symlink(home.root() / "secret.txt", root / "escape.txt", ec);
    if (!ec) EXPECT_EQ(fixture::respond(root, "GET /escape.txt HTTP/1.1\r\n\r\n").status, 403);
}

XTEST(XdevFixture, ConcurrentOwnersReserveDifferentPortsAndSlowRequestsCannotBlockShutdown,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE"}) {
    auto home = tk::Home::isolated("http-lifecycle");
    auto first = fixture::Server::start(home.root());
    auto second = fixture::Server::start(home.root());
    ASSERT_TRUE(first) << first.error();
    ASSERT_TRUE(second) << second.error();
    EXPECT_NE(first->port(), second->port());
    auto slow = platform::TcpStream::connect_loopback(first->port());
    ASSERT_TRUE(slow) << slow.error();
    ASSERT_TRUE(slow->send_all("GET /", std::chrono::seconds(1)));
    const auto started = std::chrono::steady_clock::now();
    first->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    const auto response = fetch(second->port(), "GET /missing HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(response) << response.error();
    EXPECT_TRUE(response->starts_with("HTTP/1.1 404 Not Found\r\n"));
}


XTEST(XdevFixture, CliChildConsumesAnExplicitLoopbackFixture,
      .area = "testkit", .resources = {"port:8080"}) {
    if (!std::getenv("XDEV_HTTP_CHILD")) return;
    const auto* environment = std::getenv("XLINGS_TEST_FIXTURE_URL");
    ASSERT_NE(environment, nullptr);
    const std::string_view url{environment};
    constexpr std::string_view PREFIX { "http://127.0.0.1:" };
    ASSERT_TRUE(url.starts_with(PREFIX));
    unsigned port { 0 };
    const auto number = url.substr(PREFIX.size());
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), port);
    ASSERT_EQ(error, std::errc{});
    ASSERT_EQ(end, number.data() + number.size());
    ASSERT_LE(port, 65535);
    const auto response = fetch(static_cast<std::uint16_t>(port), "GET /payload.txt HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(response) << response.error();
    EXPECT_TRUE(response->ends_with("fixture-child-payload"));
}

XTEST(XdevFixture, CliRunsArtifactWorkersWithFixtureUrlAndMergesTheirRecords,
      .area = "testkit", .covers = {"TEST-HTTP-FIXTURE", "CI-RESOURCE-LOCKS"}) {
    const auto* binary = std::getenv("XDEV_BIN");
    if (!binary || !*binary) GTEST_SKIP() << "set XDEV_BIN to the built xdev";
    auto home = tk::Home::isolated("fixture-cli");
    tk::write_file(home.root() / "mcpp.toml", "[package]\nname = \"fixture-cli\"\n");
    tk::write_file(home.root() / "fixture/payload.txt", "fixture-child-payload");
    std::string discovery;
    const std::string child { "XdevFixture.CliChildConsumesAnExplicitLoopbackFixture" };
    for (const auto* name : {"a", "b"}) {
        const auto source = "tests/unit/" + std::string(name) + ".cpp";
        tk::write_file(home.root() / source, "import std;");
        auto destination = home.root() / "target/fixture/bin/unit" / name;
        if constexpr (tk::is_windows) destination += ".exe";
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(platform::get_executable_path(), destination);
        nlohmann::json row{{"member", ""}, {"test", "unit/" + std::string(name)}, {"main", source},
            {"cases", {{{"test", child}, {"area", "testkit"}, {"resources", {"port:8080"}}}}}};
        discovery += row.dump() + "\n";
    }
    const auto input = home.root() / "discovery.ndjson";
    tk::write_file(input, discovery);
    const auto out = home.root() / "run";
    auto env = tk::inherited_env();
    env["XDEV_HTTP_CHILD"] = "1";
    // Child invocations must not append to this program's own result streams.
    env.erase("XTEST_META_OUT");
    env.erase("XTEST_RESULTS_OUT");
    const auto run = tk::run({.argv = {binary, "test", "--no-build", "--discovery", input.string(),
        "-j", "2", "--fixture", (home.root() / "fixture").string(), "--out", out.string(),
        "--", "--", "--gtest_filter=" + child}, .env = std::move(env), .cwd = home.root(),
        .timeout = std::chrono::seconds(30)});
    ASSERT_EQ(run.exit_code, 0) << run.transcript();
    const auto report = nlohmann::json::parse(tk::read_file(out / "report.json"));
    int programs { 0 }, cases { 0 };
    for (const auto& row : report["records"]) {
        if (row["kind"] == "binary") { ++programs; EXPECT_EQ(row["status"], "pass"); }
        if (row["kind"] == "case") { ++cases; EXPECT_EQ(row["status"], "pass"); }
    }
    EXPECT_EQ(programs, 2);
    EXPECT_EQ(cases, 2);
    EXPECT_TRUE(std::filesystem::is_regular_file(out / "workers/0/cases.ndjson"));
    EXPECT_TRUE(std::filesystem::is_regular_file(out / "workers/1/cases.ndjson"));
}
