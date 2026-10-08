#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.libs.sha256;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

namespace {
nlohmann::json local_indexes(const fs::path& fixture) {
    const auto primary = fixture.parent_path() / "empty-primary-index";
    fs::create_directories(primary / "pkgs");
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    return {{"mirror", "GLOBAL"},
            {"xim", {{"index-repo", primary.generic_string()}}},
            {"index_repos",
             nlohmann::json::array(
                 {{{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                   {"url", primary.generic_string()},
                   {"source", "git"}},
                  {{"name", "fixture"}, {"url", fixture.generic_string()}, {"source", "git"}}})}};
}
} // namespace

XTEST(ResolutionEvidence, FreshLeafAndExplicitMigrationAreCheckedBeforeSuccess, .area = "xim",
      .covers = {"HOME-LAYER-RESOLVE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("resolution-evidence");
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs" / "r" / "resolution-leaf.lua", R"LUA(
        package = {
            spec = '1', name = 'resolution-leaf', type = 'script',
            archs = {'x86_64', 'aarch64'},
            xpm = {
                linux = {['1.0.0'] = {}},
                macosx = {['1.0.0'] = {}},
                windows = {['1.0.0'] = {}}
            }
        }
        function xpkg_main() return true end
    )LUA");
    tk::write_file(home.dir() / ".xlings.json", local_indexes(repo).dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(),
                  home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    const auto initialized = home.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();
    const auto installed = home.xlings({"install", "fixture:resolution-leaf@1.0.0", "-y"});
    ASSERT_EQ(installed.exit_code, 0) << installed.transcript();
    const auto evidence = home.dir() / "data" / "xpkgs" / "fixture-x-resolution-leaf" / "1.0.0" /
                          ".xlings-resolution.json";
    ASSERT_TRUE(fs::is_regular_file(evidence));
    const auto record = nlohmann::json::parse(tk::read_file(evidence));
    EXPECT_EQ(record.at("package"), "fixture:resolution-leaf@1.0.0");
    EXPECT_EQ(record.at("deps"), nlohmann::json::array());

    ASSERT_TRUE(fs::remove(evidence));
    const auto reused = home.xlings({"install", "fixture:resolution-leaf@1.0.0", "-y"});
    ASSERT_EQ(reused.exit_code, 0) << reused.transcript();
    EXPECT_FALSE(fs::exists(evidence)) << "legacy reuse cannot invent an empty dependency closure";
    const auto migrated =
        home.xlings({"install", "fixture:resolution-leaf@1.0.0", "--reconfig", "-y"});
    ASSERT_EQ(migrated.exit_code, 0) << migrated.transcript();
    ASSERT_TRUE(fs::is_regular_file(evidence));
    EXPECT_EQ(nlohmann::json::parse(tk::read_file(evidence)).at("deps"), nlohmann::json::array());

    ASSERT_TRUE(fs::remove(evidence));
    ASSERT_TRUE(fs::create_directory(evidence));
    tk::write_file(evidence / "sentinel", "must survive failed recording");
    const auto refused =
        home.xlings({"install", "fixture:resolution-leaf@1.0.0", "--reconfig", "-y"});
    EXPECT_NE(refused.exit_code, 0) << refused.transcript();
    EXPECT_NE(refused.transcript().find("resolution record is not a regular file"),
              std::string::npos);
    EXPECT_EQ(tk::read_file(evidence / "sentinel"), "must survive failed recording");
}

XTEST(EntryActivationE2E, ARecipeCannotClaimTheSharedEntryInAFreshHome, .area = "xim",
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("entry-activation-forgery");
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs" / "f" / "forged.lua", R"LUA(
        package = {
            spec = '1', name = 'forged', type = 'package', archs = {'x86_64','aarch64'},
            xpm = {linux={['1.0.0']={}}, macosx={['1.0.0']={}}, windows={['1.0.0']={}}}
        }
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            os.mkdir(pkginfo.install_dir())
            io.writefile(path.join(pkginfo.install_dir(), 'xlings'), 'fake shared client')
            return true
        end
        function config()
            xvm.add('xlings', {bindir=pkginfo.install_dir()})
            return true
        end
    )LUA");
    tk::write_file(home.dir() / ".xlings.json", local_indexes(repo).dump());
    const auto entry = home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings");
    fs::create_directories(entry.parent_path());
    fs::copy_file(tk::xlings_binary(), entry);
    const auto initialized = home.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();
    const auto before = xlings::sha256::hex_file(entry);
    ASSERT_TRUE(before);
    const auto result = home.xlings({"install", "fixture:forged@1.0.0", "-y"});
    EXPECT_NE(result.exit_code, 0) << result.transcript();
    EXPECT_NE(result.transcript().find("only the xlings package"), std::string::npos)
        << result.transcript();
    EXPECT_EQ(xlings::sha256::hex_file(entry), before);
    EXPECT_EQ(home.xlings({"--version"}).exit_code, 0);
}

XTEST(HomeLayerInstall, BorrowedPayloadUsesTheCurrentScopeAndAReadonlyWorker, .area = "xim",
      .covers = {"HOME-LAYER-RESOLVE"}, .requires_ = {"linux", "xlings-bin", "sandbox"},
      .resources = {"sandbox"}, .proves = "isolation") {
    using Json = nlohmann::json;
    auto user = tk::Home::isolated("borrowed-install");
    const auto system = user.root() / "system";
    const auto payload = system / "data/xpkgs/fixture-x-layer-recipe/1.0.0";
    tk::write_file(payload / "bin/layer-command", "system payload sentinel");
    tk::write_file(payload / ".xlings-resolution.json",
                   Json{{"package", "fixture:layer-recipe@1.0.0"}, {"deps", Json::array()}}.dump());
    tk::write_file(system / ".xlings-home",
                   R"({"schema":1,"id":"system","mode":"multi","layout":1})");
    Json sourceVersions = {{"layer-command",
                            {{"type", "program"},
                             {"versions",
                              {{"fixture:1.0.0",
                                {{"path", (payload / "bin").generic_string()},
                                 {"alias", {"echo source-config"}}}}}}}}};
    tk::write_file(system / ".xlings.json", Json{{"versions", sourceVersions}}.dump());
    tk::write_file(
        system / "subos/default/.xlings.json",
        Json{{"workspace",
              {{"layer-command", {{"active", "fixture:1.0.0"}, {"installed", {"fixture:1.0.0"}}}}}},
             {"configured", {{"fixture:layer-recipe@1.0.0", 99}}}}
            .dump());
    const auto sourceState = tk::read_file(system / ".xlings.json");
    const auto sourceScope = tk::read_file(system / "subos/default/.xlings.json");
    const auto repo = user.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    const auto forbidden = payload / "host-write";
    tk::write_file(repo / "pkgs/l/layer-recipe.lua",
                   std::format(R"LUA(
        local top = io.open({0}, 'w')
        if top then top:write('host top-level escaped'); top:close() end
        package = {{spec='1',name='layer-recipe',type='package',archs={{'x86_64','aarch64'}},
            xpm={{linux={{['1.0.0']={{}}}}}}}}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install() error('a borrowed payload must never install') end
        function config()
            local f = io.open(path.join(pkginfo.install_dir(), 'hook-write'), 'w')
            assert(not f, 'system payload was writable')
            xvm.add('layer-command', {{bindir=path.join(pkginfo.install_dir(),'bin'),
                                     alias='echo current-scope-config'}})
            return true
        end
    )LUA",
                               Json(forbidden.generic_string()).dump()));
    tk::write_file(
        user.dir() / ".xlings.json",
        local_indexes(repo).dump());
    const auto entry = user.dir() / "bin/xlings";
    fs::create_directories(entry.parent_path());
    fs::copy_file(tk::xlings_binary(), entry);
    ASSERT_EQ(user.xlings({"self", "init"}).exit_code, 0);
    const auto result = user.xlings({"install", "fixture:layer-recipe@1.0.0", "-y"},
                                    {{"XLINGS_SYSTEM_LAYER", system.generic_string()}});
    ASSERT_EQ(result.exit_code, 0) << result.transcript();
    EXPECT_FALSE(fs::exists(user.dir() / "data/xpkgs/fixture-x-layer-recipe/1.0.0"));
    EXPECT_FALSE(fs::exists(forbidden));
    EXPECT_FALSE(fs::exists(payload / "hook-write"));
    EXPECT_EQ(tk::read_file(payload / "bin/layer-command"), "system payload sentinel");
    EXPECT_EQ(tk::read_file(system / ".xlings.json"), sourceState);
    EXPECT_EQ(tk::read_file(system / "subos/default/.xlings.json"), sourceScope);
    const auto state = Json::parse(tk::read_file(user.dir() / ".xlings.json"));
    const auto& data = state.at("versions").at("layer-command").at("versions").at("fixture:1.0.0");
    EXPECT_EQ(data.at("layer").at("home").get<std::string>(), fs::canonical(system).string());
    EXPECT_NE(data.at("alias").dump().find("current-scope-config"), std::string::npos);
    const auto scope = Json::parse(tk::read_file(user.dir() / "subos/default/.xlings.json"));
    EXPECT_EQ(scope.at("configured").at("fixture:layer-recipe@1.0.0").get<int>(), 0);
}

XTEST(InstallerMaterialization, AUserFileRefusesTheWholeRegistrationBeforeMetadataChanges,
      .area = "xim", .requires_ = {"xlings-bin"}) {
    using Json = nlohmann::json;
    auto home = tk::Home::isolated("installer-user-file");
    const auto repo = home.root() / "index";
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/c/collision.lua", R"LUA(
        package = {spec='1',name='collision',type='package',archs={'x86_64','aarch64'},
            xpm={linux={['1.0.0']={}},macosx={['1.0.0']={}},windows={['1.0.0']={}}}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install()
            os.mkdir(pkginfo.install_dir())
            io.writefile(path.join(pkginfo.install_dir(),'asset'), 'derived content')
            return true
        end
        function config()
            xvm.add('collision-asset', {type='files',src='asset',dst='usr/share/collision'})
            return true
        end
    )LUA");
    tk::write_file(
        home.dir() / ".xlings.json",
        local_indexes(repo).dump());
    const auto entry = home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings");
    fs::create_directories(entry.parent_path());
    fs::copy_file(tk::xlings_binary(), entry);
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto sentinel = home.dir() / "subos/default/usr/share/collision";
    tk::write_file(sentinel, "user-owned content");
    const auto versionsBefore =
        Json::parse(tk::read_file(home.dir() / ".xlings.json")).value("versions", Json::object());
    const auto workspaceBefore = tk::read_file(home.dir() / "subos/default/.xlings.json");
    const auto result = home.xlings({"install", "fixture:collision@1.0.0", "-y"});
    EXPECT_NE(result.exit_code, 0) << result.transcript();
    EXPECT_EQ(tk::read_file(sentinel), "user-owned content");
    EXPECT_EQ(
        Json::parse(tk::read_file(home.dir() / ".xlings.json")).value("versions", Json::object()),
        versionsBefore);
    EXPECT_EQ(tk::read_file(home.dir() / "subos/default/.xlings.json"), workspaceBefore);
}
