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
std::expected<mcpplibs::xpkg::PackageExecutor, std::string>
create_executor(const std::filesystem::path& package, mcpplibs::xpkg::ExecutionContext& context,
                const std::vector<std::filesystem::path>& hook_logs = {},
                bool readonly_payload = false);
int worker_main(int argc, char* argv[]);
} // namespace xlings::xim::lua_boundary
