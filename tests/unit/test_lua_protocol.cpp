#include <gtest/gtest.h>

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
import xlings.core.xim.lua_protocol;
import xlings.libs.json;

namespace xp = mcpplibs::xpkg;
namespace protocol = xlings::xim::lua_protocol;

TEST(LuaProtocol, PreservesArchitectureResourcesAndAllDependencyDomains) {
    xp::Package package;
    package.spec = "2";
    package.name = "tool";
    package.namespace_ = "fixture";
    package.description = "unicode 路径";
    package.type = xp::PackageType::Config;
    package.status = xp::PackageStatus::Stable;
    package.programs = {"tool", "tool-cc"};
    package.archs = {"aarch64"};
    package.xpm.source = "xlings-res";
    package.xpm.source_mirrors = {{"CN", "cn/${version}"}};
    package.xpm.platform_sources = {{"linux", "mirror/${arch}"}};
    package.xpm.platform_source_mirrors = {{"linux", {{"GLOBAL", "global"}}}};
    auto& resource = package.xpm.entries["linux"]["1.2.3"];
    resource.revision = 7;
    resource.is_res = true;
    resource.ref = "stable";
    resource.archs["aarch64"] = {.url = "url", .sha256 = "sha", .mirrors = {{"CN", "cn-url"}}};
    resource.sha256_by_arch = {{"aarch64", "checksum"}};
    resource.arch_alias = {{"aarch64", "arm64"}};
    package.xpm.runtime_deps["linux"] = {"fixture:lib@>=2"};
    package.xpm.build_deps["linux"] = {"host:compiler@4"};
    package.xpm.deps["linux"] = {"fixture:lib@>=2", "host:compiler@4"};
    package.xpm.exports["linux"].runtime = {.loader = "ld", .libdirs = {"lib64"}, .abi = "musl"};
    package.xpm.inherits = {{"ubuntu", "linux"}};
    const auto restored = protocol::package(protocol::encode(package));
    EXPECT_EQ(restored.type, package.type);
    EXPECT_EQ(restored.description, package.description);
    EXPECT_EQ(restored.xpm.entries.at("linux").at("1.2.3").archs.at("aarch64").mirrors.at("CN"),
              "cn-url");
    EXPECT_EQ(restored.xpm.entries.at("linux").at("1.2.3").revision, 7);
    EXPECT_EQ(restored.xpm.entries.at("linux").at("1.2.3").arch_alias.at("aarch64"), "arm64");
    EXPECT_EQ(restored.xpm.platform_source_mirrors.at("linux").at("GLOBAL"), "global");
    EXPECT_EQ(restored.xpm.runtime_deps, package.xpm.runtime_deps);
    EXPECT_EQ(restored.xpm.build_deps, package.xpm.build_deps);
    EXPECT_EQ(restored.xpm.exports.at("linux").runtime.abi, "musl");
    auto malformed = protocol::encode(package);
    malformed.erase("xpm");
    EXPECT_THROW(protocol::package(malformed), nlohmann::json::exception);
}

TEST(LuaProtocol, PreservesResolvedDependenciesLogsAndDeferredScopeEffects) {
    xp::HookInvocation invocation;
    invocation.action = xp::HookAction::RunHook;
    invocation.hook = xp::HookType::Config;
    invocation.package = "recipe.lua";
    invocation.context.args = {"a b", "--credential=secret"};
    invocation.context.install_dir = "payload";
    invocation.context.install_file = "archive.tar.gz";
    invocation.context.hook_log = "only-this-hook.log";
    invocation.context.dependency_store_roots = {"one", "two"};
    invocation.context.deps_exports["lib@>=2"] = {
        .loader = "/store/ld", .libdirs = {"/store/lib"}, .abi = "musl"};
    invocation.context.resolved_deps["lib@>=2"] = {.spec = "lib@>=2",
                                                   .name = "fixture:lib",
                                                   .version = "2.4",
                                                   .install_dir = "/store/lib",
                                                   .libdirs = {"/store/lib/lib64"},
                                                   .source = "plan-range"};
    const auto restored = protocol::invocation(protocol::encode(invocation));
    EXPECT_EQ(restored.context.args, invocation.context.args);
    EXPECT_EQ(restored.context.hook_log, invocation.context.hook_log);
    EXPECT_EQ(restored.context.resolved_deps.at("lib@>=2").source, "plan-range");
    EXPECT_EQ(restored.context.dependency_store_roots, invocation.context.dependency_store_roots);
    xp::HookResponse response;
    response.result = {.success = true, .output = "log output", .version = "2.4"};
    response.hooks[3] = true;
    response.xvm_ops.push_back({.op = "add",
                                .name = "tool",
                                .version = "2.4",
                                .binding = "suite@2.4",
                                .src = "share/data",
                                .dst = "usr/share/data",
                                .args = {"a b"},
                                .envs = {{"MODE", "value"}}});
    response.install_requests = {{.op = "install", .target = "fixture:extra@1"}};
    const auto answer = protocol::response(protocol::encode(response));
    ASSERT_EQ(answer.xvm_ops.size(), 1u);
    EXPECT_EQ(answer.xvm_ops.front().binding, "suite@2.4");
    EXPECT_EQ(answer.xvm_ops.front().args, (std::vector<std::string>{"a b"}));
    EXPECT_EQ(answer.xvm_ops.front().envs, response.xvm_ops.front().envs);
    EXPECT_EQ(answer.install_requests.front().target, "fixture:extra@1");
    auto malformed = protocol::encode(response);
    malformed.erase("install_requests");
    EXPECT_THROW(protocol::response(malformed), nlohmann::json::exception);
}
