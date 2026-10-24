#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.home.domain_producer;
import xlings.libs.elfread;
import xlings.subos.home_view;
import xlings.subos.manifest;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;

XTEST(DomainProducer, InstallsAtLogicalPrefixInsideANamespaceAndPreservesHostData,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"DOM-BUILD-INSIDE", "DOM-LAYOUTS", "LUBAN-ENTER-QUIET", "PERM-FETCH-ASK", "SUBOS-DOMAIN-SESSION-GPU"}, .requires_ = {"linux", "bwrap", "xlings-bin"},
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
    // A query inside answers from the index the system has (design
    // 2026-10-16 L §7), and never fetches one.
    EXPECT_TRUE(fs::is_directory(privateHome / "data/xim-pkgindex"));
    // Its parsed index with it, naming its recipes where the system sees
    // them (design 2026-10-20 §4): nothing inside has to rebuild it -- in a
    // sandbox nothing inside can.
    const auto cache = privateHome / "data/xim-pkgindex/.xlings-index-cache.json";
    ASSERT_TRUE(fs::exists(cache));
    const auto parsed = Json::parse(tk::read_file(cache), nullptr, false);
    ASSERT_TRUE(parsed.is_object());
    for (auto it = parsed["entries"].begin(); it != parsed["entries"].end(); ++it) {
        const auto path = it.value().value("path", std::string{});
        EXPECT_TRUE(path.empty() || path.starts_with("/xlings/data/xim-pkgindex/")) << it.key() << ": " << path;
    }
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
    {
        // What the owner's side was run with reaches the root's command
        // (E18: luban's own greeting asks for no line of xlings').
        xlings::platform::set_env_variable("XLINGS_ENTER_QUIET", "1");
        auto quiet = xlings::home::domain_producer::command((**scope).domain, backendArguments, std::nullopt,
                                                             backendSource ? &*backendSource : nullptr, true);
        xlings::platform::unset_env_variable("XLINGS_ENTER_QUIET");
        ASSERT_TRUE(quiet) << quiet.error();
        EXPECT_NE(std::ranges::search(*quiet, std::vector<std::string>{"--setenv", "XLINGS_ENTER_QUIET", "1"}).begin(), quiet->end());
    }
    {
        // A session there sees this computer's GPU devices, which its own
        // sandbox grants or not (J58); maintenance sees none.
        const bool dri = fs::exists("/dev/dri");
        auto session = xlings::home::domain_producer::command((**scope).domain, std::vector<std::string>{"subos", "use", "domain-root", "--cmd", "true"},
                                                               std::nullopt, backendSource ? &*backendSource : nullptr, true);
        ASSERT_TRUE(session) << session.error();
        const std::vector<std::string> bind{"--dev-bind", "/dev/dri", "/dev/dri"};
        EXPECT_EQ(std::ranges::search(*session, bind).begin() != session->end(), dri);
        EXPECT_EQ(std::ranges::search(*backendCommand, bind).begin(), backendCommand->end());
    }
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

    // What the system asked its owner for is queued where it lives (its
    // supervisor's home is the domain's): the owner lists and answers it
    // there (luban-os-ci L2-14: the owner found none).
    tk::write_file(producer.requests_dir("domain-root") / "r0a1b2c.json",
                   Json{{"id", "r0a1b2c"}, {"argv", {"install", "xim:fixture-tool"}},
                        {"created", "2026-10-22T00:00:00Z"}, {"reason", "fetch=ask"}}.dump());
    auto requests = home.xlings({"subos", "requests", "domain-root", "--json"});
    ASSERT_EQ(requests.exit_code, 0) << requests.transcript();
    EXPECT_NE(requests.out.find("\"r0a1b2c\""), std::string::npos) << requests.transcript();
    auto denied = home.xlings({"subos", "deny", "domain-root", "r0a1b2c"});
    EXPECT_EQ(denied.exit_code, 0) << denied.transcript();
    EXPECT_FALSE(fs::exists(producer.requests_dir("domain-root") / "r0a1b2c.json")) << denied.transcript();
    // A machine's broker request is served where the system lives too: its
    // verdict and audit are the domain's (an install it ran used to go into
    // the owner's copy, which the machine never reads: luban-os-ci L2-02).
    auto served = home.xlings({"subos", "__broker-serve", "domain-root", "--actor", "probe", "--", "list"});
    EXPECT_NE(served.exit_code, 0) << "a read is not the broker's: " << served.transcript();
    const auto domainLog = producer.logs_dir("domain-root") / "events.ndjson";
    ASSERT_TRUE(fs::exists(domainLog)) << served.transcript();
    EXPECT_NE(tk::read_file(domainLog).find("\"actor\":\"probe\""), std::string::npos) << tk::read_file(domainLog);
    const auto ownerLog = outer.logs_dir("domain-root") / "events.ndjson";
    EXPECT_TRUE(!fs::exists(ownerLog) || tk::read_file(ownerLog).find("\"actor\":\"probe\"") == std::string::npos);
    // What it records there is read from here too (part 2 J40): its log and report.
    auto logged_there = home.xlings({"subos", "log", "domain-root", "--json"});
    EXPECT_NE(logged_there.out.find("\"actor\":\"probe\""), std::string::npos) << logged_there.transcript();
    auto reported = home.xlings({"subos", "report", "domain-root"});
    EXPECT_EQ(reported.exit_code, 0) << reported.transcript();
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
    // Empty templates have no shell in the readonly /usr projection. Reserve
    // this fixture's machine mountpoints without changing that projection.
    const auto machineRoot = privateHome / "subos/domain-root/rootfs";
    for (const auto* directory : {"bin", "lib", "lib64"}) {
        const auto at = machineRoot / directory;
        ASSERT_TRUE(fs::is_symlink(fs::symlink_status(at))) << at;
        ASSERT_EQ(fs::read_symlink(at), fs::path("usr") / directory) << at;
        ASSERT_TRUE(fs::remove(at));
        ASSERT_TRUE(fs::create_directory(at));
    }
    std::vector<std::string> shellGrants;
    for (const auto& file : shellFiles) {
        ASSERT_TRUE(fs::is_regular_file(file)) << file;
        const auto relative = file.relative_path();
        ASSERT_FALSE(relative.empty());
        const auto directory = relative.begin()->generic_string();
        ASSERT_TRUE(directory == "bin" || directory == "lib" || directory == "lib64")
            << "fixture runtime grant needs a machine mountpoint: " << file;
        const auto destination = machineRoot / relative;
        fs::create_directories(destination.parent_path());
        ASSERT_FALSE(fs::exists(fs::symlink_status(destination))) << destination;
        tk::write_file(destination, "");
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
    {
        // The owner's index moves on (`xlings update`) while the system runs:
        // the next call through the domain brings it, in place -- the same
        // directory, which the running session has bound, not one swapped in
        // and the old deleted (design 2026-10-20 §4.13 V12).
        const auto index = home.dir() / "data/xim-pkgindex", carried = privateHome / "data/xim-pkgindex";
        const auto inode = [&] { return tk::run({.argv = {"stat", "-c", "%i", carried.string()}}).out; };
        const auto call = [&] {
            auto r = home.xlings({"subos", "exec", "domain-root", "--", "/xlings/bin/xlings", "--version"});
            EXPECT_EQ(r.exit_code, 0) << r.transcript();
        };
        const auto bound = inode();
        tk::write_file(index / "pkgs/o/old-one.lua", "-- the first snapshot's\n");
        tk::write_file(index / ".xlings-index-version", "snap-1\n");
        call();
        EXPECT_EQ(tk::read_file(carried / ".xlings-index-version"), "snap-1\n");
        EXPECT_TRUE(fs::exists(carried / "pkgs/o/old-one.lua"));
        fs::remove(index / "pkgs/o/old-one.lua");
        tk::write_file(index / "pkgs/n/new-one.lua", "-- the second snapshot's\n");
        tk::write_file(index / ".xlings-index-version", "snap-2\n");
        call();
        EXPECT_EQ(tk::read_file(carried / ".xlings-index-version"), "snap-2\n");
        EXPECT_EQ(tk::read_file(carried / "pkgs/n/new-one.lua"), "-- the second snapshot's\n");
        EXPECT_FALSE(fs::exists(carried / "pkgs/o/old-one.lua"));
        EXPECT_EQ(inode(), bound) << "the carried index was replaced, not updated";
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
    EXPECT_EQ(tk::read_file(privateHome / "subos/domain-root/rootfs/tmp/copied"), "host must remain intact");
    const auto copiedOut = home.root() / "copied-out";
    auto retrieved = home.xlings({"subos", "cp", "domain-root:/tmp/copied", copiedOut.string()});
    ASSERT_EQ(retrieved.exit_code, 0) << retrieved.transcript();
    EXPECT_EQ(tk::read_file(copiedOut), "host must remain intact");
    EXPECT_NE(home.xlings({"subos", "cp", hostSentinel.string(), "domain-root:/xlings/forbidden"}).exit_code, 0);
    fs::create_directory_symlink(hostSentinel.parent_path(), privateHome / "subos/domain-root/rootfs/tmp/escape");
    EXPECT_NE(home.xlings({"subos", "cp", hostSentinel.string(), "domain-root:/tmp/escape/host-sentinel"}).exit_code, 0);
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");
    auto forked = home.xlings({"subos", "new", "domain-sibling", "--from", "domain-root"});
    ASSERT_EQ(forked.exit_code, 0) << forked.transcript();
    EXPECT_EQ(tk::read_file(privateHome / "subos/domain-sibling/rootfs/tmp/copied"), "host must remain intact");
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

// Each layer lists itself and what is below it (part 3 §3.2): the host counts
// its Luban systems in one line, and a system does not list its siblings.
XTEST(DomainProducer, AHostCountsItsSystemsAndASystemDoesNotListItsSiblings,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"LAYER-VISIBILITY", "SUBOS-DOMAIN-RELAYED"}, .requires_ = {"linux", "bwrap", "xlings-bin"},
      .resources = {"sandbox"}, .proves = "isolation") {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux namespaces";
    if (const auto why = tk::probe("bwrap")) GTEST_SKIP() << *why;
    const auto executable = xlings::elfread::read(tk::xlings_binary());
    ASSERT_TRUE(executable);
    if (!executable->interpreter.empty()) GTEST_SKIP() << "a domain root runs the static release candidate";
    auto home = tk::Home::isolated("domain-layers");
    const auto repo = home.root() / "index";
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/d/domain-base.lua", R"LUA(
        package = { spec='1', name='domain-base', type='subos', archs={'x86_64','aarch64'},
            xpm={linux={['1.0.0']={}}} }
        import('xim.libxpkg.pkginfo')
        function install()
            local dir = pkginfo.install_dir()
            os.mkdir(path.join(dir, 'bin'))
            io.writefile(path.join(dir, '.xlings.json'), '{"workspace":{},"subos_kind":"rootfs"}')
            return true
        end
    )LUA");
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    for (const auto* name : {"sys-a", "sys-b"}) {
        auto made = home.xlings({"subos", "new", name, "--rootfs", "--domain", "/xlings", "--from", "fixture:domain-base@1.0.0"});
        ASSERT_EQ(made.exit_code, 0) << made.transcript();
    }
    const auto listed = home.xlings({"subos", "list"});
    ASSERT_EQ(listed.exit_code, 0) << listed.transcript();
    EXPECT_EQ(listed.out.find("sys-a"), std::string::npos) << listed.transcript();
    EXPECT_NE(listed.transcript().find("2 Luban systems: luban ls"), std::string::npos) << listed.transcript();
    const auto everything = home.xlings({"subos", "list", "--all"});
    EXPECT_NE(everything.out.find("sys-a"), std::string::npos) << everything.transcript();
    EXPECT_NE(everything.out.find("sys-b"), std::string::npos) << everything.transcript();
    const auto inside = home.xlings({"subos", "exec", "sys-a", "--", "/xlings/bin/xlings", "subos", "list"});
    ASSERT_EQ(inside.exit_code, 0) << inside.transcript();
    EXPECT_NE(inside.out.find("sys-a"), std::string::npos) << inside.transcript();
    EXPECT_EQ(inside.out.find("sys-b"), std::string::npos) << "a sibling is listed inside: " << inside.transcript();
    // What a domain system answers comes back through the relay as events:
    // upgrade's plan is one, so the terminal shows it (it once showed only
    // "no renderer for 'subos_upgrade_plan'").
    const auto upgrade = home.xlings({"subos", "upgrade", "sys-a", "--dry-run"});
    ASSERT_EQ(upgrade.exit_code, 0) << upgrade.transcript();
    EXPECT_NE(upgrade.out.find("sys-a is up to date"), std::string::npos) << upgrade.transcript();
    EXPECT_EQ(upgrade.transcript().find("no renderer"), std::string::npos) << upgrade.transcript();
    // Asked through the interface (luban, the window), a routed command's
    // data comes back as events too: `luban history` of a domain's system was
    // empty, the list printed on a stdout nobody read (design 2026-10-19 F13).
    const auto history = home.xlings({"interface", "subos_command", "--args",
                                      R"({"argv":["rollback","sys-a","--list"]})"});
    ASSERT_EQ(history.exit_code, 0) << history.transcript();
    bool listed_generations = false;
    for (const auto& j : history.json_lines()) {
        if (j.is_object() && j.value("kind", "") == "data" && j.value("dataKind", "") == "subos_generations")
            listed_generations = j["payload"].is_array() && !j["payload"].empty();
    }
    EXPECT_TRUE(listed_generations) << history.transcript();
    // Read from a pipe, not through the interface (luban's Port::json): what
    // the command prints still comes back as it printed it.
    const auto piped = home.xlings({"subos", "config", "sys-a", "--json"});
    ASSERT_EQ(piped.exit_code, 0) << piped.transcript();
    EXPECT_TRUE(Json::parse(piped.out, nullptr, false).is_object()) << "a pipe reads the JSON it asked for: " << piped.out;
    const auto missing = home.xlings({"interface", "subos_command", "--args",
                                      R"({"argv":["rollback","sys-a","--to","99"]})"});
    EXPECT_NE(missing.out.find(R"("kind":"error")"), std::string::npos)
        << "an error inside the domain is an event outside: " << missing.transcript();
}

// One default runtime per environment (part 3 §4.4): two systems of one
// domain, one on glibc 1.0 and one on 2.0, share the one xz both use --
// before, the second was refused ("xz is linked against glibc 1.0, which
// subos ... still uses"). Each carries its own runtime in its generation.
XTEST(DomainProducer, TwoSystemsOnTwoRuntimesShareOnePayload,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"RUNTIME-SCOPE", "DOMAIN-LUBAN-SYSTEM"}, .requires_ = {"linux", "bwrap", "xlings-bin"},
      .resources = {"sandbox"}, .proves = "isolation") {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux namespaces";
    if (const auto why = tk::probe("bwrap")) GTEST_SKIP() << *why;
    const auto executable = xlings::elfread::read(tk::xlings_binary());
    ASSERT_TRUE(executable);
    if (!executable->interpreter.empty()) GTEST_SKIP() << "a domain root runs the static release candidate";
    auto home = tk::Home::isolated("domain-runtimes");
    const auto repo = home.root() / "index";
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    // A libc in name: a loader and a libc.so.6 per version.
    tk::write_file(repo / "pkgs/g/glibc.lua", R"LUA(
        package = { spec='1', name='glibc', type='package', archs={'x86_64','aarch64'},
            xpm={linux={['1.0.0']={}, ['2.0.0']={}}},
            exports={runtime={loader='lib64/ld-linux-x86-64.so.2', abi='linux-x86_64-glibc', libdirs={'lib64'}}} }
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            local lib = path.join(pkginfo.install_dir(), 'lib64')
            os.mkdir(lib)
            io.writefile(path.join(lib, 'ld-linux-x86-64.so.2'), 'loader ' .. pkginfo.version())
            io.writefile(path.join(lib, 'libc.so.6'), 'libc ' .. pkginfo.version())
            return true
        end
        function config()
            -- As the real one does: its libraries are registered.
            xvm.add('libc.so.6', {type = 'lib', bindir = path.join(pkginfo.install_dir(), 'lib64'),
                                  filename = 'libc.so.6'})
            return true
        end
    )LUA");
    tk::write_file(repo / "pkgs/x/xz.lua", R"LUA(
        package = { spec='1', name='xz', type='package', archs={'x86_64','aarch64'},
            programs={'xz'}, xpm={linux={deps={runtime={'fixture:glibc'}}, ['5.8.3']={}}} }
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            os.mkdir(path.join(pkginfo.install_dir(), 'bin'))
            io.writefile(path.join(pkginfo.install_dir(), 'bin', 'xz'), '#!/bin/sh\necho xz\n')
            return true
        end
        function config()
            xvm.add('xz', {bindir = path.join(pkginfo.install_dir(), 'bin')})
            return true
        end
    )LUA");
    for (const auto* v : {"1.0.0", "2.0.0"})
        tk::write_file(repo / std::format("pkgs/s/system-{}.lua", v[0]), std::format(R"LUA(
        package = {{ spec='1', name='system-{0}', type='subos', archs={{'x86_64','aarch64'}},
            xpm={{linux={{['1.0.0']={{}}}}}} }}
        import('xim.libxpkg.pkginfo')
        function install()
            local dir = pkginfo.install_dir()
            os.mkdir(path.join(dir, 'bin'))
            io.writefile(path.join(dir, '.xlings.json'),
                '{{"workspace":{{}},"subos_kind":"rootfs","packages":["fixture:glibc@{1}","fixture:xz@5.8.3"]}}')
            return true
        end
    )LUA", v[0], v));
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    for (const auto* v : {"1", "2"}) {
        auto made = home.xlings({"subos", "new", std::format("sys-{}", v), "--rootfs", "--domain", "/xlings",
                                 "--from", std::format("fixture:system-{}@1.0.0", v), "-y"});
        ASSERT_EQ(made.exit_code, 0) << made.transcript();
        EXPECT_EQ(made.transcript().find("still uses"), std::string::npos) << made.transcript();
    }
    const auto store = home.dir() / "domains/xlings/private";
    std::size_t xz = 0;
    for (auto& e : fs::directory_iterator(store / "data/xpkgs/fixture-x-xz")) xz += e.is_directory() ? 1 : 0;
    EXPECT_EQ(xz, 1u) << "one xz for both";
    for (const auto& [name, version] : {std::pair{"sys-1", "1.0.0"}, std::pair{"sys-2", "2.0.0"}}) {
        const auto runtime = store / "subos" / name / "root/usr/lib/xlings-abi/linux-x86_64-glibc/libc.so.6";
        EXPECT_EQ(fs::read_symlink(runtime).generic_string(),
                  std::format("/xlings/data/xpkgs/fixture-x-glibc/{}/lib64/libc.so.6", version)) << name;
    }
    const auto record = Json::parse(tk::read_file(store / "data/xpkgs/fixture-x-xz/5.8.3/.xlings-resolution.json"));
    ASSERT_EQ(record["deps"].size(), 1u) << record.dump(2);
    EXPECT_EQ(record["deps"][0]["source"], "scope");
    EXPECT_EQ(record["deps"][0]["libdirs"][0], "/xlings/abi/linux-x86_64-glibc");
    // Exported, a payload bound through the runtime keeps its binding: the
    // image's <home>/abi is the runtime its root carries (2026.10.15.1
    // refused: "/xlings/abi/...: export path is outside the checked payload closure").
    const auto output = home.root() / "export";
    auto exported = home.xlings({"subos", "export", "sys-2", "--rootfs", output.string()});
    ASSERT_EQ(exported.exit_code, 0) << exported.transcript();
    EXPECT_EQ(fs::read_symlink(output / "xlings/abi").generic_string(), "subos/default/root/usr/lib/xlings-abi");
    EXPECT_EQ(fs::read_symlink(output / "xlings/subos/default/root/usr/lib/xlings-abi/linux-x86_64-glibc/libc.so.6").generic_string(),
              "/xlings/data/xpkgs/fixture-x-glibc/2.0.0/lib64/libc.so.6");
}

// What luban asks before it moves a system into its own userland (design
// 2026-10-23 part 3, J47): the domain tried, as making a system there starts
// -- not the tools judged. Where bwrap makes namespaces the domain's home is
// initialised (the client's own `self init` runs there, static or not);
// elsewhere an error says why.
XTEST(DomainProducer, ADomainCheckTriesTheNamespaceAsMakingASystemThereStarts,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"DOM-CHECK-TRIED"}, .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux namespaces";
    auto home = tk::Home::isolated("domain-check");
    const auto primary = home.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    Json config{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    EXPECT_EQ(home.xlings({"subos", "__domain-check"}).exit_code, 2) << "which domain";

    const bool builds = !tk::probe("bwrap");
    const auto checked = home.xlings({"subos", "__domain-check", "/xlings"});
    const auto inside = home.dir() / "domains/xlings/private/subos/default";
    if (builds) {
        EXPECT_EQ(checked.exit_code, 0) << checked.transcript();
        EXPECT_TRUE(fs::is_directory(inside)) << "its home initialised inside";
    } else {
        EXPECT_EQ(checked.exit_code, 1) << checked.transcript();
        EXPECT_NE(checked.transcript().find("cannot build the domain at /xlings"), std::string::npos)
            << "says which and why: " << checked.transcript();
        EXPECT_FALSE(fs::is_directory(inside));
    }
    // Asked of this home's own prefix -- what the check asks inside the
    // domain -- whether a root made here can be entered: the domain's bwrap,
    // not the producer's (2026.10.23.2 said yes where only the producer's
    // worked, and luban carried a system away to make it again in the prefix).
    const auto own = home.xlings({"subos", "__domain-check", home.dir().string()});
    if (builds) EXPECT_EQ(own.exit_code, 0) << own.transcript();
    else if (own.exit_code != 0) EXPECT_NE(own.transcript().find("bwrap"), std::string::npos) << own.transcript();
    // Whatever this host is: a namespace backend that fails is a domain that
    // does not build (XDEV_BWRAP, the producer's explicit backend).
    const auto no_bwrap = home.root() / "no-bwrap";
    tk::write_file(no_bwrap, "#!/bin/sh\nexit 1\n");
    fs::permissions(no_bwrap, fs::perms::owner_all, fs::perm_options::add);
    fs::remove_all(home.dir() / "domains");
    const auto refused = home.xlings({.argv = {"subos", "__domain-check", "/xlings"}, .env = {{"XDEV_BWRAP", no_bwrap.string()}}});
    EXPECT_EQ(refused.exit_code, 1) << refused.transcript();
    EXPECT_NE(refused.transcript().find("cannot build the domain at /xlings"), std::string::npos) << refused.transcript();
    EXPECT_FALSE(fs::is_directory(inside));
}

