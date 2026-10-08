#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.core.home.domain_producer;
import xlings.core.elfread;
import xlings.subos.home_view;
import xlings.subos.manifest;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

XTEST(DomainProducer, InstallsAtLogicalPrefixInsideANamespaceAndPreservesHostData,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"DOM-BUILD-INSIDE", "DOM-LAYOUTS"}, .requires_ = {"linux", "bwrap", "xlings-bin"},
      .resources = {"sandbox"}, .proves = "isolation") {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux namespaces";
    if (const auto why = tk::probe("bwrap")) GTEST_SKIP() << *why;
    const auto executable = xlings::elfread::read(tk::xlings_binary());
    ASSERT_TRUE(executable) << "cannot read the candidate ELF executable";
    if (!executable->interpreter.empty())
        GTEST_SKIP() << "domain root runtime requires the static release candidate; the development client has an ELF interpreter";
    auto home = tk::Home::isolated("domain-producer");
    const auto hostSentinel = home.root() / "host-sentinel";
    tk::write_file(hostSentinel, "host must remain intact");
    const auto repo = home.root() / "index";
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    std::string recipe = R"LUA(
        package = { spec='1', name='domain-base', type='subos', archs={'x86_64','aarch64'},
            xpm={linux={['1.0.0']={}}} }
        import('xim.libxpkg.pkginfo')
        function install()
            local dir = pkginfo.install_dir()
            assert(dir:sub(1, 8) == '/xlings/', 'producer ran outside its logical prefix: '..dir)
            local r = assert(io.open(HOST_SENTINEL, 'r'))
            assert(r:read('*a') == 'host must remain intact'); r:close()
            local writer = io.open(HOST_SENTINEL, 'w')
            if writer then writer:close(); error('host root is writable') end
            os.mkdir(path.join(dir, 'bin'))
            io.writefile(path.join(dir, '.xlings.json'), '{"workspace":{},"subos_kind":"rootfs"}')
            io.writefile(path.join(dir, 'produced-prefix'), dir)
            return true
        end
    )LUA";
    recipe.replace(recipe.find("HOST_SENTINEL"), std::string("HOST_SENTINEL").size(), Json(hostSentinel.generic_string()).dump());
    recipe.replace(recipe.find("HOST_SENTINEL"), std::string("HOST_SENTINEL").size(), Json(hostSentinel.generic_string()).dump());
    tk::write_file(repo / "pkgs/d/domain-base.lua", recipe);
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    auto initialized = home.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();
    const auto before = tk::read_file(home.dir() / ".xlings.json");
    auto created = home.xlings({"subos", "new", "domain-root", "--rootfs", "--domain", "/xlings",
                               "--from", "fixture:domain-base@1.0.0"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto privateHome = home.dir() / "domains/xlings/private";
    EXPECT_EQ(tk::read_file(privateHome / "data/xpkgs/fixture-x-domain-base/1.0.0/produced-prefix"),
              "/xlings/data/xpkgs/fixture-x-domain-base/1.0.0");
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");
    const auto outerBefore = Json::parse(before);
    const auto outerAfter = Json::parse(tk::read_file(home.dir() / ".xlings.json"));
    for (const auto* key : {"versions", "workspace", "configured", "activeSubos"})
        EXPECT_EQ(outerAfter.value(key, Json{}), outerBefore.value(key, Json{}));
    EXPECT_TRUE(outerAfter["subos"].contains("domain-root"));
    auto status = home.xlings({"subos", "status", "domain-root", "--json"});
    ASSERT_EQ(status.exit_code, 0) << status.transcript();
    const auto state = Json::parse(status.out);
    EXPECT_EQ(state["root"]["kind"], "rootfs");
    EXPECT_TRUE(state["root"]["tree"].get<std::string>().starts_with("/xlings/subos/domain-root/"));
    EXPECT_NE(home.xlings({"subos", "use", "domain-root", "--shell=sh"}).exit_code, 0);
    const auto descriptor = Json::parse(tk::read_file(xlings::subos::HomeView{home.dir()}.instance_file("domain-root")));
    EXPECT_EQ(descriptor["prefix_domain"]["physical_home"], fs::canonical(privateHome).generic_string());
    EXPECT_EQ(descriptor["prefix_domain"]["logical_home"], "/xlings");
    auto scope = xlings::home::domain_producer::read_scope(home.dir(), "domain-root");
    ASSERT_TRUE(scope) << scope.error(); ASSERT_TRUE(*scope);
    EXPECT_EQ((**scope).producerInstance, privateHome / "subos/domain-root");
    // The actual owned /xlings/bin/xlings entry must still discover and probe
    // the host backend. A caller's home is not proof that bwrap is its shim.
    std::optional<xlings::home::domain_producer_source::Facade> backendSource;
    if ((**scope).domain.systemSource) {
        const auto stage = home.root() / "backend-source-view";
        ASSERT_TRUE(fs::create_directory(stage));
        auto prepared = xlings::home::domain_producer_source::prepare((**scope).domain, stage);
        ASSERT_TRUE(prepared) << prepared.error();
        backendSource.emplace(std::move(*prepared));
    }
    const std::vector<std::string> backendArguments{"self", "doctor", "--isolation", "--json"};
    auto backendCommand = xlings::home::domain_producer::command(
        (**scope).domain, backendArguments, std::nullopt,
        backendSource ? &*backendSource : nullptr, true);
    ASSERT_TRUE(backendCommand) << backendCommand.error();
    auto backendDiagnosis = tk::run({.argv = *backendCommand, .env = home.env(), .cwd = home.root()});
    ASSERT_EQ(backendDiagnosis.exit_code, 0) << backendDiagnosis.transcript();
    const auto backendReport = Json::parse(backendDiagnosis.out);
    ASSERT_TRUE(backendReport["ok"].get<bool>()) << backendDiagnosis.transcript();
    ASSERT_EQ(backendReport["backend"]["name"].get<std::string>(), "bwrap");
    EXPECT_FALSE(backendReport["bwrap"].empty());
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");
    const auto beforeSwitch = tk::read_file(home.dir() / ".xlings.json");
    auto switched = home.xlings({"interface", "switch_subos", "--args", Json{{"name", "domain-root"}}.dump()});
    EXPECT_NE(switched.exit_code, 0) << switched.transcript();
    EXPECT_EQ(tk::read_file(home.dir() / ".xlings.json"), beforeSwitch);
    const xlings::subos::HomeView producer{privateHome};
    const xlings::subos::HomeView outer{home.dir()};
    auto info = home.xlings({"subos", "info", "domain-root"});
    ASSERT_EQ(info.exit_code, 0) << info.transcript();
    EXPECT_NE(info.transcript().find("/xlings/subos/domain-root"), std::string::npos);

    auto configured = home.xlings({"subos", "config", "domain-root", "--observe", "basic", "--json"});
    ASSERT_EQ(configured.exit_code, 0) << configured.transcript();
    EXPECT_TRUE(fs::is_regular_file(producer.policy_file("domain-root")));
    EXPECT_FALSE(fs::exists(outer.policy_file("domain-root")));
    auto policy = home.xlings({"subos", "config", "domain-root", "--json"});
    ASSERT_EQ(policy.exit_code, 0) << policy.transcript();
    EXPECT_EQ(Json::parse(policy.out)["observe"]["level"], "basic");
    EXPECT_EQ(Json::parse(policy.out)["source"], "file");
    auto invalidObserve = home.xlings({"subos", "exec", "domain-root", "--observe", "unknown", "--", "/bin/true"});
    EXPECT_EQ(invalidObserve.exit_code, 125) << invalidObserve.transcript();
    EXPECT_NE(invalidObserve.transcript().find("--observe expects"), std::string::npos);

    // Explicit grants provide a shell and its exact loader dependencies; the
    // root still obtains no host directories implicitly.
    const auto shell = xlings::elfread::read("/bin/sh");
    ASSERT_TRUE(shell);
    std::set<fs::path> shellFiles{"/bin/sh"};
    if (!shell->interpreter.empty()) {
        shellFiles.insert(shell->interpreter);
        auto libraries = tk::run({.argv = {shell->interpreter, "--list", "/bin/sh"}, .env = home.env()});
        ASSERT_EQ(libraries.exit_code, 0) << libraries.transcript();
        std::istringstream tokens(libraries.out);
        std::string token;
        while (tokens >> token) if (token.starts_with('/')) shellFiles.insert(token);
    }
    std::vector<std::string> shellGrants;
    for (const auto& file : shellFiles) {
        ASSERT_TRUE(fs::is_regular_file(file)) << file;
        shellGrants.insert(shellGrants.end(), {"--mount", file.string() + ":" + file.string() + ":ro"});
    }
    const auto mountSource = home.root() / "domain-mount-source";
    tk::write_file(mountSource / "probe", "outer mount source");
    tk::write_file(privateHome / "domain-mount-source/probe", "wrong private mount source");
    for (const auto* spelling : {"./domain-mount-source", "~/domain-mount-source"}) {
        std::vector<std::string> args{"subos", "exec", "domain-root", "--mount",
            std::string(spelling) + ":/mnt/domain-probe:ro"};
        args.insert(args.end(), shellGrants.begin(), shellGrants.end());
        args.insert(args.end(), {"--", "/bin/sh", "-c",
            "uidSeen=0; capsSeen=0; while read -r key a b rest; do "
            "case \"$key\" in Uid:) [ \"$a:$b\" = 0:0 ] && uidSeen=1;; "
            "CapEff:) [ \"$a\" = 0000000000000000 ] && capsSeen=1;; esac; "
            "done < /proc/self/status; [ \"$uidSeen:$capsSeen\" = 1:1 ] || exit 73; "
            "IFS= read -r value < /mnt/domain-probe/probe || :; printf '%s|%s|%s' \"$value\" \"$1\" \"$2\"",
            "probe", "--mount", "~/command-literal:/unused:ro"});
        auto mounted = home.xlings(args);
        ASSERT_EQ(mounted.exit_code, 0) << mounted.transcript();
        EXPECT_EQ(mounted.out, "outer mount source|--mount|~/command-literal:/unused:ro");

        args = {"subos", "use", "domain-root", "--no-keep",
            "--mount=" + std::string(spelling) + ":/mnt/domain-probe:ro"};
        args.insert(args.end(), shellGrants.begin(), shellGrants.end());
        args.insert(args.end(), {"--cmd",
            "IFS= read -r value < /mnt/domain-probe/probe || :; printf 'use-mount:%s' \"$value\""});
        auto used = home.xlings(args);
        ASSERT_EQ(used.exit_code, 0) << used.transcript();
        EXPECT_NE(used.out.find("use-mount:outer mount source"), std::string::npos) << used.transcript();
        EXPECT_EQ(used.out.find("wrong private mount source"), std::string::npos);
    }

    auto doctor = home.xlings({"subos", "doctor", "domain-root", "--json"});
    const auto diagnosed = Json::parse(doctor.out);
    ASSERT_EQ(diagnosed["instances"].size(), 1u) << doctor.transcript();
    EXPECT_EQ(diagnosed["instances"][0]["instance"], "domain-root");
    EXPECT_NE(diagnosed.dump().find("/xlings/config/subos/domain-root/policy.json"), std::string::npos);
    {
        std::ofstream events(producer.logs_dir("domain-root") / "events.ndjson", std::ios::app);
        ASSERT_TRUE(events);
        events << Json{{"kind", "lifecycle"}, {"event", "domain-route-probe"}, {"session", "domain-session-probe"}}.dump() << '\n';
    }
    tk::write_file(outer.logs_dir("domain-root") / "events.ndjson",
                  Json{{"kind", "lifecycle"}, {"event", "outer-control-probe"}, {"session", "domain-session-probe"}}.dump() + "\n");
    auto logged = home.xlings({"subos", "log", "domain-root", "--json", "--session", "domain-session-probe"});
    ASSERT_EQ(logged.exit_code, 0) << logged.transcript();
    EXPECT_EQ(Json::parse(logged.out)["event"], "domain-route-probe");
    auto missingSession = home.xlings({"subos", "log", "domain-root", "--session", "--global"});
    EXPECT_NE(missingSession.exit_code, 0);
    EXPECT_NE(missingSession.transcript().find("missing value"), std::string::npos);


    const auto eventArguments = Json{{"name", "domain-root"}, {"session", "domain-session-probe"}}.dump();
    auto interfaceEvents = home.xlings({"interface", "subos_events", "--args", eventArguments});
    ASSERT_EQ(interfaceEvents.exit_code, 0) << interfaceEvents.transcript();
    std::istringstream eventLines(interfaceEvents.out);
    std::string eventLine;
    int dataCount = 0, resultCount = 0;
    while (std::getline(eventLines, eventLine)) {
        const auto event = Json::parse(eventLine);
        if (event["kind"] == "data") {
            ++dataCount;
            EXPECT_EQ(event["dataKind"], "subos_event");
            EXPECT_EQ(event["payload"]["event"], "domain-route-probe");
        } else if (event["kind"] == "result") {
            ++resultCount;
            EXPECT_EQ(event["exitCode"], 0);
            EXPECT_EQ(event["data"]["count"], 1);
        }
    }
    EXPECT_EQ(dataCount, 1);
    EXPECT_EQ(resultCount, 1);
    EXPECT_EQ(interfaceEvents.out.find("outer-control-probe"), std::string::npos);

    auto generations = home.xlings({"subos", "rollback", "domain-root", "--list"});
    EXPECT_EQ(generations.exit_code, 0) << generations.transcript();
    EXPECT_NE(generations.transcript().find("subos new --rootfs"), std::string::npos);
    const auto instanceBefore = tk::read_file(privateHome / "subos/domain-root/.xlings.json");
    const auto runtime = xlings::subos::manifest::parse(Json::parse(instanceBefore)).runtime;
    ASSERT_FALSE(runtime.empty());
    auto rebound = home.xlings({"subos", "runtime", runtime, "domain-root"});
    EXPECT_EQ(rebound.exit_code, 0) << rebound.transcript();
    EXPECT_EQ(tk::read_file(privateHome / "subos/domain-root/.xlings.json"), instanceBefore);

    struct StopOnExit {
        tk::Home& home;
        ~StopOnExit() { (void)home.xlings({"subos", "stop", "domain-root"}); }
    } stopOnExit{home};
    auto started = home.xlings({"subos", "start", "domain-root", "--ttl", "30s"});
    ASSERT_EQ(started.exit_code, 0) << started.transcript();
    EXPECT_TRUE(fs::is_regular_file(producer.run_dir("domain-root") / "session.json"));
    EXPECT_FALSE(fs::exists(outer.run_dir("domain-root") / "session.json"));
    const auto sessionBefore = tk::read_file(producer.run_dir("domain-root") / "session.json");
    for (int call = 0; call < 2; ++call) {
        const auto joined = home.xlings({"subos", "exec", "domain-root", "--", "/xlings/bin/xlings", "--version"});
        EXPECT_EQ(joined.exit_code, 0) << joined.transcript();
        EXPECT_NE(joined.out.find("xlings "), std::string::npos);
        EXPECT_EQ(tk::read_file(producer.run_dir("domain-root") / "session.json"), sessionBefore);
    }
    auto running = home.xlings({"subos", "ps", "domain-root", "--json"});
    ASSERT_EQ(running.exit_code, 0) << running.transcript();
    EXPECT_EQ(Json::parse(running.out)["instance"], "domain-root");
    auto stopped = home.xlings({"subos", "stop", "domain-root"});
    ASSERT_EQ(stopped.exit_code, 0) << stopped.transcript();
    EXPECT_FALSE(fs::exists(producer.run_dir("domain-root") / "session.json"));
    auto ended = home.xlings({"subos", "ps", "domain-root", "--json"});
    EXPECT_EQ(ended.exit_code, 0) << ended.transcript();
    EXPECT_TRUE(ended.out.empty()) << ended.transcript();

    const auto controlBefore = tk::read_file(outer.instance_file("domain-root"));
    auto unconfirmed = home.xlings({"subos", "remove", "domain-root"});
    EXPECT_EQ(unconfirmed.exit_code, 2) << unconfirmed.transcript();
    EXPECT_EQ(tk::read_file(outer.instance_file("domain-root")), controlBefore);
    auto copied = home.xlings({"subos", "cp", hostSentinel.string(), "domain-root:/tmp/copied"});
    ASSERT_EQ(copied.exit_code, 0) << copied.transcript();
    EXPECT_EQ(tk::read_file(privateHome / "subos/domain-root/root/tmp/copied"), "host must remain intact");
    const auto copiedOut = home.root() / "copied-out";
    auto retrieved = home.xlings({"subos", "cp", "domain-root:/tmp/copied", copiedOut.string()});
    ASSERT_EQ(retrieved.exit_code, 0) << retrieved.transcript();
    EXPECT_EQ(tk::read_file(copiedOut), "host must remain intact");
    EXPECT_NE(home.xlings({"subos", "cp", hostSentinel.string(), "domain-root:/xlings/forbidden"}).exit_code, 0);
    fs::create_directory_symlink(hostSentinel.parent_path(), privateHome / "subos/domain-root/root/tmp/escape");
    EXPECT_NE(home.xlings({"subos", "cp", hostSentinel.string(), "domain-root:/tmp/escape/host-sentinel"}).exit_code, 0);
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");
    auto forked = home.xlings({"subos", "new", "domain-sibling", "--from", "domain-root"});
    ASSERT_EQ(forked.exit_code, 0) << forked.transcript();
    EXPECT_EQ(tk::read_file(privateHome / "subos/domain-sibling/root/tmp/copied"), "host must remain intact");
    auto sibling = xlings::home::domain_producer::read_scope(home.dir(), "domain-sibling");
    ASSERT_TRUE(sibling) << sibling.error(); ASSERT_TRUE(*sibling);
    EXPECT_EQ((**sibling).domain.physicalHome, (**scope).domain.physicalHome);
    EXPECT_NE(home.xlings({"subos", "new", "cross-domain-fork", "--from", "domain-root", "--rootfs", "--domain", "/other-prefix"}).exit_code, 0);
    EXPECT_FALSE(fs::exists(home.dir() / "subos/cross-domain-fork"));
    const auto output = home.root() / "export.tar.gz";
    tk::write_file(output, "user-owned output");
    auto exported = home.xlings({"subos", "export", "domain-root", "--tar", output.string()});
    EXPECT_NE(exported.exit_code, 0);
    EXPECT_EQ(tk::read_file(output), "user-owned output");
    auto collision = home.xlings({"subos", "new", "domain-root", "--rootfs", "--domain", "/xlings", "-y"});
    EXPECT_NE(collision.exit_code, 0) << collision.transcript();
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");
    auto failed = home.xlings({"subos", "new", "failed-domain-root", "--rootfs", "--domain", "/xlings",
                              "--from", "fixture:missing-base@1.0.0"});
    EXPECT_NE(failed.exit_code, 0) << failed.transcript();
    EXPECT_FALSE(fs::exists(home.dir() / "subos/failed-domain-root"));
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");
    tk::write_file(outer.instance("domain-root") / "user-owned", "preserve foreign control data");
    EXPECT_NE(home.xlings({"subos", "remove", "domain-root", "-y"}).exit_code, 0);
    EXPECT_TRUE(fs::is_directory(producer.instance("domain-root")));
    EXPECT_EQ(tk::read_file(outer.instance("domain-root") / "user-owned"), "preserve foreign control data");
    fs::remove(outer.instance("domain-root") / "user-owned");
    tk::write_file(outer.instance_file("domain-root"), "{bad JSON");
    const auto corruptEvents = home.xlings({"interface", "subos_events", "--args", eventArguments});
    EXPECT_NE(corruptEvents.exit_code, 0) << corruptEvents.transcript();
    EXPECT_NE(corruptEvents.out.find("E_INVALID_INPUT"), std::string::npos);
    EXPECT_EQ(corruptEvents.out.find("outer-control-probe"), std::string::npos);
    EXPECT_NE(home.xlings({"subos", "remove", "domain-root", "-y"}).exit_code, 0);
    EXPECT_TRUE(fs::is_directory(producer.instance("domain-root")));
    tk::write_file(outer.instance_file("domain-root"), controlBefore);
    const auto payload = privateHome / "data/xpkgs/fixture-x-domain-base/1.0.0";
    const auto payloadBefore = tk::read_file(payload / "produced-prefix");
    auto removed = home.xlings({"interface", "remove_subos", "--args", Json{{"name", "domain-root"}, {"yes", true}}.dump()});
    ASSERT_EQ(removed.exit_code, 0) << removed.transcript();
    int removedData = 0, removedResults = 0;
    std::istringstream removalLines(removed.out);
    while (std::getline(removalLines, eventLine)) {
        const auto event = Json::parse(eventLine);
        if (event["kind"] == "data" && event["dataKind"] == "subos_removed") {
            ++removedData;
            EXPECT_EQ(event["payload"]["name"], "domain-root");
        }
        if (event["kind"] == "result") ++removedResults;
    }
    EXPECT_EQ(removedData, 1);
    EXPECT_EQ(removedResults, 1);
    EXPECT_FALSE(fs::exists(producer.instance("domain-root")));
    EXPECT_FALSE(fs::exists(outer.instance("domain-root")));
    EXPECT_FALSE(fs::exists(outer.instance_file("domain-root")));
    EXPECT_FALSE(Json::parse(tk::read_file(home.dir() / ".xlings.json"))["subos"].contains("domain-root"));
    EXPECT_TRUE(fs::is_directory(producer.instance("domain-sibling")));
    EXPECT_TRUE(fs::is_regular_file(outer.instance_file("domain-sibling")));
    EXPECT_EQ(tk::read_file(payload / "produced-prefix"), payloadBefore);
    EXPECT_EQ(tk::read_file(outer.logs_dir("domain-root") / "events.ndjson"),
              Json({{"kind", "lifecycle"}, {"event", "outer-control-probe"}, {"session", "domain-session-probe"}}).dump() + "\n");
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");

}
