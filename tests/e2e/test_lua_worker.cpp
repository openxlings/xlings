#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import mcpplibs.xpkg.executor;
import xlings.platform.worker;
import xlings.core.xim.lua_protocol;
import xlings.core.elfread;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace xp = mcpplibs::xpkg;
namespace protocol = xlings::xim::lua_protocol;
namespace transport = xlings::platform::worker;

XTEST(LuaWorker, DedicatedControlPipesPreserveOneLuaStateAndCumulativeEffects, .area = "xim",
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("lua-worker-protocol");
    const auto recipe = home.root() / "worker.lua";
    tk::write_file(recipe, R"LUA(
        io.stdout:write('raw top-level output is never a control message\n')
        import('xim.libxpkg.xvm')
        import('xim.libxpkg.pkgmanager')
        local configured = false
        function install()
            configured = true
            xvm.add('first', {version='1.0.0', args={'space value'}})
            pkgmanager.install('fixture:extra@1.0.0')
            return true
        end
        function config()
            assert(configured, 'Lua state was reloaded')
            xvm.add('second', {type='files', src='share/data', dst='usr/share/data'})
            pkgmanager.remove('fixture:old')
            return true
        end
    )LUA");
    auto process = transport::Process::launch({tk::xlings_binary().string(), "__xpkg-worker",
                                               std::string(transport::kReadToken),
                                               std::string(transport::kWriteToken)},
                                              home.env());
    ASSERT_TRUE(process) << process.error();
    auto invoke = [&](xp::HookInvocation request) {
        nlohmann::json input = {{"protocol", xp::kHookBoundaryProtocol},
                                {"operation", "executor"},
                                {"invocation", protocol::encode(request)},
                                {"environment", nlohmann::json::object()},
                                {"cwd", home.root().generic_string()},
                                {"capture_log", (home.root() / "recipe.log").generic_string()}};
        auto wire = process->exchange(input.dump());
        if (!wire)
            throw std::runtime_error(wire.error());
        auto answer = nlohmann::json::parse(*wire);
        if (!answer.at("ok").get<bool>())
            throw std::runtime_error(answer.at("error").get<std::string>());
        return protocol::response(answer.at("value"));
    };
    auto executor = xp::create_executor(recipe, invoke);
    ASSERT_TRUE(executor) << executor.error();
    EXPECT_NE(tk::read_file(home.root() / "recipe.log").find("raw top-level output"),
              std::string::npos);
    xp::ExecutionContext context;
    context.install_dir = home.root() / "payload";
    context.pkg_name = "worker";
    context.version = "1.0.0";
    ASSERT_TRUE(executor->run_hook(xp::HookType::Install, context).success);
    ASSERT_EQ(executor->xvm_operations().size(), 1u);
    ASSERT_TRUE(executor->run_hook(xp::HookType::Config, context).success);
    ASSERT_EQ(executor->xvm_operations().size(), 2u);
    EXPECT_EQ(executor->xvm_operations()[0].args, (std::vector<std::string>{"space value"}));
    ASSERT_EQ(executor->install_requests().size(), 2u);
    EXPECT_EQ(executor->install_requests()[1].target, "fixture:old");
}

XTEST(LuaWorker, MalformedPolicyStopsBeforeAnyRecipeTopLevelCode, .area = "xim",
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("lua-worker-no-fallback");
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    const auto marker = home.root() / "host-top-level";
    const auto script = home.root() / "script.lua";
    tk::write_file(script, "local f=assert(io.open([[" + marker.generic_string() +
                               "]],'w')); f:write('host');f:close()\nfunction xpkg_main() end\n");
    tk::write_file(home.dir() / "config" / "subos" / "box" / "policy.json", "{invalid policy");
    const auto result = home.xlings({"script", script.string()}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    EXPECT_NE(result.exit_code, 0) << result.transcript();
    EXPECT_FALSE(fs::exists(marker));
    EXPECT_NE(result.transcript().find("policy"), std::string::npos);
}

XTEST(LuaWorker, DeclaredPolicyIsolatesWholeLuaIncludingIoExecuteAndPopen, .area = "xim",
      .requires_ = {"linux", "sandbox", "xlings-bin"}, .resources = {"sandbox"},
      .proves = "isolation") {
    auto home = tk::Home::isolated("lua-worker-isolation");
    ASSERT_TRUE(home.seed_sandbox_backend());
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "config", "box", "--sandbox=dev"}).exit_code, 0);
    const auto recipe_logs = home.dir() / "logs" / "recipes";
    ASSERT_FALSE(fs::exists(recipe_logs));
    const auto forbidden = home.root() / "host-secret";
    const auto external = home.root() / "external-recipes";
    const auto script = home.dir() / "data/external-recipes/script.lua";
    fs::create_directories(external);
    fs::create_directories(script.parent_path().parent_path());
    fs::create_directory_symlink(external, script.parent_path());
    tk::write_file(forbidden, "host original");
    const auto quoted = "'" + forbidden.generic_string() + "'";
    const std::string load_output = "RECIPE_LOAD_OUTPUT_BELONGS_IN_ITS_LOG";
    tk::write_file(script, "print('" + load_output + "')\nlocal r=io.open([[" +
                               forbidden.generic_string() +
                               "]],'r'); assert(r==nil, 'unrelated host file became visible')\n"
                               "local f=io.open([[" +
                               forbidden.generic_string() +
                               "]],'w'); if f then f:write('virtual top-level'); f:close() end\n"
                               "function xpkg_main()\n"
                               " os.execute([[printf escaped > " +
                               quoted +
                               "]])\n"
                               " local p=assert(io.popen([[printf escaped > " +
                               quoted +
                               "; printf alive]])); assert(p:read('*a')=='alive'); p:close()\n"
                               "end\n");
    const auto result = home.xlings({"script", script.string()}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    EXPECT_EQ(result.exit_code, 0) << result.transcript();
    EXPECT_EQ(result.out.find(load_output), std::string::npos);
    ASSERT_TRUE(fs::is_directory(recipe_logs));
    bool output_published = false;
    for (const auto& file : fs::directory_iterator(recipe_logs)) {
        if (file.path().filename().string().ends_with(".executor.load.log") &&
            tk::read_file(file.path()).find(load_output) != std::string::npos)
            output_published = true;
    }
    EXPECT_TRUE(output_published);
    EXPECT_EQ(tk::read_file(forbidden), "host original");
    EXPECT_TRUE(fs::is_symlink(script.parent_path()));
}

XTEST(LuaWorker, RecipeLogDirectorySymlinkRefusesBeforeRecipeLoad, .area = "xim",
      .requires_ = {"linux", "sandbox", "xlings-bin"}, .resources = {"sandbox"},
      .proves = "isolation") {
    auto home = tk::Home::isolated("lua-log-symlink");
    ASSERT_TRUE(home.seed_sandbox_backend());
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "config", "box", "--sandbox=dev"}).exit_code, 0);
    const auto outside = home.root() / "user-log-directory";
    const auto sentinel = outside / "user-file";
    tk::write_file(sentinel, "user-owned bytes");
    const auto before = fs::last_write_time(sentinel);
    const auto recipe_logs = home.dir() / "logs" / "recipes";
    fs::create_directories(recipe_logs.parent_path());
    fs::create_directory_symlink(outside, recipe_logs);
    const auto script = home.root() / "script.lua";
    tk::write_file(script, "error('RECIPE_TOP_LEVEL_MUST_NOT_RUN')\nfunction xpkg_main() end\n");
    const auto result = home.xlings({"script", script.string()}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    EXPECT_NE(result.exit_code, 0) << result.transcript();
    EXPECT_NE(result.transcript().find("recipe log directory"), std::string::npos);
    EXPECT_EQ(result.transcript().find("RECIPE_TOP_LEVEL_MUST_NOT_RUN"), std::string::npos);
    EXPECT_TRUE(fs::is_symlink(recipe_logs));
    EXPECT_EQ(tk::read_file(sentinel), "user-owned bytes");
    EXPECT_EQ(fs::last_write_time(sentinel), before);
    EXPECT_EQ(std::distance(fs::directory_iterator(outside), fs::directory_iterator{}), 1);
}

XTEST(LuaWorker, RuntimeInterpreterAliasUnderPrivateTmpRemainsExecutableAndReadonly, .area = "xim",
      .requires_ = {"linux", "sandbox", "xlings-bin"}, .resources = {"sandbox"},
      .proves = "isolation") {
    const auto runtime = xlings::elfread::read(tk::xlings_binary());
    ASSERT_TRUE(runtime);
    if (runtime->interpreter.empty())
        GTEST_SKIP() << "static entry has no PT_INTERP alias to exercise";
    auto home = tk::Home::isolated("lua-loader");
    ASSERT_TRUE(home.seed_sandbox_backend());
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);

    struct LoaderStage {
        fs::path root;
        ~LoaderStage() {
            if (!root.empty()) {
                std::error_code error;
                fs::remove_all(root, error); // exclusively reserved fixture directory
            }
        }
    } stage;
    std::random_device random;
    for (int attempt = 0; attempt != 32; ++attempt) {
        const auto candidate = fs::path("/tmp") / std::format("lw{:08x}", random());
        if (fs::create_directory(candidate)) {
            stage.root = candidate;
            break;
        }
    }
    ASSERT_FALSE(stage.root.empty());
    const auto loader = stage.root / "l" / "ld";
    fs::create_directory(stage.root / "s");
    fs::create_directory(stage.root / "f");
    fs::copy_file(fs::canonical(runtime->interpreter), stage.root / "f" / "ld");
    fs::create_symlink(stage.root / "f" / "ld", stage.root / "s" / "ld");
    fs::create_directory_symlink("s", stage.root / "l");
    ASSERT_LE(loader.generic_string().size(), runtime->interpreter.size());
    const auto original_loader = tk::read_file(loader);
    auto entry_bytes = tk::read_file(tk::xlings_binary());
    const auto position = entry_bytes.find(runtime->interpreter + '\0');
    ASSERT_NE(position, std::string::npos);
    const auto replacement = loader.generic_string();
    entry_bytes.replace(position, runtime->interpreter.size(),
                        replacement +
                            std::string(runtime->interpreter.size() - replacement.size(), '\0'));
    const auto entry = home.root() / "entry" / "xlings";
    fs::create_directories(entry.parent_path());
    fs::copy_file(tk::xlings_binary(), entry);
    tk::write_file(entry, entry_bytes);
    const auto patched = xlings::elfread::read(entry);
    ASSERT_TRUE(patched);
    ASSERT_EQ(patched->interpreter, replacement);
    auto environment = home.env();
    environment["XLINGS_ACTIVE_SUBOS"] = "box";
    const auto host =
        tk::run({.argv = {entry.string(), "--version"}, .env = environment, .cwd = home.root()});
    ASSERT_EQ(host.exit_code, 0) << host.transcript();
    const auto secret = stage.root / "unrelated-host-data";
    tk::write_file(secret, "host secret");
    const auto script = home.root() / "script.lua";
    tk::write_file(script, "assert(io.open([[" + secret.generic_string() +
                               "]], 'r') == nil, 'runtime mount exposed its parent')\n"
                               "assert(io.open([[" +
                               replacement +
                               "]], 'w') == nil, 'runtime loader is writable')\n"
                               "function xpkg_main() os.execute('uname >/dev/null') end\n");
    for (const auto preset : {"dev", "locked"}) {
        ASSERT_EQ(
            home.xlings({"subos", "config", "box", "--sandbox=" + std::string(preset)}).exit_code,
            0);
        const auto result = tk::run({.argv = {entry.string(), "script", script.string()},
                                     .env = environment,
                                     .cwd = home.root()});
        EXPECT_EQ(result.exit_code, 0) << preset << '\n' << result.transcript();
    }
    EXPECT_EQ(tk::read_file(loader), original_loader);
    EXPECT_EQ(tk::read_file(secret), "host secret");
    EXPECT_TRUE(fs::is_symlink(stage.root / "l"));
    const auto journal = tk::read_file(home.dir() / "logs" / "subos" / "box" / "events.ndjson");
    EXPECT_NE(journal.find("uname"), std::string::npos);
}

XTEST(LuaWorker, FullObservationAuditsHookSubprocessesWithoutArgumentValues, .area = "xim",
      .requires_ = {"linux", "sandbox", "xlings-bin"}, .resources = {"sandbox"},
      .proves = "isolation") {
    auto home = tk::Home::isolated("lua-worker-full-observe");
    ASSERT_TRUE(home.seed_sandbox_backend());
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "config", "box", "--sandbox=locked"}).exit_code, 0);
    const auto script = home.root() / "script.lua";
    tk::write_file(script,
                   "os.execute('true')\nfunction xpkg_main(argument)\n"
                   " os.execute('uname >/dev/null')\n"
                   " os.execute([[bash -c 'echo probe >/dev/tcp/127.0.0.1/9' >/dev/null 2>&1]])\n"
                   "end\n");
    const std::string secret = "ARGUMENT_VALUE_MUST_NOT_BE_AUDITED";
    const auto result =
        home.xlings({"script", script.string(), secret}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    const auto journal = tk::read_file(home.dir() / "logs" / "subos" / "box" / "events.ndjson");
    EXPECT_NE(journal.find("uname"), std::string::npos);
    EXPECT_NE(journal.find("net-attempt"), std::string::npos);
    EXPECT_NE(journal.find("127.0.0.1"), std::string::npos);
    EXPECT_NE(journal.find("\"result\":\"unknown\""), std::string::npos);
    EXPECT_NE(journal.find("\"worker\":\"lua\""), std::string::npos);
    EXPECT_EQ(journal.find(secret), std::string::npos);
}

XTEST(LuaWorker, LockedAuditFailureRefusesLuaBeforeItsTopLevelRuns, .area = "xim",
      .requires_ = {"linux", "sandbox", "xlings-bin"}, .resources = {"sandbox"},
      .proves = "isolation") {
    auto home = tk::Home::isolated("lua-worker-locked-audit");
    ASSERT_TRUE(home.seed_sandbox_backend());
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "config", "box", "--sandbox=locked"}).exit_code, 0);
    const auto journal = home.dir() / "logs" / "subos" / "box" / "events.ndjson";
    std::error_code ec;
    fs::remove(journal, ec);
    fs::create_directories(journal);
    const auto script = home.root() / "script.lua";
    tk::write_file(script, "error('RECIPE_TOP_LEVEL_MUST_NOT_RUN')\nfunction xpkg_main() end\n");
    const auto result = home.xlings({"script", script.string()}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    EXPECT_NE(result.exit_code, 0) << result.transcript();
    EXPECT_NE(result.transcript().find("audit"), std::string::npos);
    EXPECT_EQ(result.transcript().find("RECIPE_TOP_LEVEL_MUST_NOT_RUN"), std::string::npos);
}

XTEST(LuaWorker, HostEffectsRejectHostSourcesTraversalAndSymlinksButAcceptOwnedPayloadAssets,
      .area = "xim", .cost = tk::Cost::Medium, .requires_ = {"linux", "sandbox", "xlings-bin"},
      .resources = {"sandbox"}, .proves = "isolation") {
    auto home = tk::Home::isolated("lua-worker-host-effects");
    ASSERT_TRUE(home.seed_sandbox_backend());
    const auto secret = home.root() / "private-user-file";
    tk::write_file(secret, "never expose this host data through a recipe declaration");
    const auto sibling = home.dir() / "data/xpkgs/unrelated-x-payload/1.0.0/user-file";
    tk::write_file(sibling, "unrelated payload bytes");
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(
        repo / "pkgs" / "f" / "fixture.lua",
        R"LUA(
        package = {
            spec = '1', name = 'fixture', type = 'package', archs = {'x86_64','aarch64'},
            xpm = {linux = {['1.0.0']={}, ['2.0.0']={}, ['3.0.0']={}, ['4.0.0']={}}}
        }
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
    )LUA" + std::string("local sibling = io.open([[") + sibling.generic_string() +
            "]], 'w'); assert(not sibling, 'unrelated payload became writable')\n" + R"LUA(
            os.mkdir(pkginfo.install_dir())
            io.writefile(path.join(pkginfo.install_dir(), 'asset'), 'owned payload')
            if pkginfo.version() == '2.0.0' then
    )LUA" + std::string("os.execute('ln -s ' .. string.format('%q', [[") +
            secret.generic_string() +
            "]]) .. ' ' .. string.format('%q', path.join(pkginfo.install_dir(), 'escape')))\n" +
            R"LUA(
            end
            return true
        end
        function config()
            local dir = pkginfo.install_dir()
            xvm.add('fixture', {type='group'})
            if pkginfo.version() == '1.0.0' then
    )LUA" + std::string("xvm.add('escape', {type='files', bindir=[[") +
            secret.parent_path().generic_string() +
            "]], src='private-user-file', dst='usr/share/forbidden'})\n" + R"LUA(
            elseif pkginfo.version() == '2.0.0' then
                xvm.add('escape', {type='files', src='escape', dst='usr/share/forbidden'})
            elseif pkginfo.version() == '3.0.0' then
                xvm.add('escape', {type='files', src='../private-user-file', dst='usr/share/forbidden'})
            else
                xvm.add('owned', {type='files', src='asset', dst='usr/share/owned'})
            end
            return true
        end
    )LUA");
    const auto primary = home.root() / "empty-primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    nlohmann::json config = {
        {"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", nlohmann::json::array({
            {{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
            {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}};
    tk::write_file(home.dir() / ".xlings.json", config.dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / "xlings");
    const auto initialized = home.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();
    ASSERT_EQ(home.xlings({"subos", "new", "box"}).exit_code, 0);
    ASSERT_EQ(home.xlings({"subos", "config", "box", "--sandbox=locked"}).exit_code, 0);
    ASSERT_FALSE(fs::exists(home.dir() / "data/xpkgs/fixture-x-fixture"));
    for (const auto version : {"1.0.0", "2.0.0", "3.0.0"}) {
        const auto result =
            home.xlings({"install", "fixture:fixture@" + std::string(version), "-y"},
                        {{"XLINGS_ACTIVE_SUBOS", "box"}});
        EXPECT_NE(result.exit_code, 0) << result.transcript();
        EXPECT_NE(result.transcript().find("unsafe Lua host effect"), std::string::npos)
            << result.transcript();
        EXPECT_FALSE(fs::exists(home.dir() / "subos" / "box" / "usr" / "share" / "forbidden"));
    }
    const auto allowed =
        home.xlings({"install", "fixture:fixture@4.0.0", "-y"}, {{"XLINGS_ACTIVE_SUBOS", "box"}});
    ASSERT_EQ(allowed.exit_code, 0) << allowed.transcript();
    EXPECT_EQ(tk::read_file(home.dir() / "subos" / "box" / "usr" / "share" / "owned"),
              "owned payload");
    EXPECT_EQ(tk::read_file(secret), "never expose this host data through a recipe declaration");
    EXPECT_EQ(tk::read_file(sibling), "unrelated payload bytes");
}
