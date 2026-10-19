#include <gtest/gtest.h>
import xlings.testkit;
import xlings.testkit.fixture;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import xlings.core.xim.downloader;
import xlings.core.xim.libxpkg.types.type;
import xlings.platform;

namespace tk = xlings::testkit;

namespace {
nlohmann::json payload(std::string_view transcript, std::string_view kind) {
    std::istringstream lines{std::string(transcript)};
    std::string line;
    while (std::getline(lines, line)) {
        auto event = nlohmann::json::parse(line, nullptr, false);
        if (event.is_object() && event.value("kind", "") == "data"
            && event.value("dataKind", "") == kind) return event["payload"];
    }
    return nullptr;
}
}

XTEST(FirstRunNetwork, SocketCapsNeverExpandTheCallersBudget, .area = "cli",
      .covers = {"NET-SOCKET-CAPS"}) {
    using xlings::tinyhttps::bounded_timeout_seconds;
    EXPECT_EQ(bounded_timeout_seconds(30, "8"), 8);
    EXPECT_EQ(bounded_timeout_seconds(3, "8"), 3);
    for (const auto* invalid : {"0", "-1", "off", "8seconds", "99999999999999999999"})
        EXPECT_EQ(bounded_timeout_seconds(30, invalid), 30);
}

XTEST(FirstRunNetwork, EndpointDiscoveryIsLocalAndKeepsEveryDeclaredArtifactBase, .area = "cli",
      .covers = {"NET-ENDPOINTS"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("network-endpoints");
    tk::write_file(home.dir() / ".xlings.json", R"({"mirror":"auto","index_repos":[{
        "name":"corp","url":"https://example.invalid/corp.git","artifact":{
        "GLOBAL":"https://example.invalid/resources/corp",
        "CN":"https://mirror.invalid/resources/corp"}}]})");
    const auto result = home.xlings({"interface", "network_endpoints", "--args", "{}"}, {},
                                    std::chrono::seconds(5));
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    const auto data = payload(result.transcript(), "network_endpoints");
    ASSERT_TRUE(data.is_object()) << result.transcript();
    std::set<std::string> found;
    for (const auto& endpoint : data.at("endpoints"))
        if (endpoint.at("resource") == "corp") found.insert(endpoint.at("url"));
    EXPECT_EQ(found, (std::set<std::string>{
        "https://example.invalid/resources/corp/corp-pointers.json",
        "https://mirror.invalid/resources/corp/corp-pointers.json"}));
    EXPECT_FALSE(std::filesystem::exists(home.dir() / "data" / "xim-pkgindex"));
}

XTEST(FirstRunNetwork, ProbeReportsProxyFailureWithoutPrintingSecrets, .area = "cli",
      .covers = {"NET-PROBE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("network-probe");
    auto result = home.xlings({"interface", "probe_url", "--args",
        R"({"url":"https://example.invalid/resource","socket_timeout_ms":200})"},
        {{"HTTPS_PROXY", "http://127.0.0.1:9"}, {"https_proxy", "http://127.0.0.1:9"},
         {"NO_PROXY", ""}, {"no_proxy", ""}}, std::chrono::seconds(5));
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    auto data = payload(result.transcript(), "network_probe");
    ASSERT_TRUE(data.is_object()) << result.transcript();
    EXPECT_FALSE(data.at("reachable").get<bool>());
    EXPECT_TRUE(data.at("proxy").get<bool>());
    EXPECT_EQ(data.at("host"), "example.invalid");
    EXPECT_FALSE(data.at("status").get<std::string>().empty());

    result = home.xlings({"interface", "probe_url", "--args",
        R"({"url":"https://user:probe-secret@example.invalid/path?token=probe-secret"})"}, {},
        std::chrono::seconds(5));
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    data = payload(result.transcript(), "network_probe");
    ASSERT_TRUE(data.is_object()) << result.transcript();
    EXPECT_EQ(data.at("host"), "example.invalid");
    EXPECT_EQ(result.transcript().find("probe-secret"), std::string::npos);
}

XTEST(FirstRunNetwork, ProbeReadsARealResourceAndDistinguishesAnHttpFailure, .area = "cli",
      .covers = {"NET-PROBE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("network-resource");
    const auto root = home.root() / "network-fixture";
    tk::write_file(root / "pointer.json", "{\"indexes\":{}}\n");
    auto server = tk::fixture::Server::start(root);
    ASSERT_TRUE(server) << server.error();
    for (const auto* path : {"pointer.json", "missing.json"}) {
        const auto args = nlohmann::json{{"url", server->url() + "/" + path}, {"socket_timeout_ms", 500}};
        const auto response = home.xlings({"interface", "probe_url", "--args", args.dump()}, {},
                                         std::chrono::seconds(5));
        ASSERT_EQ(response.exit_code, 0) << response.transcript();
        const auto data = payload(response.transcript(), "network_probe");
        ASSERT_TRUE(data.is_object()) << response.transcript();
        const bool available = std::string_view(path) == "pointer.json";
        EXPECT_EQ(data.at("reachable").get<bool>(), available);
        EXPECT_EQ(data.at("http_status").get<int>(), available ? 200 : 404);
    }
}

XTEST(FirstRunNetwork, OnlyIncreasingBytesRenewTheIdleBudget, .area = "cli",
      .covers = {"NET-SOCKET-CAPS"}) {
    xlings::tinyhttps::TransferBudget idle(30, 120);
    idle.progress(20, 1);
    EXPECT_FALSE(idle.expired(49));
    idle.progress(49, 1); // heartbeat or a duplicate progress sample
    EXPECT_TRUE(idle.expired(50));
    idle.progress(50, 2);
    EXPECT_FALSE(idle.expired(79));
    for (int t = 60; t < 120; t += 10) idle.progress(t, t);
    EXPECT_TRUE(idle.expired(120)); // continued bytes cannot renew total time
    xlings::tinyhttps::TransferBudget disabled(0, 0);
    EXPECT_FALSE(disabled.expired(100000));
}

XTEST(FirstRunNetwork, EveryCandidateAttemptIsReportedBeforeTransfer, .area = "cli",
      .covers = {"NET-SOCKET-CAPS"}) {
    auto home = tk::Home::isolated("network-source-attempt");
    xlings::tinyhttps::DownloadOptions opts;
    opts.destFile = home.root() / "downloaded.bin";
    opts.urls = {"https://first.invalid/file", "https://second.invalid/file"};
    opts.retryCount = 0;
    std::vector<std::string> attempts;
    opts.onUrlAttempt = [&](std::string_view url) { attempts.emplace_back(url); };
    opts.transferOverride = [&](const std::string& url, const std::filesystem::path& file) {
        EXPECT_FALSE(attempts.empty());
        EXPECT_EQ(attempts.back(), url);
        if (url == opts.urls.front()) return xlings::tinyhttps::DownloadFileResult{
            .success = false, .error = "HTTP 404", .failure = xlings::tinyhttps::FailureKind::Source};
        tk::write_file(file, "payload");
        return xlings::tinyhttps::DownloadFileResult{.success = true, .bytesWritten = 7};
    };
    const auto result = xlings::tinyhttps::download_file(opts);
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(attempts, opts.urls);
    EXPECT_EQ(result.sourceUrl, opts.urls.back());
}

XTEST(FirstRunNetwork, DiagnosticCapabilitiesLeaveTheHomeUnchanged, .area = "cli",
      .covers = {"NET-ENDPOINTS", "NET-PROBE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("network-readonly");
    auto snapshot = [&] {
        std::map<std::filesystem::path, std::string> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(home.dir()))
            files.emplace(entry.path().lexically_relative(home.dir()),
                entry.is_regular_file() ? tk::read_file(entry.path()) : "<directory>");
        return files;
    };
    const auto before = snapshot();
    for (const auto* capability : {"network_endpoints", "probe_url", "system_status", "payload_status"}) {
        const auto args = std::string_view(capability) == "probe_url"
            ? R"({"url":"http://127.0.0.1:9/missing","socket_timeout_ms":100})"
            : std::string_view(capability) == "payload_status" ? R"({"targets":[]})" : "{}";
        const auto result = home.xlings({"interface", capability, "--args", args}, {},
                                       std::chrono::seconds(5));
        if (std::string_view(capability) == "payload_status") {
            // A cold home has no catalog to resolve even a status request.
            // Its local refusal must still leave that home untouched.
            ASSERT_EQ(result.exit_code, 1) << result.transcript();
            EXPECT_NE(result.out.find("E_NOT_FOUND"), std::string::npos) << result.transcript();
        } else {
            ASSERT_EQ(result.exit_code, 0) << capability << result.transcript();
        }
        EXPECT_EQ(snapshot(), before) << "a diagnostic initialized, migrated or logged into the home";
    }
}

XTEST(FirstRunNetwork, AVerifiedCacheHitStartsNoMetadataConnection, .area = "cli",
      .covers = {"NET-SOCKET-CAPS"}) {
    auto home = tk::Home::isolated("network-cache-reuse");
    auto listener = xlings::platform::LoopbackListener::open();
    ASSERT_TRUE(listener) << listener.error();
    std::atomic<int> connections{0};
    std::jthread proxy([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto accepted = listener->accept(std::chrono::milliseconds(20));
            if (!accepted) return;
            if (*accepted) ++connections; // close immediately; no external network
        }
    });
    struct Environment {
        std::map<std::string, std::string> saved;
        void set(std::string name, std::string value) {
            const auto* old = std::getenv(name.c_str());
            saved.emplace(name, old ? old : "");
            xlings::platform::set_env_variable(name, value);
        }
        ~Environment() {
            for (const auto& [key, value] : saved) xlings::platform::set_env_variable(key, value);
        }
    } environment;
    const auto proxyUrl = std::format("http://127.0.0.1:{}", listener->port());
    environment.set("HTTPS_PROXY", proxyUrl);
    environment.set("NO_PROXY", "");
    if constexpr (!xlings::platform::is_windows) {
        environment.set("https_proxy", proxyUrl);
        environment.set("no_proxy", "");
    }
    // Positive control: the metadata client really reaches this proxy.
    EXPECT_EQ(xlings::tinyhttps::query_content_length("https://resource.invalid/payload.bin", 1), -1);
    ASSERT_GT(connections.load(), 0);
    connections.store(0);
    const auto cache = home.root() / "cache";
    tk::write_file(cache / "payload.bin", "payload");
    const auto digest = xlings::sha256::hex_file(cache / "payload.bin");
    ASSERT_TRUE(digest);
    xlings::xim::DownloadTask task;
    task.name = "cached";
    task.url = "https://resource.invalid/payload.bin";
    task.destDir = cache;
    task.sha256 = *digest;
    const std::array tasks{task};
    const auto result = xlings::xim::download_all(tasks, {}, nullptr, nullptr);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].success) << result[0].error;
    EXPECT_EQ(tk::read_file(cache / "payload.bin"), "payload");
    EXPECT_EQ(connections.load(), 0) << "a verified local cache hit made an unnecessary HEAD request";
}
