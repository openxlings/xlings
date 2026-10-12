// The operation journal (Luban OS architecture design §13): one file per
// operation, the processes it runs append to it under the trace they inherit,
// credentials never reach it, and `self log` reads it back -- the bundle
// without the home's paths or the user's name.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.core.xself.oplog;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::vector<fs::path> journals(const tk::Home& home) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(home.dir() / "logs" / "ops", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".ndjson") out.push_back(it->path());
    return out;
}

std::vector<Json> lines(const fs::path& f) {
    std::vector<Json> out;
    std::istringstream in(tk::read_file(f));
    for (std::string l; std::getline(in, l);)
        if (auto j = Json::parse(l, nullptr, false); j.is_object()) out.push_back(j);
    return out;
}

}  // namespace

XTEST(OpsJournal, AnOperationIsOneFileItsChildrenJoin, .area = "self", .covers = {"OPS-JOURNAL"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("ops-journal");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    auto made = home.xlings({"subos", "new", "probe", "--proxy", "socks5h://me:secret@127.0.0.1:1080"},
                            {{"XLINGS_TRACE_ID", "feedfacecafe0001"}});
    // Whatever the command answered, the journal has its start and its end.
    auto child = home.xlings({"subos", "list"}, {{"XLINGS_TRACE_ID", "feedfacecafe0001"}});
    std::optional<fs::path> mine;
    for (const auto& f : journals(home)) if (f.stem() == "feedfacecafe0001") mine = f;
    ASSERT_TRUE(mine) << "no journal for the trace";
    const auto ls = lines(*mine);
    ASSERT_FALSE(ls.empty());
    int ends = 0;
    for (const auto& l : ls) {
        EXPECT_EQ(l.value("trace", ""), "feedfacecafe0001");
        if (l.value("line", "") == "end") ++ends;
    }
    EXPECT_EQ(ends, 2) << "two processes of one operation";
    const auto text = tk::read_file(*mine);
    EXPECT_EQ(text.find("secret"), std::string::npos) << "a credential reached the journal";
    EXPECT_NE(text.find("***@127.0.0.1"), std::string::npos) << text;

    auto listed = home.xlings({"self", "log"});
    EXPECT_EQ(listed.exit_code, 0) << listed.transcript();
    EXPECT_NE(listed.out.find("feedfacecafe"), std::string::npos) << listed.transcript();
    auto shown = home.xlings({"self", "log", "feedface"});
    EXPECT_EQ(shown.exit_code, 0) << shown.transcript();
    EXPECT_NE(shown.out.find("subos new probe"), std::string::npos) << shown.transcript();
    const auto bundleFile = home.root() / "op.bundle.ndjson";
    auto bundled = home.xlings({"self", "log", "feedface", "--bundle", bundleFile.string()});
    EXPECT_EQ(bundled.exit_code, 0) << bundled.transcript();
    EXPECT_EQ(tk::read_file(bundleFile).find(home.dir().string()), std::string::npos) << "the home's path is in the bundle";
}

XTEST(OpsJournal, CredentialsAndPathsAreTakenOut, .area = "self", .covers = {"OPS-JOURNAL"}) {
    namespace ops = xlings::observe::ops;
    const std::vector<std::string> argv{"install", "--token", "abc", "--password=x", "https://u:p@host/x", "plain",
                                        "--env", "PLAIN=v", "--env=K=v2", "API_SECRET=s", "a=b"};
    const auto r = ops::redact_argv(argv);
    EXPECT_EQ(r, (std::vector<std::string>{"install", "--token", "***", "--password=***", "https://***@host/x", "plain",
                                           "--env", "PLAIN=***", "--env=K=***", "API_SECRET=***", "a=b"}));
    const auto b = xlings::xself::redact_for_bundle("/home/someone/.xlings/x /home/someone/y someone", "/home/someone/.xlings",
                                                    "/home/someone", "someone");
    EXPECT_EQ(b, "$XLINGS_HOME/x ~/y user");  // privacy-ok: the fixture of the redaction itself
}
