module xlings.core.xim.lua_protocol;
import std;
import xlings.libs.json;
import mcpplibs.xpkg;
import mcpplibs.xpkg.executor;
namespace xlings::xim::lua_protocol {
namespace xpkg = mcpplibs::xpkg;
using Json = nlohmann::json;
namespace {
Json pack(const xpkg::ArchResource& v) {
    Json j = Json::object();
    j["url"] = v.url;
    j["sha256"] = v.sha256;
    j["mirrors"] = v.mirrors;
    return j;
}
xpkg::ArchResource unpack_ArchResource(const Json& j) {
    xpkg::ArchResource v;
    j.at("url").get_to(v.url);
    j.at("sha256").get_to(v.sha256);
    j.at("mirrors").get_to(v.mirrors);
    return v;
}
Json pack(const xpkg::PlatformResource& v) {
    Json j = Json::object();
    j["url"] = v.url;
    j["sha256"] = v.sha256;
    j["ref"] = v.ref;
    j["mirrors"] = v.mirrors;
    j["sha256_by_arch"] = v.sha256_by_arch;
    j["arch_alias"] = v.arch_alias;
    j["is_res"] = v.is_res;
    j["revision"] = v.revision;
    j["archs"] = Json::object();
    for (const auto& [k, item] : v.archs)
        j["archs"][k] = pack(item);
    return j;
}
xpkg::PlatformResource unpack_PlatformResource(const Json& j) {
    xpkg::PlatformResource v;
    j.at("url").get_to(v.url);
    j.at("sha256").get_to(v.sha256);
    j.at("ref").get_to(v.ref);
    j.at("mirrors").get_to(v.mirrors);
    j.at("sha256_by_arch").get_to(v.sha256_by_arch);
    j.at("arch_alias").get_to(v.arch_alias);
    j.at("is_res").get_to(v.is_res);
    j.at("revision").get_to(v.revision);
    for (auto it = j.at("archs").begin(); it != j.at("archs").end(); ++it)
        v.archs.emplace(it.key(), unpack_ArchResource(it.value()));
    return v;
}
Json pack(const xpkg::ExportsRuntime& v) {
    Json j = Json::object();
    j["loader"] = v.loader;
    j["libdirs"] = v.libdirs;
    j["abi"] = v.abi;
    return j;
}
xpkg::ExportsRuntime unpack_ExportsRuntime(const Json& j) {
    xpkg::ExportsRuntime v;
    j.at("loader").get_to(v.loader);
    j.at("libdirs").get_to(v.libdirs);
    j.at("abi").get_to(v.abi);
    return v;
}
Json pack(const xpkg::ExportsBlock& v) {
    Json j = Json::object();
    j["runtime"] = pack(v.runtime);
    return j;
}
xpkg::ExportsBlock unpack_ExportsBlock(const Json& j) {
    xpkg::ExportsBlock v;
    v.runtime = unpack_ExportsRuntime(j.at("runtime"));
    return v;
}
Json pack(const xpkg::PlatformMatrix& v) {
    Json j = Json::object();
    j["source"] = v.source;
    j["source_mirrors"] = v.source_mirrors;
    j["platform_sources"] = v.platform_sources;
    j["platform_source_mirrors"] = v.platform_source_mirrors;
    j["deps"] = v.deps;
    j["runtime_deps"] = v.runtime_deps;
    j["build_deps"] = v.build_deps;
    j["inherits"] = v.inherits;
    j["entries"] = Json::object();
    for (const auto& [k, items] : v.entries) {
        j["entries"][k] = Json::object();
        for (const auto& [key, item] : items)
            j["entries"][k][key] = pack(item);
    }
    j["exports"] = Json::object();
    for (const auto& [k, item] : v.exports)
        j["exports"][k] = pack(item);
    return j;
}
xpkg::PlatformMatrix unpack_PlatformMatrix(const Json& j) {
    xpkg::PlatformMatrix v;
    j.at("source").get_to(v.source);
    j.at("source_mirrors").get_to(v.source_mirrors);
    j.at("platform_sources").get_to(v.platform_sources);
    j.at("platform_source_mirrors").get_to(v.platform_source_mirrors);
    j.at("deps").get_to(v.deps);
    j.at("runtime_deps").get_to(v.runtime_deps);
    j.at("build_deps").get_to(v.build_deps);
    j.at("inherits").get_to(v.inherits);
    for (auto it = j.at("entries").begin(); it != j.at("entries").end(); ++it)
        for (auto entry = it.value().begin(); entry != it.value().end(); ++entry)
            v.entries[it.key()].emplace(entry.key(), unpack_PlatformResource(entry.value()));
    for (auto it = j.at("exports").begin(); it != j.at("exports").end(); ++it)
        v.exports.emplace(it.key(), unpack_ExportsBlock(it.value()));
    return v;
}
Json pack(const xpkg::Package& v) {
    Json j = Json::object();
    j["spec"] = v.spec;
    j["name"] = v.name;
    j["description"] = v.description;
    j["namespace_"] = v.namespace_;
    j["homepage"] = v.homepage;
    j["repo"] = v.repo;
    j["docs"] = v.docs;
    j["authors"] = v.authors;
    j["maintainers"] = v.maintainers;
    j["licenses"] = v.licenses;
    j["categories"] = v.categories;
    j["keywords"] = v.keywords;
    j["programs"] = v.programs;
    j["archs"] = v.archs;
    j["xvm_enable"] = v.xvm_enable;
    j["xpm"] = pack(v.xpm);
    return j;
}
xpkg::Package unpack_Package(const Json& j) {
    xpkg::Package v;
    j.at("spec").get_to(v.spec);
    j.at("name").get_to(v.name);
    j.at("description").get_to(v.description);
    j.at("namespace_").get_to(v.namespace_);
    j.at("homepage").get_to(v.homepage);
    j.at("repo").get_to(v.repo);
    j.at("docs").get_to(v.docs);
    j.at("authors").get_to(v.authors);
    j.at("maintainers").get_to(v.maintainers);
    j.at("licenses").get_to(v.licenses);
    j.at("categories").get_to(v.categories);
    j.at("keywords").get_to(v.keywords);
    j.at("programs").get_to(v.programs);
    j.at("archs").get_to(v.archs);
    j.at("xvm_enable").get_to(v.xvm_enable);
    v.xpm = unpack_PlatformMatrix(j.at("xpm"));
    return v;
}
Json pack(const xpkg::DepExport& v) {
    Json j = Json::object();
    j["loader"] = v.loader;
    j["libdirs"] = v.libdirs;
    j["abi"] = v.abi;
    return j;
}
xpkg::DepExport unpack_DepExport(const Json& j) {
    xpkg::DepExport v;
    j.at("loader").get_to(v.loader);
    j.at("libdirs").get_to(v.libdirs);
    j.at("abi").get_to(v.abi);
    return v;
}
Json pack(const xpkg::ResolvedDep& v) {
    Json j = Json::object();
    j["spec"] = v.spec;
    j["name"] = v.name;
    j["version"] = v.version;
    j["install_dir"] = v.install_dir;
    j["libdirs"] = v.libdirs;
    j["source"] = v.source;
    return j;
}
xpkg::ResolvedDep unpack_ResolvedDep(const Json& j) {
    xpkg::ResolvedDep v;
    j.at("spec").get_to(v.spec);
    j.at("name").get_to(v.name);
    j.at("version").get_to(v.version);
    j.at("install_dir").get_to(v.install_dir);
    j.at("libdirs").get_to(v.libdirs);
    j.at("source").get_to(v.source);
    return v;
}
Json pack(const xpkg::ExecutionContext& v) {
    Json j = Json::object();
    j["pkg_name"] = v.pkg_name;
    j["version"] = v.version;
    j["platform"] = v.platform;
    j["arch"] = v.arch;
    j["deps_list"] = v.deps_list;
    j["args"] = v.args;
    j["runtime_deps_list"] = v.runtime_deps_list;
    j["build_deps_list"] = v.build_deps_list;
    j["subos_sysrootdir"] = v.subos_sysrootdir;
    j["pkgindex_dir"] = v.pkgindex_dir;
    j["install_file"] = v.install_file.generic_string();
    j["install_dir"] = v.install_dir.generic_string();
    j["run_dir"] = v.run_dir.generic_string();
    j["xpkg_dir"] = v.xpkg_dir.generic_string();
    j["bin_dir"] = v.bin_dir.generic_string();
    j["project_data_dir"] = v.project_data_dir.generic_string();
    j["hook_log"] = v.hook_log.generic_string();
    j["deps_exports"] = Json::object();
    for (const auto& [k, item] : v.deps_exports)
        j["deps_exports"][k] = pack(item);
    j["resolved_deps"] = Json::object();
    for (const auto& [k, item] : v.resolved_deps)
        j["resolved_deps"][k] = pack(item);
    j["dependency_store_roots"] = Json::array();
    for (const auto& p : v.dependency_store_roots)
        j["dependency_store_roots"].push_back(p.generic_string());
    j["self_exports"] = pack(v.self_exports);
    return j;
}
xpkg::ExecutionContext unpack_ExecutionContext(const Json& j) {
    xpkg::ExecutionContext v;
    j.at("pkg_name").get_to(v.pkg_name);
    j.at("version").get_to(v.version);
    j.at("platform").get_to(v.platform);
    j.at("arch").get_to(v.arch);
    j.at("deps_list").get_to(v.deps_list);
    j.at("args").get_to(v.args);
    j.at("runtime_deps_list").get_to(v.runtime_deps_list);
    j.at("build_deps_list").get_to(v.build_deps_list);
    j.at("subos_sysrootdir").get_to(v.subos_sysrootdir);
    j.at("pkgindex_dir").get_to(v.pkgindex_dir);
    v.install_file = std::filesystem::path(j.at("install_file").get<std::string>());
    v.install_dir = std::filesystem::path(j.at("install_dir").get<std::string>());
    v.run_dir = std::filesystem::path(j.at("run_dir").get<std::string>());
    v.xpkg_dir = std::filesystem::path(j.at("xpkg_dir").get<std::string>());
    v.bin_dir = std::filesystem::path(j.at("bin_dir").get<std::string>());
    v.project_data_dir = std::filesystem::path(j.at("project_data_dir").get<std::string>());
    v.hook_log = std::filesystem::path(j.at("hook_log").get<std::string>());
    for (auto it = j.at("deps_exports").begin(); it != j.at("deps_exports").end(); ++it)
        v.deps_exports.emplace(it.key(), unpack_DepExport(it.value()));
    for (auto it = j.at("resolved_deps").begin(); it != j.at("resolved_deps").end(); ++it)
        v.resolved_deps.emplace(it.key(), unpack_ResolvedDep(it.value()));
    for (const auto& p : j.at("dependency_store_roots"))
        v.dependency_store_roots.emplace_back(p.get<std::string>());
    v.self_exports = unpack_DepExport(j.at("self_exports"));
    return v;
}
Json pack(const xpkg::XvmOp& v) {
    Json j = Json::object();
    j["op"] = v.op;
    j["name"] = v.name;
    j["version"] = v.version;
    j["bindir"] = v.bindir;
    j["alias"] = v.alias;
    j["type"] = v.type;
    j["filename"] = v.filename;
    j["binding"] = v.binding;
    j["includedir"] = v.includedir;
    j["src"] = v.src;
    j["dst"] = v.dst;
    j["args"] = v.args;
    j["var"] = v.var;
    j["value"] = v.value;
    j["mode"] = v.mode;
    j["envs"] = v.envs;
    return j;
}
xpkg::XvmOp unpack_XvmOp(const Json& j) {
    xpkg::XvmOp v;
    j.at("op").get_to(v.op);
    j.at("name").get_to(v.name);
    j.at("version").get_to(v.version);
    j.at("bindir").get_to(v.bindir);
    j.at("alias").get_to(v.alias);
    j.at("type").get_to(v.type);
    j.at("filename").get_to(v.filename);
    j.at("binding").get_to(v.binding);
    j.at("includedir").get_to(v.includedir);
    j.at("src").get_to(v.src);
    j.at("dst").get_to(v.dst);
    j.at("args").get_to(v.args);
    j.at("var").get_to(v.var);
    j.at("value").get_to(v.value);
    j.at("mode").get_to(v.mode);
    j.at("envs").get_to(v.envs);
    return v;
}
Json pack(const xpkg::InstallRequest& v) {
    Json j = Json::object();
    j["op"] = v.op;
    j["target"] = v.target;
    return j;
}
xpkg::InstallRequest unpack_InstallRequest(const Json& j) {
    xpkg::InstallRequest v;
    j.at("op").get_to(v.op);
    j.at("target").get_to(v.target);
    return v;
}
Json pack(const xpkg::HookResult& v) {
    Json j = Json::object();
    j["success"] = v.success;
    j["output"] = v.output;
    j["error"] = v.error;
    j["version"] = v.version;
    return j;
}
xpkg::HookResult unpack_HookResult(const Json& j) {
    xpkg::HookResult v;
    j.at("success").get_to(v.success);
    j.at("output").get_to(v.output);
    j.at("error").get_to(v.error);
    j.at("version").get_to(v.version);
    return v;
}
} // namespace
Json encode(const xpkg::Package& v) {
    auto j = pack(v);
    j["type"] = static_cast<int>(v.type);
    j["status"] = static_cast<int>(v.status);
    return j;
}
xpkg::Package package(const Json& j) {
    auto v = unpack_Package(j);
    const int type = j.at("type").get<int>(), status = j.at("status").get<int>();
    if (type < 0 || type > 4 || status < 0 || status > 2)
        throw std::runtime_error("invalid package enum");
    v.type = static_cast<xpkg::PackageType>(type);
    v.status = static_cast<xpkg::PackageStatus>(status);
    return v;
}
Json encode(const xpkg::HookInvocation& v) {
    return {{"action", std::string(xpkg::hook_action_name(v.action))},
            {"package", v.package.generic_string()},
            {"hook", static_cast<int>(v.hook)},
            {"context", pack(v.context)},
            {"log_level", v.log_level}};
}
xpkg::HookInvocation invocation(const Json& j) {
    auto action = xpkg::hook_action_from_string(j.at("action").get<std::string>());
    int hook = j.at("hook").get<int>();
    if (!action || hook < 0 || hook > 4)
        throw std::runtime_error("invalid hook invocation");
    return {.action = *action,
            .package = j.at("package").get<std::string>(),
            .hook = static_cast<xpkg::HookType>(hook),
            .context = unpack_ExecutionContext(j.at("context")),
            .log_level = j.at("log_level").get<std::string>()};
}
Json encode(const xpkg::HookResponse& v) {
    Json j = {{"result", pack(v.result)},
              {"hooks", v.hooks},
              {"xvm_ops", Json::array()},
              {"install_requests", Json::array()}};
    for (const auto& op : v.xvm_ops)
        j["xvm_ops"].push_back(pack(op));
    for (const auto& req : v.install_requests)
        j["install_requests"].push_back(pack(req));
    return j;
}
xpkg::HookResponse response(const Json& j) {
    xpkg::HookResponse v;
    v.result = unpack_HookResult(j.at("result"));
    j.at("hooks").get_to(v.hooks);
    for (const auto& op : j.at("xvm_ops"))
        v.xvm_ops.push_back(unpack_XvmOp(op));
    for (const auto& req : j.at("install_requests"))
        v.install_requests.push_back(unpack_InstallRequest(req));
    return v;
}
} // namespace xlings::xim::lua_protocol
