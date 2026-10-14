// The guest agent on a relay's standard input and output (luban
// __agent-stdio), reached through luban.vm's Client over a child process:
// the path a PC's Luban OS window takes to the host machine's sandboxes.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import luban.vm;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {
std::optional<fs::path> luban_binary() {
    if (const char* forced = std::getenv("LUBAN_BIN"); forced && *forced) return fs::path(forced);
    const auto xlings = tk::xlings_binary();
    if (xlings.empty()) return std::nullopt;
    for (const auto& candidate : {xlings.parent_path() / "luban", xlings.parent_path() / "luban-tool" / "luban"})
        if (fs::is_regular_file(candidate)) return candidate;
    return std::nullopt;
}
}  // namespace

// What a PC's Luban OS window reaches the host machine's sandboxes through
// (part 4 §8.2): luban's agent on a relay's standard input and output, a
// command on pipes (its bytes as written) and one on a terminal, each with
// its exit code -- here with luban itself as the relay, as wsl.exe runs it there.
XTEST(LubanRelay, TheAgentOnARelaysStdioRunsCommandsOnPipesAndOnATerminal, .area = "luban",
      .covers = {"CARRIER-WSL2-LUBAN"}, .requires_ = {"linux", "xlings-bin"}) {
    const auto bin = luban_binary();
    if (!bin) GTEST_SKIP() << "no luban beside the xlings under test (mcpp build -p luban-tool, or LUBAN_BIN)";
    auto client = luban::vm::Client::through({bin->string(), "__agent-stdio"});
    ASSERT_TRUE(client) << client.error();
    std::map<std::uint32_t, std::string> out;
    std::map<std::uint32_t, int> code;
    bool up = false;
    const auto pump = [&](std::uint32_t until) {
        for (int i = 0; i < 200 && !(until ? code.contains(until) : up); ++i) {
            auto frames = client->poll(100);
            ASSERT_TRUE(frames) << "the relay ended";
            for (const auto& [type, body] : *frames) {
                if (type == 'H') up = true;
                if (type == 'D') out[body.first] += body.second;
                if (type == 'X') code[body.first] = Json::parse(body.second, nullptr, false).value("code", -1);
            }
        }
    };
    pump(0);
    ASSERT_TRUE(up) << "the agent did not say hello";
    ASSERT_TRUE(client->open_command(1, {"/bin/sh", "-c", "printf 'first\\n'; cat; exit 5"}, 0, 0, Json::object(), true));
    ASSERT_TRUE(client->send(1, "typed\n"));
    ASSERT_TRUE(client->end_input(1));
    pump(1);
    ASSERT_TRUE(code.contains(1));
    EXPECT_EQ(code[1], 5);
    EXPECT_EQ(out[1], "first\ntyped\n");
    ASSERT_TRUE(client->open_command(2, {"/bin/sh", "-c", "tty >/dev/null && echo on-a-terminal; exit 3"}, 24, 80, Json::object(), false));
    pump(2);
    ASSERT_TRUE(code.contains(2));
    EXPECT_EQ(code[2], 3);
    EXPECT_NE(out[2].find("on-a-terminal"), std::string::npos) << out[2];
}

