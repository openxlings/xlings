// Running commands in an instance from outside it (C12, design §12): the
// exec surface, its exit-code table, hot sessions, throwaway instances, file
// copies, and the interface capability agents use.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("exec");
    Box() {
        home.seed_sandbox_backend();
        auto r = home.xlings({"subos", "new", "box"});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }
    tk::RunResult exec(std::vector<std::string> args) const {
        std::vector<std::string> a{"subos", "exec"};
        a.insert(a.end(), args.begin(), args.end());
        return home.xlings(a);
    }
};

}  // namespace

XTEST(SubosExec, ArgvIsPassedVerbatimAndTheExitCodeIsTheCommands,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F16", "EXIT-CMD"},
      .requires_ = {"posix", "xlings-bin"}) {
    Box box;
    // No shell between us and the command: the space and the quote arrive as
    // one argument each, untouched.
    auto r = box.exec({"box", "--", "/bin/sh", "-c", "printf '[%s]' \"$1\"; exit 5", "sh",
                       "a b 'c'"});
    EXPECT_EQ(r.exit_code, 5) << r.transcript();
    EXPECT_EQ(r.out, "[a b 'c']");
    EXPECT_EQ(r.err.find("entering"), std::string::npos) << "exec announces nothing";
}

XTEST(SubosExec, TheExitCodeTable,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"EXIT-125", "EXIT-126", "EXIT-127", "EXIT-124", "EXIT-SIGNAL"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    EXPECT_EQ(box.exec({"nosuch", "--", "true"}).exit_code, 125) << "no such instance";
    EXPECT_EQ(box.exec({"box", "--sandbox", "--", "no-such-program"}).exit_code, 127);
    EXPECT_EQ(box.exec({"box", "--sandbox", "--", "/etc/passwd"}).exit_code, 126);
    EXPECT_EQ(box.exec({"box", "--sandbox", "--timeout", "1", "--", "sleep", "30"}).exit_code, 124);
    EXPECT_EQ(box.exec({"box", "--sandbox", "--", "/bin/sh", "-c", "kill -TERM $$"}).exit_code,
              128 + 15);
    auto j = box.exec({"box", "--sandbox", "--json", "--", "/bin/sh", "-c", "exit 3"});
    EXPECT_EQ(j.exit_code, 3);
    auto last = j.err.substr(j.err.rfind('{'));
    auto result = nlohmann::json::parse(last, nullptr, false);
    ASSERT_FALSE(result.is_discarded()) << j.transcript();
    EXPECT_EQ(result["exit"], 3);
    EXPECT_EQ(result["mode"], "sandbox");
}

XTEST(SubosExec, StartRunsAHotSessionThatExecJoinsAndStopEnds,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"SES-START-STOP", "SES-JOIN"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto s = box.home.xlings({"subos", "start", "box", "--ttl", "60"});
    ASSERT_EQ(s.exit_code, 0) << s.transcript();
    // Two commands, one session: what the first leaves in /tmp the second sees.
    EXPECT_EQ(box.exec({"box", "--", "/bin/sh", "-c", "echo hot > /tmp/state"}).exit_code, 0);
    auto r = box.exec({"box", "--", "cat", "/tmp/state"});
    EXPECT_EQ(r.exit_code, 0) << r.transcript();
    EXPECT_EQ(r.out, "hot\n");
    EXPECT_EQ(box.home.xlings({"subos", "stop", "box"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(box.home.dir() / "run" / "subos" / "box" / "session.json"));
}

XTEST(SubosExec, ATemporaryInstanceGoesAndItsAuditStays,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"SES-TEMP"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto r = box.exec({"--temp", "--sandbox", "--json", "--", "/bin/sh", "-c", "echo t; exit 2"});
    EXPECT_EQ(r.exit_code, 2) << r.transcript();
    auto result = nlohmann::json::parse(r.err.substr(r.err.rfind('{')), nullptr, false);
    ASSERT_FALSE(result.is_discarded()) << r.transcript();
    const auto name = result.value("instance", "");
    ASSERT_TRUE(name.starts_with("tmp-")) << name;
    EXPECT_FALSE(fs::exists(box.home.dir() / "subos" / name));
    EXPECT_TRUE(fs::exists(box.home.dir() / "logs" / "subos" / name / "events.ndjson"));
    auto list = box.home.xlings({"subos", "list"});
    EXPECT_EQ(list.out.find(name), std::string::npos) << "unregistered too";
}

XTEST(SubosExec, CpCopiesIntoAndOutOfTheInstancesOwnTreesOnly,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"SES-CP"},
      .requires_ = {"xlings-bin"}) {
    Box box;
    auto host_file = box.home.root() / "in.txt";
    tk::write_file(host_file, "payload");
    auto in = box.home.xlings({"subos", "cp", host_file.string(), "box:/tmp/"});
    ASSERT_EQ(in.exit_code, 0) << in.transcript();
    EXPECT_EQ(tk::read_file(box.home.dir() / "subos" / "box" / "tmp" / "in.txt"), "payload");

    auto out_file = box.home.root() / "out.txt";
    auto out = box.home.xlings({"subos", "cp", "box:/tmp/in.txt", out_file.string()});
    ASSERT_EQ(out.exit_code, 0) << out.transcript();
    EXPECT_EQ(tk::read_file(out_file), "payload");

    auto refused = box.home.xlings({"subos", "cp", host_file.string(), "box:/etc/passwd"});
    EXPECT_NE(refused.exit_code, 0);
}

XTEST(SubosExec, TheInterfaceRunsArgvAndReturnsTheExitCode,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"F16"},
      .requires_ = {"posix", "xlings-bin"}) {
    Box box;
    nlohmann::json args{{"name", "box"}, {"argv", {"/bin/sh", "-c", "echo from-iface; exit 3"}}};
    auto r = box.home.xlings({"interface", "subos_exec", "--args", args.dump()});
    std::string output;
    std::optional<int> exitCode;
    for (auto& line : r.json_lines()) {
        if (line.value("kind", "") == "data" && line.value("dataKind", "") == "subos_exec_output")
            output += line["payload"].value("data", "");
        if (line.value("kind", "") == "result") exitCode = line.value("exitCode", -1);
    }
    EXPECT_NE(output.find("from-iface"), std::string::npos) << r.transcript();
    ASSERT_TRUE(exitCode.has_value()) << r.transcript();
    EXPECT_EQ(*exitCode, 3);
}
