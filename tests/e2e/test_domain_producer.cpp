#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.libs.json;
import xlings.core.home.domain_producer;
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

    auto doctor = home.xlings({"subos", "doctor", "domain-root", "--json"});
    const auto diagnosed = Json::parse(doctor.out);
    ASSERT_EQ(diagnosed["instances"].size(), 1u) << doctor.transcript();
    EXPECT_EQ(diagnosed["instances"][0]["instance"], "domain-root");
    EXPECT_NE(diagnosed.dump().find("/xlings/config/subos/domain-root/policy.json"), std::string::npos);
    {
        std::ofstream events(producer.logs_dir("domain-root") / "events.ndjson", std::ios::app);
        ASSERT_TRUE(events);
        events << Json{{"kind", "lifecycle"}, {"event", "domain-route-probe"}, {"session", "--global"}}.dump() << '\n';
    }
    tk::write_file(outer.logs_dir("domain-root") / "events.ndjson",
                  Json{{"kind", "lifecycle"}, {"event", "outer-control-probe"}, {"session", "--global"}}.dump() + "\n");
    auto logged = home.xlings({"subos", "log", "domain-root", "--json", "--session", "--global"});
    ASSERT_EQ(logged.exit_code, 0) << logged.transcript();
    EXPECT_EQ(Json::parse(logged.out)["event"], "domain-route-probe");

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
    EXPECT_NE(home.xlings({"subos", "remove", "domain-root", "-y"}).exit_code, 0);
    EXPECT_TRUE(fs::is_directory(producer.instance("domain-root")));
    tk::write_file(outer.instance_file("domain-root"), controlBefore);
    const auto payload = privateHome / "data/xpkgs/fixture-x-domain-base/1.0.0";
    const auto payloadBefore = tk::read_file(payload / "produced-prefix");
    auto removed = home.xlings({"subos", "remove", "domain-root", "-y"});
    ASSERT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(fs::exists(producer.instance("domain-root")));
    EXPECT_FALSE(fs::exists(outer.instance("domain-root")));
    EXPECT_FALSE(fs::exists(outer.instance_file("domain-root")));
    EXPECT_FALSE(Json::parse(tk::read_file(home.dir() / ".xlings.json"))["subos"].contains("domain-root"));
    EXPECT_TRUE(fs::is_directory(producer.instance("domain-sibling")));
    EXPECT_TRUE(fs::is_regular_file(outer.instance_file("domain-sibling")));
    EXPECT_EQ(tk::read_file(payload / "produced-prefix"), payloadBefore);
    EXPECT_EQ(tk::read_file(outer.logs_dir("domain-root") / "events.ndjson"),
              Json({{"kind", "lifecycle"}, {"event", "outer-control-probe"}, {"session", "--global"}}).dump() + "\n");
    EXPECT_EQ(tk::read_file(hostSentinel), "host must remain intact");

}
