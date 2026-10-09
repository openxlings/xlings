#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.platform;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.subos.root;
import xlings.core.subos.store_closure;
import xlings.core.subos.root_view;
import xlings.subos.rootfs;
import xlings.subos.roles;
import xlings.subos.session;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.broker;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace p = xlings::platform;
namespace session = xlings::subos::session;
namespace closure = xlings::subos_root::store_closure;
namespace view = xlings::subos_root::root_view;
namespace rf = xlings::subos::rootfs;
using Json = nlohmann::json;

namespace {
constexpr auto roleVariable = "XLINGS_TEST_ROOT_VIEW_ROLE";

struct NamespaceProbe {
    NamespaceProbe() {
        const auto* role = std::getenv(roleVariable);
        if (!role || !*role)
            return;
        const auto value = std::string(role);
        p::unset_env_variable(roleVariable);
        if (value == "init") {
            const std::vector<std::string> arguments;
            std::_Exit(session::session_init(arguments));
        }
        if (value != "probe")
            std::_Exit(125);
        const fs::path home(std::getenv("XLINGS_HOME"));
        const auto visible = home / "data/xpkgs/fixture-x-old-visible/1.0.0/bin/old-visible";
        const auto hidden = home / "data/xpkgs/fixture-x-other-scope/9.0.0/private.txt";
        const auto added = home / "data/xpkgs/fixture-x-root-new/1.0.0/.xlings-resolution.json";
        std::ofstream write(visible, std::ios::app);
        const bool writable = write.good();
        write.close();
        const Json result{
            {"visible", fs::exists(visible)},
            {"visible_content", tk::read_file(visible)},
            {"hidden", fs::exists(hidden)},
            {"writable", writable},
            {"added", fs::exists(added)},
            {"projected", fs::exists(home / "subos/box/root/usr/bin/root-new")},
            {"root_pointer", fs::read_symlink(home / "subos/box/root").generic_string()}};
        tk::write_file("/run/root-view-result/result.json", result.dump());
        std::_Exit(0);
    }
} namespaceProbe;

std::string backend() {
    for (const auto& candidate :
         {std::string(std::getenv("XDEV_BWRAP") ? std::getenv("XDEV_BWRAP") : ""),
          std::string("/usr/lib/xlings/bwrap"), std::string("/usr/bin/bwrap"),
          std::string("/usr/local/bin/bwrap")}) {
        if (!candidate.empty() && fs::exists(candidate) &&
            tk::run({.argv = {candidate, "--ro-bind", "/", "/", "--", "/bin/true"}}).exit_code == 0)
            return candidate;
    }
    return {};
}

struct RunningView {
    tk::Home home = tk::Home::isolated("root-view-broker");
    std::shared_ptr<view::View> prepared;
    int generation{};
    ~RunningView() {
        (void)session::stop({home.dir()}, "box");
        if (prepared)
            prepared->close();
    }

    void setup() {
        const auto primary = home.root() / "empty-index";
        const auto fixture = home.root() / "fixture-index";
        tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
        fs::create_directories(primary / "pkgs");
        tk::write_file(fixture / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
        tk::write_file(fixture / "pkgs/r/root-new.lua", R"LUA(
            package = {
                spec = '1', name = 'root-new', type = 'script',
                archs = {'x86_64', 'aarch64'},
                xpm = {linux = {['1.0.0'] = {}}}
            }
            function xpkg_main() return true end
        )LUA");
        Json config{
            {"mirror", "GLOBAL"},
            {"xim", {{"index-repo", primary.string()}}},
            {"index_repos",
             Json::array({{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                           {"url", primary.string()},
                           {"source", "git"}},
                          {{"name", "fixture"}, {"url", fixture.string()}, {"source", "git"}}})}};
        tk::write_file(home.dir() / ".xlings.json", config.dump());
        fs::create_directories(home.dir() / "bin");
        fs::copy_file(tk::xlings_binary(), home.dir() / "bin/xlings",
                      fs::copy_options::overwrite_existing);
        const auto initialized = home.xlings({"self", "init"});
        if (initialized.exit_code != 0)
            throw std::runtime_error(initialized.transcript());
        const auto payload = home.dir() / "data/xpkgs/fixture-x-old-visible/1.0.0";
        tk::write_file(payload / "bin/old-visible", "old visible sentinel");
        tk::write_file(payload / ".xlings-resolution.json",
                       R"({"package":"fixture:old-visible@1.0.0","deps":[]})");
        tk::write_file(home.dir() / "data/xpkgs/fixture-x-other-scope/9.0.0/private.txt",
                       "other scope must stay hidden");
        config = Json::parse(tk::read_file(home.dir() / ".xlings.json"));
        config["versions"] = {
            {"old-visible",
             {{"type", "program"},
              {"versions", {{"fixture:1.0.0", {{"path", (payload / "bin").string()}}}}}}}};
        tk::write_file(home.dir() / ".xlings.json", config.dump());
        const auto scope = home.dir() / "subos/box";
        tk::write_file(scope / ".xlings.json",
                       Json{{"workspace",
                             {{"old-visible",
                               {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}}}}}}
                           .dump());
        const auto declared =
            xlings::subos_root::declare_kind(home.dir(), "box", xlings::subos::roles::Kind::Rootfs);
        if (!declared)
            throw std::runtime_error(declared.error());
        rf::Inputs initial;
        initial.payloads = {payload};
        initial.programs = {{"old-visible", payload / "bin/old-visible"}};
        const auto committed = rf::commit(scope, rf::plan(initial), "initial fixture");
        if (!committed)
            throw std::runtime_error(committed.error());
        generation = *committed;
        const auto laid = rf::lay_out(scope / "rootfs", rf::usr_of(scope), home.dir());
        if (!laid)
            throw std::runtime_error(laid.error());
        auto inputs = closure::read_scope(home.dir(), "box", scope / "rootfs");
        if (!inputs)
            throw std::runtime_error(inputs.error());
        auto result = view::prepare(*inputs);
        if (!result)
            throw std::runtime_error(result.error());
        prepared = std::move(*result);
        fs::create_directories(home.root() / "results");
    }

    int start(const std::string& bwrap, bool failRefresh) {
        std::vector<std::string> argv{bwrap,  "--unshare-user", "--ro-bind",   "/",
                                      "/",    "--proc",         "/proc",       "--tmpfs",
                                      "/run", "--dir",          "/run/xlings", "--die-with-parent"};
        for (const auto& binding : prepared->bindings())
            argv.insert(argv.end(),
                        {"--ro-bind", binding.source.string(), binding.destination.string()});
        argv.insert(argv.end(),
                    {"--bind", (home.root() / "results").string(), "/run/root-view-result", "--bind",
                     xlings::subos::HomeView{home.dir()}.broker_socket("box").string(),
                     std::string(xlings::subos::broker::kSocketInside), "--",
                     p::get_executable_path().string()});
        auto environment = tk::inherited_env();
        environment[roleVariable] = "init";
        environment["XLINGS_HOME"] = home.dir().string();
        auto owner = tk::inherited_env();
        owner["XLINGS_HOME"] = home.dir().string();
        owner["XLINGS_ACTIVE_SUBOS"] = "box";
        owner["XLINGS_NON_INTERACTIVE"] = "1";
        owner.erase("XLINGS_SUBOS_MODE");
        owner.erase("XLINGS_PROJECT_DIR");
        return session::host(
            {home.dir()},
            session::Launch{
                .instance = "box",
                .argv = std::move(argv),
                .env = environment,
                .backend = "bwrap",
                .digest = "root-view-fixture",
                .exec_env = {{roleVariable, "probe"}, {"XLINGS_HOME", home.dir().string()}},
                .default_cwd = "/",
                .ttl = 30,
                .detached = true,
                .broker_policy = xlings::subos::policy::preset(xlings::subos::policy::Preset::Dev),
                .broker_exe = {tk::xlings_binary().string()},
                .broker_env = std::move(owner),
                .refresh_root =
                    [current = prepared, failRefresh](std::span<const int, 3> target) {
                        if (failRefresh) {
                            const std::array<int, 3> broken{-1, target[1], target[2]};
                            return current->refresh(broken);
                        }
                        return current->refresh(target);
                    },
                .finalize_root = [current = prepared] { current->close(); }});
    }

    Json command(const std::vector<std::string>& arguments) {
        const auto fd = p::unix_connect(xlings::subos::HomeView{home.dir()}.broker_socket("box"));
        if (fd < 0)
            throw std::runtime_error("fixture broker unavailable");
        const int input = p::open_null();
        const int output = p::open_for_append(home.root() / "broker.log");
        const std::array<int, 3> stdio{input, output, output};
        const Json request{{"op", "run"}, {"argv", arguments}};
        if (!p::send_message(fd, request.dump(), stdio))
            throw std::runtime_error("cannot send fixture install");
        p::close_fd(input);
        p::close_fd(output);
        for (;;) {
            p::PollFd descriptor{fd};
            if (p::poll_fds(std::span(&descriptor, 1), 30000) <= 0) {
                p::close_fd(fd);
                throw std::runtime_error("fixture broker timeout");
            }
            auto packet = p::receive_message(fd);
            if (!packet) {
                p::close_fd(fd);
                throw std::runtime_error("fixture broker closed before reply");
            }
            p::close_fds(packet->fds);
            auto reply = Json::parse(packet->data);
            if (reply.contains("started"))
                continue;
            p::close_fd(fd);
            return reply;
        }
    }
    Json install() {
        return command({"install", "fixture:root-new@1.0.0"});
    }
};
} // namespace

XTEST(RootStoreClosureIsolation, BrokerSuccessPublishesNewPayloadAndProjectionBeforeReply,
      .area = "subos", .cost = tk::Cost::Medium, .covers = {"ROOT-STORE-CLOSURE"},
      .requires_ = {"linux", "bwrap", "xlings-bin"}, .resources = {"sandbox"},
      .proves = "isolation") {
    if constexpr (!tk::is_linux)
        GTEST_SKIP() << "Linux mount namespaces";
    if (const auto why = tk::probe("bwrap"))
        GTEST_SKIP() << *why;
    const auto bwrap = backend();
    ASSERT_FALSE(bwrap.empty());
    RunningView running;
    ASSERT_NO_THROW(running.setup());
    ASSERT_EQ(running.start(bwrap, false), 0)
        << tk::read_file(running.home.dir() / "logs/subos/box/session.log")
        << tk::read_file(running.home.dir() / "logs/subos/box/events.ndjson");
    const auto visible = running.home.dir() / "data/xpkgs/fixture-x-old-visible/1.0.0";
    fs::rename(visible, running.home.root() / "parked-old-payload");
    tk::write_file(visible / "bin/old-visible", "replacement at the same payload path");
    tk::write_file(visible / ".xlings-resolution.json",
                   R"({"package":"fixture:old-visible@1.0.0","deps":[]})");
    const auto reply = running.install();
    ASSERT_EQ(reply.value("exit", -1), 0)
        << reply << tk::read_file(running.home.root() / "broker.log");
    const auto inspected = session::join({running.home.dir()}, "box",
                                         {.argv = {p::get_executable_path().string()}, .cwd = "/"});
    ASSERT_EQ(inspected.exit_code, 0) << inspected.error;
    const auto result = Json::parse(tk::read_file(running.home.root() / "results/result.json"));
    EXPECT_TRUE(result["visible"].get<bool>());
    EXPECT_EQ(result["visible_content"].get<std::string>(), "replacement at the same payload path");
    EXPECT_FALSE(result["hidden"].get<bool>());
    EXPECT_FALSE(result["writable"].get<bool>());
    EXPECT_TRUE(result["added"].get<bool>());
    EXPECT_TRUE(result["projected"].get<bool>());
    EXPECT_NE(result["root_pointer"].get<std::string>(),
              "root.gen/" + std::to_string(running.generation));
    EXPECT_EQ(
        tk::read_file(running.home.dir() / "data/xpkgs/fixture-x-other-scope/9.0.0/private.txt"),
        "other scope must stay hidden");
    const auto removed = running.command({"remove", "old-visible@fixture:1.0.0", "-y", "--force"});
    ASSERT_EQ(removed.value("exit", -1), 0)
        << removed << tk::read_file(running.home.root() / "broker.log");
    const auto afterRemoval = session::join(
        {running.home.dir()}, "box", {.argv = {p::get_executable_path().string()}, .cwd = "/"});
    ASSERT_EQ(afterRemoval.exit_code, 0) << afterRemoval.error;
    const auto retired = Json::parse(tk::read_file(running.home.root() / "results/result.json"));
    EXPECT_FALSE(retired["visible"].get<bool>())
        << "withdrawing a replacement must not reveal its previous mount";
    EXPECT_FALSE(retired["hidden"].get<bool>());
    EXPECT_TRUE(retired["added"].get<bool>());
    EXPECT_EQ(tk::read_file(running.home.root() / "parked-old-payload/bin/old-visible"),
              "old visible sentinel");
}

XTEST(RootStoreClosureIsolation, FailedNamespaceRefreshReturns125AndEndsTheSession, .area = "subos",
      .cost = tk::Cost::Medium, .covers = {"ROOT-STORE-CLOSURE", "ROOT-REFRESH-FAIL"},
      .requires_ = {"linux", "bwrap", "xlings-bin"}, .resources = {"sandbox"},
      .proves = "isolation") {
    if constexpr (!tk::is_linux)
        GTEST_SKIP() << "Linux mount namespaces";
    if (const auto why = tk::probe("bwrap"))
        GTEST_SKIP() << *why;
    RunningView running;
    ASSERT_NO_THROW(running.setup());
    ASSERT_EQ(running.start(backend(), true), 0)
        << tk::read_file(running.home.dir() / "logs/subos/box/session.log")
        << tk::read_file(running.home.dir() / "logs/subos/box/events.ndjson");
    const auto reply = running.install();
    EXPECT_EQ(reply.value("exit", -1), 125) << reply;
    EXPECT_FALSE(reply.value("error", "").empty());
    for (int attempt = 0; attempt < 100 && session::find({running.home.dir()}, "box"); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(session::find({running.home.dir()}, "box"));
    EXPECT_FALSE(fs::exists(running.home.dir() / "run/subos/box/session.json"));
    EXPECT_NE(tk::read_file(running.home.dir() / "logs/subos/box/events.ndjson")
                  .find("root-refresh-failed"),
              std::string::npos);
}
