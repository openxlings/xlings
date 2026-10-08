#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.platform.stream;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace ps = xlings::platform::stream;

namespace {
struct FixtureProcess {
    FixtureProcess() {
        const auto* gate = std::getenv("XLINGS_STREAM_TEST_GATE");
        if (!gate || !*gate) return;
        std::fwrite("EARLY-OUT", 1, 9, stdout);
        std::fflush(stdout);
        std::fwrite("EARLY-ERR", 1, 9, stderr);
        std::fflush(stderr);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!std::filesystem::exists(gate)) {
            if (std::chrono::steady_clock::now() >= until) std::exit(99);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const std::string binary(65536, '\xff');
        std::fwrite(binary.data(), 1, binary.size(), stdout);
        std::fflush(stdout);
        std::fwrite(binary.data(), 1, binary.size(), stderr);
        std::fflush(stderr);
        std::exit(7);
    }
} fixture;

struct Gate {
    std::string key, prior;
    explicit Gate(const std::filesystem::path& path) : Gate("XLINGS_STREAM_TEST_GATE", path.string()) {}
    Gate(std::string name, std::string value) : key(std::move(name)) {
        if (const auto* old = std::getenv(key.c_str())) prior = old;
        xlings::platform::set_env_variable(key, value);
    }
    ~Gate() {
        if (prior.empty()) xlings::platform::unset_env_variable(key);
        else xlings::platform::set_env_variable(key, prior);
    }
};
}

XTEST(PlatformStream, RealSubosInterfaceFramesEarlyStreamsAndBinaryChunks,
      .area = "interface", .covers = {"IFACE-EXEC-STREAM"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("interface-stream");
    const auto created = home.xlings({"subos", "new", "box"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto gate_file = home.root() / "release-interface";
    const nlohmann::json arguments{{"name", "box"},
        {"argv", {xlings::platform::get_executable_path().string()}},
        {"env", {{"XLINGS_STREAM_TEST_GATE", gate_file.string()}}}};
    const auto args_file = home.root() / "arguments.json";
    tk::write_file(args_file, arguments.dump());
    Gate selected_home{"XLINGS_HOME", home.dir().string()};
    Gate project{"XLINGS_PROJECT_DIR", ""};
    Gate subos{"XLINGS_ACTIVE_SUBOS", ""};
    const auto original = std::filesystem::current_path();
    struct Cwd {
        std::filesystem::path previous;
        ~Cwd() { std::error_code ignored; std::filesystem::current_path(previous, ignored); }
    } cwd{original};
    std::filesystem::current_path(home.root());
    std::string pending, outer_errors;
    bool early_out{}, early_err{}, binary{}, final{};
    int result_code{};
    const auto code = ps::run({tk::xlings_binary().string(), "interface", "subos_exec", "--args-file", args_file.string()},
        [&](std::string_view channel, std::string_view bytes) {
            if (channel == "stderr") { outer_errors.append(bytes); return; }
            pending.append(bytes);
            for (std::size_t newline; (newline = pending.find('\n')) != pending.npos;) {
                const auto event = nlohmann::json::parse(pending.substr(0, newline), nullptr, false);
                pending.erase(0, newline + 1);
                ASSERT_TRUE(event.is_object());
                if (event.value("kind", "") == "result") {
                    final = true;
                    result_code = event.value("exitCode", 0);
                }
                if (event.value("dataKind", "") != "subos_exec_output") continue;
                EXPECT_FALSE(final);
                const auto& payload = event.at("payload");
                if (payload.value("data", "").find("EARLY-OUT") != std::string::npos)
                    early_out = payload.value("stream", "") == "stdout";
                if (payload.value("data", "").find("EARLY-ERR") != std::string::npos)
                    early_err = payload.value("stream", "") == "stderr";
                binary = binary || payload.value("encoding", "") == "base64";
                if (early_out && early_err && !std::filesystem::exists(gate_file))
                    tk::write_file(gate_file, "the wire delivered both streams before completion");
            }
        });
    EXPECT_EQ(code, 7) << outer_errors;
    EXPECT_TRUE(early_out);
    EXPECT_TRUE(early_err);
    EXPECT_TRUE(binary);
    EXPECT_TRUE(final);
    EXPECT_EQ(result_code, 7);
    EXPECT_TRUE(pending.empty());
}

XTEST(PlatformStream, SeparateByteStreamsArriveBeforeExitWithoutWaitingForNewlines,
      .area = "platform", .covers = {"IFACE-EXEC-STREAM"}) {
    auto home = tk::Home::isolated("stream-output");
    const auto gate_file = home.root() / "release gate with spaces";
    Gate gate{gate_file};
    std::string out, err;
    std::size_t maximum{};
    const auto code = ps::run({xlings::platform::get_executable_path().string()},
        [&](std::string_view channel, std::string_view bytes) {
            maximum = std::max(maximum, bytes.size());
            (channel == "stdout" ? out : err).append(bytes);
            if (out.starts_with("EARLY-OUT") && err.starts_with("EARLY-ERR"))
                tk::write_file(gate_file, "release child only after receiving both streams");
        });
    EXPECT_EQ(code, 7);
    EXPECT_EQ(out, "EARLY-OUT" + std::string(65536, '\xff'));
    EXPECT_EQ(err, "EARLY-ERR" + std::string(65536, '\xff'));
    EXPECT_LE(maximum, 4096);
}

XTEST(PlatformStream, CancellationReapsAChildBlockedAfterItsFirstOutput,
      .area = "platform", .covers = {"IFACE-EXEC-STREAM"}) {
    auto home = tk::Home::isolated("stream-cancel");
    Gate gate{home.root() / "never-release"};
    const auto began = std::chrono::steady_clock::now();
    bool early{};
    const auto code = ps::run({xlings::platform::get_executable_path().string()},
        [&](std::string_view, std::string_view bytes) { early = early || bytes.starts_with("EARLY-"); },
        [&] { return early; });
    EXPECT_TRUE(early);
    EXPECT_EQ(code, 130);
    EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds(2));
}

XTEST(PlatformStream, InvalidArgvAndMissingExecutablesHaveStableExitCodes,
      .area = "platform", .covers = {"IFACE-EXEC-STREAM"}) {
    const auto output = [](std::string_view, std::string_view) {};
    EXPECT_EQ(ps::run({}, output), 125);
    EXPECT_EQ(ps::run({std::string{"invalid\0argument", 16}}, output), 125);
    EXPECT_EQ(ps::run({"xlings-test-missing-stream-program-641"}, output), 127);
}
