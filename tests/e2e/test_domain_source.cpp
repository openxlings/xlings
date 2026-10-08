#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer;
import xlings.core.elfread;
import xlings.libs.json;
import xlings.subos.home_view;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace domain = xlings::home::prefix_domain;
namespace producer = xlings::home::domain_producer;
using Json = nlohmann::json;

XTEST(DomainSourceProducer, ReusesOnlyCheckedReadonlyBytesAndExportsAnOwnedImage,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"DOM-BUILD-INSIDE", "DOM-PREFIX", "DOM-LAYOUTS", "DOM-SOURCE", "HOME-LAYER-RESOLVE"},
      .requires_ = {"linux", "bwrap", "xlings-bin"}, .resources = {"sandbox"}, .proves = "isolation") {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux namespaces";
    if (const auto why = tk::probe("bwrap")) GTEST_SKIP() << *why;
    const auto executable = xlings::elfread::read(tk::xlings_binary());
    if (!executable || !executable->interpreter.empty()) GTEST_SKIP() << "image export requires the static release binary";
    auto owner = tk::Home::isolated("domain-source-producer");
    const auto source = owner.root() / "source";
    const auto payload = source / "data/xpkgs/fixture-x-domain-source/1.0.0";
    const auto coordinate = "fixture:1.0.0";
    tk::write_file(payload / "bin/source-member", "#!/bin/sh\nexit 0\n");
    tk::write_file(payload / ".xlings-resolution.json", Json{{"package", "fixture:domain-source@1.0.0"},
        {"deps", Json::array()}, {"future", "unknown record remains"}}.dump());
    Json workspace{{"source-member", {{"active", coordinate}, {"installed", Json::array({coordinate})}}}};
    tk::write_file(payload / ".xlings.json", Json{{"workspace", workspace}, {"subos_kind", "rootfs"}}.dump());
    tk::write_file(source / ".xlings-home", R"({"schema":1,"mode":"root","layout":"multi","future":42})");
    const auto physicalSource = fs::canonical(source);
    Json version{{"path", (physicalSource / "data/xpkgs/fixture-x-domain-source/1.0.0/bin").generic_string()},
        {"kind", "program"}, {"alias", Json::array({"/xlings/data/xpkgs/fixture-x-domain-source/1.0.0/bin/source-member"})}};
    tk::write_file(source / ".xlings.json", Json{{"versions", {
        {"source-member", {{"type", "program"}, {"versions", {{coordinate, version}}}}}}}, {"future", 77}}.dump());
    tk::write_file(source / "subos/default/.xlings.json", Json{{"workspace", workspace}}.dump());
    const auto sourcePrimary = tk::read_file(source / ".xlings.json");
    const auto sourceScope = tk::read_file(source / "subos/default/.xlings.json");
    const auto sourceEvidence = tk::read_file(payload / ".xlings-resolution.json");
    const auto repo = owner.root() / "index";
    const auto primary = owner.root() / "primary";
    tk::write_file(primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(primary / "pkgs");
    tk::write_file(repo / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(repo / "pkgs/d/domain-source.lua", R"LUA(
        local proc = assert(io.open('/proc/self/status', 'r'))
        local identity = proc:read('*a'); proc:close()
        assert(identity:match('Uid:%s+(%d+)') == '1000', 'source worker lost the mapped owner identity')
        assert(identity:match('CapEff:%s+([%x]+)'):match('^0+$'), 'source worker gained capabilities')
        package = {spec='1', name='domain-source', type='subos', archs={'x86_64','aarch64'},
            xpm={linux={['1.0.0']={}}}}
        import('xim.libxpkg.pkginfo')
        import('xim.libxpkg.xvm')
        function install() error('a checked system payload must not be installed again') end
        function config()
            local dir = pkginfo.install_dir()
            assert(dir == '/run/xlings-system-source/data/xpkgs/fixture-x-domain-source/1.0.0')
            local f = io.open(path.join(dir, 'hook-write'), 'w')
            assert(not f, 'system payload is writable')
            xvm.add('source-member', {bindir=path.join(dir, 'bin'),
                alias='/xlings/data/xpkgs/fixture-x-domain-source/1.0.0/bin/source-member'})
            return true
        end
    )LUA");
    tk::write_file(owner.dir() / ".xlings.json", Json{{"mirror", "GLOBAL"}, {"xim", {{"index-repo", primary.generic_string()}}},
        {"index_repos", Json::array({{{"name", "xim"}, {"url", primary.generic_string()}, {"source", "git"}},
                                    {{"name", "fixture"}, {"url", repo.generic_string()}, {"source", "git"}}})}}.dump());
    auto selected = domain::resolve(owner.dir(), "/xlings", physicalSource);
    ASSERT_TRUE(selected) << selected.error();
    auto prepared = producer::prepare(*selected, tk::xlings_binary());
    ASSERT_TRUE(prepared) << prepared.error();
    // run() refreshes from the calling CLI image; this caller is a GTest
    // program. Select the candidate explicitly and execute the same command.
    int operation { 0 };
    auto runProducer = [&](std::span<const std::string> arguments,
                           std::optional<producer::OutputBinding> output = std::nullopt)
        -> std::expected<tk::RunResult, std::string> {
        auto refreshed = producer::refresh_entry(*selected, tk::xlings_binary());
        if (!refreshed) return std::unexpected(refreshed.error());
        const auto stage = owner.root() / ("source-view-" + std::to_string(operation++));
        std::error_code ec;
        if (!fs::create_directory(stage, ec) || ec)
            return std::unexpected("cannot reserve fixture source facade " + stage.string() + ": " + ec.message());
        auto facade = xlings::home::domain_producer_source::prepare(*selected, stage);
        if (!facade) return std::unexpected(facade.error());
        auto command = producer::command(*selected, arguments, output, &*facade);
        if (!command) return std::unexpected(command.error());
        return tk::run({.argv = std::move(*command), .env = owner.env(), .cwd = owner.root(),
                        .timeout = std::chrono::seconds(120)});
    };
    const std::vector<std::string> initialize{"self", "init"};
    auto initialized = runProducer(initialize);
    ASSERT_TRUE(initialized) << initialized.error();
    ASSERT_FALSE(initialized->timed_out) << initialized->transcript();
    ASSERT_EQ(initialized->exit_code, 0) << "signal=" << initialized->signal << '\n' << initialized->transcript();
    const std::vector<std::string> create{"subos", "new", "source-root", "--rootfs", "--from", "fixture:domain-source@1.0.0"};
    auto created = runProducer(create);
    ASSERT_TRUE(created) << created.error();
    ASSERT_FALSE(created->timed_out) << created->transcript();
    ASSERT_EQ(created->exit_code, 0) << "signal=" << created->signal << '\n' << created->transcript();
    ASSERT_TRUE(producer::publish_scope(*selected, "source-root"));
    const auto copiedSlot = selected->physicalHome / "data/xpkgs/fixture-x-domain-source/1.0.0";
    EXPECT_TRUE(fs::is_empty(copiedSlot)) << "logical mountpoints hold no copied system payload";
    const auto privateConfig = Json::parse(tk::read_file(selected->physicalHome / ".xlings.json"));
    const auto& borrowed = privateConfig["versions"]["source-member"]["versions"][coordinate];
    EXPECT_EQ(borrowed["layer"]["home"], physicalSource.generic_string());
    EXPECT_EQ(borrowed["path"], "/run/xlings-system-source/data/xpkgs/fixture-x-domain-source/1.0.0/bin");
    auto status = owner.xlings({"subos", "status", "source-root", "--json"});
    ASSERT_EQ(status.exit_code, 0) << status.transcript();
    EXPECT_EQ(Json::parse(status.out)["root"]["kind"], "rootfs");
    const auto output = owner.root() / "export";
    fs::create_directory(output);
    const std::vector<std::string> exportArgs{"subos", "export", "source-root", "--rootfs", "/run/xlings-domain-output/rootfs"};
    auto exported = runProducer(exportArgs, producer::OutputBinding{output, "/run/xlings-domain-output"});
    ASSERT_TRUE(exported) << exported.error();
    ASSERT_FALSE(exported->timed_out) << exported->transcript();
    ASSERT_EQ(exported->exit_code, 0) << "signal=" << exported->signal << '\n' << exported->transcript();
    const auto imageHome = output / "rootfs/xlings";
    EXPECT_EQ(tk::read_file(imageHome / "data/xpkgs/fixture-x-domain-source/1.0.0/bin/source-member"), "#!/bin/sh\nexit 0\n");
    const auto imageConfig = Json::parse(tk::read_file(imageHome / ".xlings.json"));
    const auto& imageVersion = imageConfig["versions"]["source-member"]["versions"][coordinate];
    EXPECT_EQ(imageVersion["path"], "/xlings/data/xpkgs/fixture-x-domain-source/1.0.0/bin");
    EXPECT_FALSE(imageVersion.contains("layer"));
    EXPECT_EQ(Json::parse(tk::read_file(imageHome / "data/xpkgs/fixture-x-domain-source/1.0.0/.xlings-resolution.json"))["future"],
              "unknown record remains");
    EXPECT_FALSE(fs::exists(payload / "hook-write"));
    EXPECT_EQ(tk::read_file(source / ".xlings.json"), sourcePrimary);
    EXPECT_EQ(tk::read_file(source / "subos/default/.xlings.json"), sourceScope);
    EXPECT_EQ(tk::read_file(payload / ".xlings-resolution.json"), sourceEvidence);
}

XTEST(RootExport, OwnsBorrowedClosureAndOmitsOtherScopeVersions,
      .area = "subos", .cost = tk::Cost::Medium,
      .covers = {"HOME-LAYER-RESOLVE"},
      .requires_ = {"linux", "xlings-bin"}) {
    if constexpr (!tk::is_linux) GTEST_SKIP() << "Linux root export";
    const auto executable = xlings::elfread::read(tk::xlings_binary());
    if (!executable || !executable->interpreter.empty()) GTEST_SKIP() << "image export requires the static release binary";
    auto owner = tk::Home::isolated("mixed-source-export");
    fs::create_directories(owner.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), owner.dir() / "bin/xlings");
    auto initialized = owner.xlings({"self", "init"});
    ASSERT_EQ(initialized.exit_code, 0) << initialized.transcript();
    auto created = owner.xlings({"subos", "new", "mixed-root", "--rootfs"});
    ASSERT_EQ(created.exit_code, 0) << created.transcript();
    const auto source = owner.root() / "system";
    const auto payload = source / "data/xpkgs/fixture-x-visible/1.0.0";
    const auto excluded = source / "data/xpkgs/fixture-x-visible/2.0.0";
    tk::write_file(payload / "bin/visible", "borrowed program bytes");
    tk::write_file(excluded / "bin/visible", "another scope's version");
    tk::write_file(payload / ".xlings-resolution.json",
                   R"({"package":"fixture:visible@1.0.0","deps":[],"future":42})");
    tk::write_file(source / ".xlings-home", R"({"mode":"root","layout":"multi"})");
    Json versions{{"visible", {{"type", "program"}, {"versions", {
        {"fixture:1.0.0", {{"path", (payload / "bin").generic_string()}}},
        {"fixture:2.0.0", {{"path", (excluded / "bin").generic_string()}}}}}}}};
    Json workspace{{"visible", {{"active", "fixture:1.0.0"}, {"installed", Json::array({"fixture:1.0.0"})}}}};
    tk::write_file(source / ".xlings.json", Json{{"versions", versions}}.dump());
    tk::write_file(source / "subos/default/.xlings.json", Json{{"workspace", workspace}}.dump());
    auto configuration = Json::parse(tk::read_file(owner.dir() / ".xlings.json"));
    versions["visible"]["versions"]["fixture:1.0.0"]["layer"] = {{"home", source.generic_string()}, {"scope", "default"}};
    configuration["versions"] = versions;
    tk::write_file(owner.dir() / ".xlings.json", configuration.dump());
    const auto scopeFile = owner.dir() / "subos/mixed-root/.xlings.json";
    auto scope = Json::parse(tk::read_file(scopeFile));
    scope["workspace"] = workspace;
    tk::write_file(scopeFile, scope.dump());
    const auto sourceBefore = tk::read_file(source / ".xlings.json");
    const auto output = owner.root() / "image";
    auto exported = owner.xlings({"subos", "export", "mixed-root", "--rootfs", output.string()});
    ASSERT_EQ(exported.exit_code, 0) << exported.transcript();
    const auto imageHome = output / owner.dir().relative_path();
    const auto image = Json::parse(tk::read_file(imageHome / ".xlings.json"));
    const auto& imageVersions = image["versions"]["visible"]["versions"];
    ASSERT_EQ(imageVersions.size(), 1u);
    EXPECT_FALSE(imageVersions.contains("fixture:2.0.0"));
    EXPECT_FALSE(imageVersions["fixture:1.0.0"].contains("layer"));
    EXPECT_EQ(imageVersions["fixture:1.0.0"]["path"],
              (owner.dir() / "data/xpkgs/fixture-x-visible/1.0.0/bin").generic_string());
    EXPECT_EQ(tk::read_file(imageHome / "data/xpkgs/fixture-x-visible/1.0.0/bin/visible"), "borrowed program bytes");
    EXPECT_FALSE(fs::exists(imageHome / "data/xpkgs/fixture-x-visible/2.0.0"));
    EXPECT_EQ(fs::read_symlink(output / payload.relative_path()),
              owner.dir() / "data/xpkgs/fixture-x-visible/1.0.0");
    EXPECT_EQ(Json::parse(tk::read_file(imageHome / "data/xpkgs/fixture-x-visible/1.0.0/.xlings-resolution.json"))["future"], 42);
    EXPECT_EQ(tk::read_file(source / ".xlings.json"), sourceBefore);
    EXPECT_EQ(tk::read_file(excluded / "bin/visible"), "another scope's version");
}
