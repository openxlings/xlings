// Observability (C22, design §22): what happened in an instance, recorded by
// the supervisor outside it, read back through log / report / the interface,
// and never carrying a secret's value.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {

struct Box {
    tk::Home home = tk::Home::isolated("observe");
    Box() {
        home.seed_sandbox_backend();
        auto r = home.xlings({"subos", "new", "box"});
        if (r.exit_code != 0) ADD_FAILURE() << r.transcript();
    }
    std::vector<nlohmann::json> log(std::string kind) const {
        return home.xlings({"subos", "log", "box", "--json", "--kind", kind, "-n", "1000"}).json_lines();
    }
};

}  // namespace

XTEST(SubosObserve, EveryProgramALockedSandboxRunsIsRecorded,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"OBS-EXEC"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"},
      .proves = "isolation") {
    Box box;
    auto r = box.home.xlings({"subos", "exec", "box", "--sandbox=locked", "--", "/bin/sh", "-c",
                              "ls / >/dev/null; uname >/dev/null; echo done"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    std::vector<std::string> paths;
    for (auto& e : box.log("exec")) paths.push_back(e.value("path", ""));
    auto ran = [&](std::string_view tail) {
        return std::ranges::any_of(paths, [&](auto& p) { return p.ends_with(tail); });
    };
    EXPECT_TRUE(ran("/sh")) << nlohmann::json(paths).dump();
    EXPECT_TRUE(ran("/ls")) << nlohmann::json(paths).dump();
    EXPECT_TRUE(ran("/uname")) << nlohmann::json(paths).dump();

    auto report = box.home.xlings({"subos", "report", "box", "--json"});
    auto j = nlohmann::json::parse(report.out, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << report.transcript();
    ASSERT_FALSE(j["sessions"].empty());
    EXPECT_GE(j["sessions"].back()["executions"].get<int>(), 3);
    EXPECT_TRUE(j["sessions"].back()["exec_traced"].get<bool>());
}

XTEST(SubosObserve, FilesChangedInAReadWriteMountAreListed,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"OBS-FS", "OBS-LOG"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    auto work = box.home.root() / "work";
    fs::create_directories(work);
    tk::write_file(work / "untouched", "x");
    ASSERT_EQ(box.home.xlings({"subos", "config", "box", "--observe", "standard"}).exit_code, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto r = box.home.xlings({"subos", "exec", "box", "--mount", work.string() + ":/work", "--",
                              "/bin/sh", "-c", "echo new > /work/made; mkdir -p /work/d; echo x > /work/d/f"});
    ASSERT_EQ(r.exit_code, 0) << r.transcript();
    auto fs_events = box.log("fs");
    ASSERT_FALSE(fs_events.empty());
    auto files = fs_events.back()["files"];
    std::set<std::string> got;
    for (auto& f : files) got.insert(f.get<std::string>());
    EXPECT_TRUE(got.contains("made")) << files.dump();
    EXPECT_TRUE(got.contains("d/f")) << files.dump();
    EXPECT_FALSE(got.contains("untouched")) << files.dump();
    auto report = box.home.xlings({"subos", "report", "box"});
    EXPECT_NE(report.out.find("files changed in rw mounts"), std::string::npos) << report.out;
}

XTEST(SubosObserve, TheInterfaceReadsTheAudit,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"OBS-IFACE-EVENTS"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    ASSERT_EQ(box.home.xlings({"subos", "exec", "box", "--sandbox", "--", "true"}).exit_code, 0);
    auto r = box.home.xlings({"interface", "subos_events", "--args",
                              R"({"name":"box","kind":"lifecycle"})"});
    int events = 0;
    for (auto& line : r.json_lines()) {
        if (line.value("dataKind", "") != "subos_event") continue;
        ++events;
        EXPECT_EQ(line["payload"]["kind"], "lifecycle");
    }
    EXPECT_GE(events, 2) << r.transcript();
}

XTEST(SubosObserve, TheAuditHasNamesNeverValues,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"OBS-REDACT"},
      .requires_ = {"linux", "xlings-bin", "sandbox"}, .resources = {"sandbox"}) {
    Box box;
    ASSERT_EQ(box.home.xlings({"subos", "config", "box", "--env-pass", "GITHUB_TOKEN"}).exit_code, 0);
    const std::map<std::string, std::string> env{{"GITHUB_TOKEN", "ghp-do-not-record-me"}};
    ASSERT_EQ(box.home.xlings({"subos", "start", "box"}).exit_code, 0);
    auto r = box.home.xlings({"subos", "exec", "box", "--env", "API_SECRET=also-not-me", "--",
                              "/bin/sh", "-c", "test -n \"$GITHUB_TOKEN\" && echo passed-in"}, env);
    (void)box.home.xlings({"subos", "stop", "box"});
    EXPECT_NE(r.out.find("passed-in"), std::string::npos) << r.transcript();
    std::string all;
    for (auto it = fs::recursive_directory_iterator(box.home.dir() / "logs");
         it != std::default_sentinel; ++it)
        if (it->is_regular_file()) all += tk::read_file(it->path());
    EXPECT_NE(all.find("GITHUB_TOKEN"), std::string::npos) << "the name is recorded";
    EXPECT_EQ(all.find("ghp-do-not-record-me"), std::string::npos);
    EXPECT_EQ(all.find("also-not-me"), std::string::npos);
}
