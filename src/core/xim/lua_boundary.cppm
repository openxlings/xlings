export module xlings.core.xim.lua_boundary;
import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.executor;
export namespace xlings::xim::lua_boundary {
std::expected<mcpplibs::xpkg::Package, std::string>
metadata(const std::filesystem::path& package, const mcpplibs::xpkg::LoaderContext& context,
         const std::string& formal_provider = {});
std::expected<mcpplibs::xpkg::PackageIndex, std::string>
build_index(const std::filesystem::path& repo, const std::string& default_namespace,
            const mcpplibs::xpkg::LoaderContext& context,
            const mcpplibs::xpkg::BuildOutput& output);
// A hook's executor: in this process, or in a Lua worker when the active SubOS
// has a policy, the payload is borrowed, or this root lacks the runtime at
// its standard loader path (design 2026-11-21-issue-724 §4.1). `runtime_note`:
// what a failed hook's message adds about that runtime (empty: nothing).
std::expected<mcpplibs::xpkg::PackageExecutor, std::string>
create_executor(const std::filesystem::path& package, mcpplibs::xpkg::ExecutionContext& context,
                const std::vector<std::filesystem::path>& hook_logs = {},
                bool readonly_payload = false, std::string* runtime_note = nullptr);
int worker_main(int argc, char* argv[]);
} // namespace xlings::xim::lua_boundary
