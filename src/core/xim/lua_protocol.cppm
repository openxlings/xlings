export module xlings.core.xim.lua_protocol;
import std;
import xlings.libs.json;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
export namespace xlings::xim::lua_protocol {
nlohmann::json encode(const mcpplibs::xpkg::Package& value);
mcpplibs::xpkg::Package package(const nlohmann::json& value);
nlohmann::json encode(const mcpplibs::xpkg::HookInvocation& value);
mcpplibs::xpkg::HookInvocation invocation(const nlohmann::json& value);
nlohmann::json encode(const mcpplibs::xpkg::HookResponse& value);
mcpplibs::xpkg::HookResponse response(const nlohmann::json& value);
} // namespace xlings::xim::lua_protocol
